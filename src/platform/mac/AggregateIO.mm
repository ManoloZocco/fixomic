#include "platform/mac/AggregateIO.h"

#include "platform/mac/CoreAudioDevices.h"
#include "platform/mac/CoreAudioUtil.h"
#include "platform/mac/FixomicDevices.h"

#import <CoreAudio/AudioHardwareTapping.h>
#import <Foundation/Foundation.h>

#include <juce_audio_basics/juce_audio_basics.h>   // juce::ScopedNoDenormals

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace fixomic::mac {

namespace {

using namespace std::chrono_literals;

constexpr double kRate = devices::kSampleRate;
constexpr int kMinCapacity = 1024;  // frames preallocated at least, whatever the buffer size

// One channel inside the IOProc's AudioBufferList: buffer index and channel within that
// (interleaved) buffer.
struct Slot {
    int buffer = -1;
    int channel = 0;
    bool valid() const { return buffer >= 0; }
};

// What a sub-device or a tap contributes to the aggregate, one entry per stream.
struct StreamShape {
    int channels;
    UInt32 terminalType;
};

struct Block {
    enum class Kind { Mic, Bridge, Other, Tap } kind;
    AudioObjectID object;
    std::vector<StreamShape> streams;
};

std::vector<StreamShape> streamShapes(AudioObjectID device, AudioObjectPropertyScope scope)
{
    std::vector<StreamShape> shapes;
    for (AudioObjectID stream : util::getArray<AudioObjectID>(device, kAudioDevicePropertyStreams, scope)) {
        const auto format = util::getScalar<AudioStreamBasicDescription>(stream, kAudioStreamPropertyVirtualFormat);
        shapes.push_back({format ? static_cast<int>(format->mChannelsPerFrame) : 0,
                          util::getScalar<UInt32>(stream, kAudioStreamPropertyTerminalType).value_or(0)});
    }
    return shapes;
}

bool isFloat32Interleaved(const AudioStreamBasicDescription& f)
{
    return f.mFormatID == kAudioFormatLinearPCM && (f.mFormatFlags & kAudioFormatFlagIsFloat) != 0
           && (f.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0 && f.mBitsPerChannel == 32;
}

bool waitForRate(AudioObjectID device, double rate)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    for (;;) {
        const double current = util::getScalar<Float64>(device, kAudioDevicePropertyNominalSampleRate).value_or(0.0);
        if (std::abs(current - rate) < 0.5)
            return true;
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(10ms);
    }
}

juce::String fourCharCode(UInt32 code)
{
    return util::statusString(static_cast<OSStatus>(code));
}

// Italian text with non-ASCII characters (juce::String(const char*) expects ASCII).
juce::String it(const char* utf8)
{
    return juce::String::fromUTF8(utf8);
}

// «name», as the panel quotes names.
juce::String quoted(const juce::String& name)
{
    return it("\u00ab") + name + it("\u00bb");
}

} // namespace

struct AggregateIO::Impl {
    AggregateIO* owner = nullptr;
    AudioObjectID aggregate = kAudioObjectUnknown;
    juce::String aggregateUid;
    AudioObjectID mic = kAudioObjectUnknown;
    AudioObjectID bridge = kAudioObjectUnknown;
    juce::String micUid;
    AudioDeviceIOProcID procId = nullptr;
    bool running = false;
    Callback callback;

    // Mapping, fixed at create() time. outR is invalid when Bridge has a single channel.
    Slot micSlot, refSlot, outLSlot, outRSlot;
    AudioObjectID micStream = kAudioObjectUnknown;     // aggregate stream objects, for latency
    AudioObjectID bridgeStream = kAudioObjectUnknown;
    juce::String mapping;

    int bufferFrames = 0;
    int capacity = 0;
    std::vector<float> micBuffer, refBuffer, outL, outR;

    // IO thread only.
    double expectedSampleTime = -1.0;

    // Written by the IO thread, read by the main thread.
    std::atomic<std::uint64_t> callbacks{0}, frames{0}, sampleTimeGaps{0};
    std::atomic<std::uint32_t> lastFrames{0}, maxFrames{0};
    // Main queue only.
    std::uint64_t overloads = 0;
    bool lostFired = false;    // delivered to a handler: never again
    bool lostPending = false;  // happened while no handler was set

    std::vector<std::unique_ptr<PropertyWatcher>> watchers;
    juce::String lastError;

    ~Impl()
    {
        if (procId != nullptr) {
            if (running)
                AudioDeviceStop(aggregate, procId);
            AudioDeviceDestroyIOProcID(aggregate, procId);
        }
        watchers.clear();
        // After a Core Audio restart the aggregate is gone and its id may name another device of the
        // new coreaudiod (e.g. the user's own aggregate): destroy it only while it is still ours.
        if (aggregate != kAudioObjectUnknown
            && util::getString(aggregate, kAudioDevicePropertyDeviceUID) == aggregateUid)
            AudioHardwareDestroyAggregateDevice(aggregate);
    }

    void fireLost()
    {
        if (lostFired)
            return;
        // A copy, so the handler may destroy the AggregateIO (and this Impl) while it runs.
        const auto handler = owner->onDeviceLost;
        if (!handler) {
            // Not latched: a handler assigned later hears the next event, and start() refuses.
            lostPending = true;
            return;
        }
        lostFired = true;
        handler();
    }

    static OSStatus ioProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input,
                           const AudioTimeStamp* inputTime, AudioBufferList* output, const AudioTimeStamp*,
                           void* context);
    void process(const AudioBufferList* input, const AudioTimeStamp* inputTime, AudioBufferList* output);
};

// ---- IO thread: no allocation, locks, logging or Objective-C ---------------------------------

OSStatus AggregateIO::Impl::ioProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input,
                                   const AudioTimeStamp* inputTime, AudioBufferList* output, const AudioTimeStamp*,
                                   void* context)
{
    static_cast<Impl*>(context)->process(input, inputTime, output);
    return noErr;
}

void AggregateIO::Impl::process(const AudioBufferList* input, const AudioTimeStamp* inputTime,
                                AudioBufferList* output)
{
    // The whole IO cycle (the Callback included) with denormals flushed to zero; the HAL thread's
    // own mode is restored on return. Saves and restores a CPU register: no allocation.
    const juce::ScopedNoDenormals noDenormals;
    if (output != nullptr)
        for (UInt32 b = 0; b < output->mNumberBuffers; ++b)
            if (output->mBuffers[b].mData != nullptr)
                std::memset(output->mBuffers[b].mData, 0, output->mBuffers[b].mDataByteSize);

    callbacks.fetch_add(1, std::memory_order_relaxed);
    if (input == nullptr || micSlot.buffer >= static_cast<int>(input->mNumberBuffers))
        return;

    // Returns the frame count of `buffer` and its interleave stride; 0 frames if `slot` does not
    // fit the buffer (the layout changed under us: the device-lost path will rebuild).
    auto view = [](const AudioBuffer& buffer, const Slot& slot, int& stride) {
        stride = static_cast<int>(buffer.mNumberChannels);
        if (buffer.mData == nullptr || stride <= slot.channel)
            return 0;
        return static_cast<int>(buffer.mDataByteSize / (sizeof(float) * static_cast<UInt32>(stride)));
    };

    int micStride = 0;
    const AudioBuffer& micBuf = input->mBuffers[micSlot.buffer];
    const int n = view(micBuf, micSlot, micStride);
    const auto* micData = static_cast<const float*>(micBuf.mData);

    int refStride = 0, refFrames = 0;
    const float* refData = nullptr;
    if (refSlot.valid() && refSlot.buffer < static_cast<int>(input->mNumberBuffers)) {
        refFrames = view(input->mBuffers[refSlot.buffer], refSlot, refStride);
        refData = static_cast<const float*>(input->mBuffers[refSlot.buffer].mData);
    }

    int outStride = 0, outFrames = 0;
    float* outData = nullptr;
    if (output != nullptr && outLSlot.valid() && outLSlot.buffer < static_cast<int>(output->mNumberBuffers)) {
        outFrames = view(output->mBuffers[outLSlot.buffer], outLSlot, outStride);
        outData = static_cast<float*>(output->mBuffers[outLSlot.buffer].mData);
    }
    const bool writeR = outRSlot.valid() && outRSlot.buffer == outLSlot.buffer && outRSlot.channel < outStride;

    if (inputTime != nullptr && (inputTime->mFlags & kAudioTimeStampSampleTimeValid) != 0) {
        if (expectedSampleTime >= 0.0 && std::abs(inputTime->mSampleTime - expectedSampleTime) > 0.5)
            sampleTimeGaps.fetch_add(1, std::memory_order_relaxed);
        expectedSampleTime = inputTime->mSampleTime + n;
    }

    for (int offset = 0; offset < n; offset += capacity) {
        const int count = std::min(capacity, n - offset);
        for (int i = 0; i < count; ++i)
            micBuffer[static_cast<size_t>(i)] = micData[(offset + i) * micStride + micSlot.channel];
        for (int i = 0; i < count; ++i) {
            const int f = offset + i;
            refBuffer[static_cast<size_t>(i)] = refData != nullptr && f < refFrames
                                                    ? refData[f * refStride + refSlot.channel]
                                                    : 0.0f;
        }
        std::fill(outL.begin(), outL.begin() + count, 0.0f);
        std::fill(outR.begin(), outR.begin() + count, 0.0f);
        if (callback)
            callback(micBuffer.data(), refBuffer.data(), outL.data(), outR.data(), count);
        if (outData != nullptr) {
            for (int i = 0; i < count && offset + i < outFrames; ++i) {
                float* frame = outData + (offset + i) * outStride;
                frame[outLSlot.channel] = outL[static_cast<size_t>(i)];
                if (writeR)
                    frame[outRSlot.channel] = outR[static_cast<size_t>(i)];
            }
        }
    }

    frames.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
    lastFrames.store(static_cast<std::uint32_t>(n), std::memory_order_relaxed);
    if (static_cast<std::uint32_t>(n) > maxFrames.load(std::memory_order_relaxed))
        maxFrames.store(static_cast<std::uint32_t>(n), std::memory_order_relaxed);
}

// ---- main thread ------------------------------------------------------------------------------

AggregateIO::AggregateIO() : impl_(std::make_unique<Impl>())
{
    impl_->owner = this;
}

AggregateIO::~AggregateIO() = default;

std::unique_ptr<AggregateIO> AggregateIO::createDetached()
{
    return std::unique_ptr<AggregateIO>(new AggregateIO());
}

std::unique_ptr<AggregateIO> AggregateIO::create(const Params& params, Callback callback, juce::String& error)
{
    @autoreleasepool {
        if (params.micUid.startsWith("fixomic_")) {
            error = it("Il microfono non pu\u00f2 essere un dispositivo fixomic (") + params.micUid + ")";
            return nullptr;
        }
        const auto mic = findDeviceByUid(params.micUid);
        if (!mic) {
            error = "Microfono non trovato (" + params.micUid + ")";
            return nullptr;
        }
        const auto bridge = findDeviceByUid(devices::kBridgeUid);
        if (!bridge) {
            error = it("fixomic Bridge non trovato: il driver fixomic \u00e8 installato? (") + devices::kBridgeUid + ")";
            return nullptr;
        }
        const juce::String micName = util::getString(*mic, kAudioObjectPropertyName);
        const int micChannels = util::channelCount(*mic, kAudioObjectPropertyScopeInput);
        if (micChannels <= 0) {
            error = "Il microfono " + quoted(micName) + " non ha canali di ingresso";
            return nullptr;
        }
        if (params.micChannel < 0 || params.micChannel >= micChannels) {
            error = "Il microfono " + quoted(micName) + " ha " + juce::String(micChannels)
                    + " canali di ingresso: il canale " + juce::String(params.micChannel + 1) + " non esiste";
            return nullptr;
        }

        // The mic is the clock master, so it has to run at the driver's only rate.
        const auto rates = util::nominalRates(*mic);
        if (std::none_of(rates.begin(), rates.end(), [](double r) { return std::abs(r - kRate) < 0.5; })) {
            error = "Il microfono " + quoted(micName) + " non supporta 48 kHz";
            return nullptr;
        }
        if (std::abs(util::getScalar<Float64>(*mic, kAudioDevicePropertyNominalSampleRate).value_or(0.0) - kRate)
            >= 0.5) {
            const OSStatus status = util::setScalar<Float64>(*mic, kAudioDevicePropertyNominalSampleRate, kRate);
            if (status != noErr || !waitForRate(*mic, kRate)) {
                error = "Impossibile portare il microfono " + quoted(micName) + " a 48 kHz (" + util::statusString(status)
                        + ")";
                return nullptr;
            }
        }

        std::unique_ptr<AggregateIO> io(new AggregateIO());
        Impl& s = *io->impl_;
        s.mic = *mic;
        s.bridge = *bridge;
        s.micUid = params.micUid;
        s.callback = std::move(callback);

        NSString* micUid = (__bridge_transfer NSString*)params.micUid.toCFString();
        NSString* bridgeUid = (__bridge_transfer NSString*)juce::String(devices::kBridgeUid).toCFString();
        NSString* aggregateUid = [@"fixomic_aggregate_" stringByAppendingString:NSUUID.UUID.UUIDString];
        s.aggregateUid = util::toString((__bridge CFStringRef)aggregateUid);
        NSMutableDictionary* description = [@{
            @kAudioAggregateDeviceNameKey : @"fixomic Aggregate",
            @kAudioAggregateDeviceUIDKey : aggregateUid,
            @kAudioAggregateDeviceMainSubDeviceKey : micUid,
            @kAudioAggregateDeviceIsPrivateKey : @1,
            @kAudioAggregateDeviceIsStackedKey : @0,
            @kAudioAggregateDeviceSubDeviceListKey : @[
                @{@kAudioSubDeviceUIDKey : micUid},
                @{@kAudioSubDeviceUIDKey : bridgeUid, @kAudioSubDeviceDriftCompensationKey : @1},
            ],
        } mutableCopy];
        if (params.tap != nullptr) {
            NSString* tapUid = (__bridge_transfer NSString*)params.tap->uid().toCFString();
            description[@kAudioAggregateDeviceTapListKey] =
                @[ @{@kAudioSubTapUIDKey : tapUid, @kAudioSubTapDriftCompensationKey : @1} ];
            // Must stay 0: with TapAutoStart = 1, AudioDeviceStart returns noErr but the IOProc is
            // never called (observed on macOS 27.2, docs/notes/aggregate.md).
            description[@kAudioAggregateDeviceTapAutoStartKey] = @0;
        }

        const OSStatus created =
            AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)description, &s.aggregate);
        if (created != noErr || s.aggregate == kAudioObjectUnknown) {
            s.aggregate = kAudioObjectUnknown;
            error = "Impossibile creare il dispositivo aggregato (AudioHardwareCreateAggregateDevice: "
                    + util::statusString(created) + ")";
            return nullptr;
        }
        // From here on `io` owns the aggregate: every early return destroys it.

        if (std::abs(util::getScalar<Float64>(s.aggregate, kAudioDevicePropertyNominalSampleRate).value_or(0.0) - kRate)
            >= 0.5) {
            const OSStatus status = util::setScalar<Float64>(s.aggregate, kAudioDevicePropertyNominalSampleRate, kRate);
            if (status != noErr || !waitForRate(s.aggregate, kRate)) {
                error = "Impossibile portare il dispositivo aggregato a 48 kHz (" + util::statusString(status) + ")";
                return nullptr;
            }
        }

        // IO buffer size, clamped to what the aggregate accepts.
        int requested = std::max(params.bufferSize, 1);
        if (const auto range = util::getScalar<AudioValueRange>(s.aggregate, kAudioDevicePropertyBufferFrameSizeRange))
            requested = std::clamp(requested, static_cast<int>(range->mMinimum), static_cast<int>(range->mMaximum));
        const OSStatus bufferStatus =
            util::setScalar<UInt32>(s.aggregate, kAudioDevicePropertyBufferFrameSize, static_cast<UInt32>(requested));
        if (bufferStatus != noErr) {
            error = "Impossibile impostare il buffer a " + juce::String(requested) + " campioni ("
                    + util::statusString(bufferStatus) + ")";
            return nullptr;
        }
        s.bufferFrames = static_cast<int>(
            util::getScalar<UInt32>(s.aggregate, kAudioDevicePropertyBufferFrameSize).value_or(requested));

        // ---- stream mapping (see docs/notes/aggregate.md) ----
        //
        // The aggregate owns its own stream objects (their kAudioObjectPropertyOwner is the
        // aggregate, not the sub-device), so ownership cannot tell them apart. The IOProc gets one
        // AudioBuffer per aggregate stream, in kAudioDevicePropertyStreams order, and the HAL lays
        // the streams out device by device in kAudioAggregateDevicePropertyActiveSubDeviceList
        // order, followed by one stream per tap. Instead of trusting that blindly, both possible
        // layouts (sub-devices then taps, taps then sub-devices) are checked against the
        // aggregate's streams, stream by stream, on channel count and terminal type (a sub-device
        // stream keeps its terminal type in the aggregate; a tap stream has none). The first
        // layout that matches every stream is used; if none does, create() fails.
        std::vector<Block> inputBlocks, outputBlocks, tapBlocks;
        const auto subDevices =
            util::getArray<AudioObjectID>(s.aggregate, kAudioAggregateDevicePropertyActiveSubDeviceList);
        for (AudioObjectID sub : subDevices) {
            const Block::Kind kind = sub == s.mic      ? Block::Kind::Mic
                                     : sub == s.bridge ? Block::Kind::Bridge
                                                       : Block::Kind::Other;
            inputBlocks.push_back({kind, sub, streamShapes(sub, kAudioObjectPropertyScopeInput)});
            outputBlocks.push_back({kind, sub, streamShapes(sub, kAudioObjectPropertyScopeOutput)});
        }
        if (params.tap != nullptr)
            tapBlocks.push_back({Block::Kind::Tap, params.tap->id(), {{std::max(params.tap->channels(), 1), 0}}});

        const auto aggregateIn =
            util::getArray<AudioObjectID>(s.aggregate, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
        const auto aggregateOut =
            util::getArray<AudioObjectID>(s.aggregate, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
        auto aggregateShapes = [](const std::vector<AudioObjectID>& streams, bool& formatOk) {
            std::vector<StreamShape> shapes;
            formatOk = true;
            for (AudioObjectID stream : streams) {
                const auto format =
                    util::getScalar<AudioStreamBasicDescription>(stream, kAudioStreamPropertyVirtualFormat);
                formatOk = formatOk && format && isFloat32Interleaved(*format);
                shapes.push_back({format ? static_cast<int>(format->mChannelsPerFrame) : 0,
                                  util::getScalar<UInt32>(stream, kAudioStreamPropertyTerminalType).value_or(0)});
            }
            return shapes;
        };
        bool inFormatOk = false, outFormatOk = false;
        const auto inShapes = aggregateShapes(aggregateIn, inFormatOk);
        const auto outShapes = aggregateShapes(aggregateOut, outFormatOk);
        if (!inFormatOk || !outFormatOk) {
            error = "Formato audio del dispositivo aggregato non supportato (serve float a 32 bit interleaved)";
            return nullptr;
        }

        // Matches `blocks` against `shapes`; on success returns the first stream index of each block.
        auto match = [](const std::vector<Block>& blocks, const std::vector<StreamShape>& shapes,
                        std::vector<int>& firstStream) {
            firstStream.clear();
            size_t index = 0;
            for (const Block& block : blocks) {
                firstStream.push_back(static_cast<int>(index));
                for (const StreamShape& expected : block.streams) {
                    if (index >= shapes.size() || shapes[index].channels != expected.channels
                        || shapes[index].terminalType != expected.terminalType)
                        return false;
                    ++index;
                }
            }
            return index == shapes.size();
        };
        auto describeShapes = [](const std::vector<StreamShape>& shapes) {
            juce::StringArray parts;
            for (const auto& shape : shapes)
                parts.add(juce::String(shape.channels) + " ch/" + fourCharCode(shape.terminalType));
            return "[" + parts.joinIntoString(", ") + "]";
        };

        std::vector<Block> inputLayout;
        std::vector<int> inputFirst;
        juce::String layoutName;
        {
            std::vector<Block> subsThenTaps = inputBlocks;
            subsThenTaps.insert(subsThenTaps.end(), tapBlocks.begin(), tapBlocks.end());
            std::vector<Block> tapsThenSubs = tapBlocks;
            tapsThenSubs.insert(tapsThenSubs.end(), inputBlocks.begin(), inputBlocks.end());
            if (match(subsThenTaps, inShapes, inputFirst)) {
                inputLayout = subsThenTaps;
                layoutName = "sub-devices then taps";
            } else if (!tapBlocks.empty() && match(tapsThenSubs, inShapes, inputFirst)) {
                inputLayout = tapsThenSubs;
                layoutName = "taps then sub-devices";
            } else {
                error = "Disposizione inattesa degli ingressi del dispositivo aggregato (" + describeShapes(inShapes)
                        + ")";
                return nullptr;
            }
        }
        std::vector<int> outputFirst;
        if (!match(outputBlocks, outShapes, outputFirst)) {
            error = "Disposizione inattesa delle uscite del dispositivo aggregato (" + describeShapes(outShapes) + ")";
            return nullptr;
        }

        // Turns "channel k of block b" into a Slot (buffer index + channel inside the buffer).
        auto slotFor = [](const Block& block, int firstStream, int channel) {
            for (size_t i = 0; i < block.streams.size(); ++i) {
                if (channel < block.streams[i].channels)
                    return Slot{firstStream + static_cast<int>(i), channel};
                channel -= block.streams[i].channels;
            }
            return Slot{};
        };
        juce::StringArray lines;
        lines.add("input layout: " + layoutName + ", aggregate input streams " + describeShapes(inShapes));
        for (size_t b = 0; b < inputLayout.size(); ++b) {
            const Block& block = inputLayout[b];
            if (block.kind == Block::Kind::Mic)
                s.micSlot = slotFor(block, inputFirst[b], params.micChannel);
            else if (block.kind == Block::Kind::Tap)
                s.refSlot = slotFor(block, inputFirst[b], 0);
            const juce::String name = block.kind == Block::Kind::Tap
                                          ? juce::String("system audio tap")
                                          : util::getString(block.object, kAudioObjectPropertyName);
            if (!block.streams.empty())
                lines.add("  input buffers " + juce::String(inputFirst[b]) + ".."
                          + juce::String(inputFirst[b] + static_cast<int>(block.streams.size()) - 1) + " = " + name);
        }
        lines.add("output: aggregate output streams " + describeShapes(outShapes));
        for (size_t b = 0; b < outputBlocks.size(); ++b) {
            const Block& block = outputBlocks[b];
            if (block.kind == Block::Kind::Bridge) {
                s.outLSlot = slotFor(block, outputFirst[b], 0);
                s.outRSlot = slotFor(block, outputFirst[b], 1);
            }
            if (!block.streams.empty())
                lines.add("  output buffers " + juce::String(outputFirst[b]) + ".."
                          + juce::String(outputFirst[b] + static_cast<int>(block.streams.size()) - 1) + " = "
                          + util::getString(block.object, kAudioObjectPropertyName)
                          + (block.kind == Block::Kind::Bridge ? "" : " (zeroed)"));
        }
        if (!s.micSlot.valid() || !s.outLSlot.valid() || (params.tap != nullptr && !s.refSlot.valid())) {
            error = juce::String("Nel dispositivo aggregato mancano i flussi ")
                    + (!s.micSlot.valid()    ? "del microfono"
                       : !s.outLSlot.valid() ? "di fixomic Bridge"
                                             : "dell'audio di sistema");
            return nullptr;
        }
        s.micStream = aggregateIn[static_cast<size_t>(s.micSlot.buffer)];
        s.bridgeStream = aggregateOut[static_cast<size_t>(s.outLSlot.buffer)];
        lines.add("mic channel " + juce::String(params.micChannel + 1) + " -> input buffer "
                  + juce::String(s.micSlot.buffer) + " channel " + juce::String(s.micSlot.channel)
                  + (s.refSlot.valid() ? ", ref -> input buffer " + juce::String(s.refSlot.buffer) + " channel "
                                             + juce::String(s.refSlot.channel)
                                       : juce::String(", no ref (silence)"))
                  + ", outL/outR -> output buffer " + juce::String(s.outLSlot.buffer) + " channels "
                  + juce::String(s.outLSlot.channel) + "/"
                  + (s.outRSlot.valid() ? juce::String(s.outRSlot.channel) : juce::String("-")));
        s.mapping = lines.joinIntoString("\n");

        // Deinterleave buffers. The HAL may deliver up to the variable-size maximum per cycle.
        const int variableMax = static_cast<int>(
            util::getScalar<UInt32>(s.aggregate, kAudioDevicePropertyUsesVariableBufferFrameSizes).value_or(0));
        s.capacity = std::max({s.bufferFrames, variableMax, kMinCapacity});
        for (auto* buffer : {&s.micBuffer, &s.refBuffer, &s.outL, &s.outR})
            buffer->assign(static_cast<size_t>(s.capacity), 0.0f);

        const OSStatus procStatus = AudioDeviceCreateIOProcID(s.aggregate, &Impl::ioProc, &s, &s.procId);
        if (procStatus != noErr || s.procId == nullptr) {
            s.procId = nullptr;
            error = "Impossibile avviare l'audio (AudioDeviceCreateIOProcID: " + util::statusString(procStatus) + ")";
            return nullptr;
        }

        // Device-lost and overload notifications (main queue). Without the device-lost listeners
        // a vanished device would go unnoticed, so failing to register one fails create(); the
        // overload counter is diagnostic only.
        Impl* impl = &s;
        auto watch = [impl](AudioObjectID object, AudioObjectPropertySelector selector,
                            std::function<void()> onChange) {
            impl->watchers.push_back(
                std::make_unique<PropertyWatcher>(object, util::address(selector), std::move(onChange)));
            return impl->watchers.back()->isListening();
        };
        auto watchAlive = [&](AudioObjectID object) {
            return watch(object, kAudioDevicePropertyDeviceIsAlive, [impl, object] {
                if (util::getScalar<UInt32>(object, kAudioDevicePropertyDeviceIsAlive).value_or(0) == 0)
                    impl->fireLost();
            });
        };
        const char* failed = nullptr;
        if (!watchAlive(s.mic))
            failed = "microphone IsAlive";
        else if (!watchAlive(s.bridge))
            failed = "Bridge IsAlive";
        else if (!watchAlive(s.aggregate))
            failed = "aggregate IsAlive";
        else if (!watch(s.aggregate, kAudioDevicePropertyDeviceHasChanged, [impl] { impl->fireLost(); }))
            failed = "aggregate DeviceHasChanged";
        else if (!watch(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, [impl] {
                     const auto current = findDeviceByUid(impl->micUid);
                     if (!current || *current != impl->mic || !findDeviceByUid(devices::kBridgeUid))
                         impl->fireLost();
                 }))
            failed = "device list";
        if (failed != nullptr) {
            error = it("Un dispositivo audio \u00e8 scomparso durante la configurazione (listener ") + failed + ")";
            return nullptr;
        }
        watch(s.aggregate, kAudioDeviceProcessorOverload, [impl] { ++impl->overloads; });
        return io;
    }
}

bool AggregateIO::start()
{
    Impl& s = *impl_;
    if (s.running)
        return true;
    if (s.lostPending || s.lostFired) {
        s.lastError = "Dispositivo audio perso o riconfigurato prima dell'avvio (AggregateIO da ricreare)";
        return false;
    }
    s.expectedSampleTime = -1.0;
    const OSStatus status = AudioDeviceStart(s.aggregate, s.procId);
    if (status != noErr) {
        s.lastError = "Impossibile avviare il dispositivo aggregato (AudioDeviceStart: " + util::statusString(status)
                      + ")";
        return false;
    }
    s.running = true;
    return true;
}

void AggregateIO::stop()
{
    Impl& s = *impl_;
    if (!s.running)
        return;
    AudioDeviceStop(s.aggregate, s.procId);
    s.running = false;
}

LatencyBreakdown AggregateIO::latency() const
{
    const Impl& s = *impl_;
    auto frames = [](AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
        return static_cast<int>(util::getScalar<UInt32>(object, selector, scope).value_or(0));
    };
    const int input = frames(s.aggregate, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput)
                      + frames(s.aggregate, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput)
                      + frames(s.micStream, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
    const int output = frames(s.aggregate, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput)
                       + frames(s.aggregate, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput)
                       + frames(s.bridgeStream, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
    return LatencyBreakdown{nominalSampleRate(), input, output, 2 * s.bufferFrames, 0, 0};
}

AggregateIO::Stats AggregateIO::stats() const
{
    const Impl& s = *impl_;
    Stats stats;
    stats.callbacks = s.callbacks.load(std::memory_order_relaxed);
    stats.frames = s.frames.load(std::memory_order_relaxed);
    stats.sampleTimeGaps = s.sampleTimeGaps.load(std::memory_order_relaxed);
    stats.overloads = s.overloads;
    stats.lastFrames = s.lastFrames.load(std::memory_order_relaxed);
    stats.maxFrames = s.maxFrames.load(std::memory_order_relaxed);
    return stats;
}

AudioObjectID AggregateIO::deviceId() const
{
    return impl_->aggregate;
}

int AggregateIO::bufferFrames() const
{
    return impl_->bufferFrames;
}

double AggregateIO::nominalSampleRate() const
{
    return util::getScalar<Float64>(impl_->aggregate, kAudioDevicePropertyNominalSampleRate).value_or(0.0);
}

double AggregateIO::actualSampleRate() const
{
    return util::getScalar<Float64>(impl_->aggregate, kAudioDevicePropertyActualSampleRate).value_or(0.0);
}

juce::String AggregateIO::describe() const
{
    const Impl& s = *impl_;
    auto frames = [](AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
        return juce::String(static_cast<int>(util::getScalar<UInt32>(object, selector, scope).value_or(0)));
    };
    auto deviceLine = [&](const juce::String& label, AudioObjectID device, AudioObjectPropertyScope scope) {
        return "  " + label + ": latency " + frames(device, kAudioDevicePropertyLatency, scope) + ", safety offset "
               + frames(device, kAudioDevicePropertySafetyOffset, scope);
    };
    juce::StringArray lines;
    lines.add(s.mapping);
    lines.add("buffer " + juce::String(s.bufferFrames) + " frames, preallocated " + juce::String(s.capacity)
              + ", nominal rate " + juce::String(nominalSampleRate(), 0));
    lines.add("latency figures (frames):");
    lines.add(deviceLine("aggregate input ", s.aggregate, kAudioObjectPropertyScopeInput) + ", mic stream latency "
              + frames(s.micStream, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal));
    lines.add(deviceLine("aggregate output", s.aggregate, kAudioObjectPropertyScopeOutput)
              + ", Bridge stream latency "
              + frames(s.bridgeStream, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal));
    lines.add(deviceLine("mic (sub-device) input ", s.mic, kAudioObjectPropertyScopeInput));
    lines.add(deviceLine("Bridge (sub-device) output", s.bridge, kAudioObjectPropertyScopeOutput));
    return lines.joinIntoString("\n");
}

juce::String AggregateIO::lastError() const
{
    return impl_->lastError;
}

} // namespace fixomic::mac
