// fxprobe: diagnostics for the fixomic HAL driver ("fixomic Mic" / "fixomic Bridge").
//
//   fxprobe list            all audio devices (hidden ones included): name, UID, channels, hidden
//   fxprobe check-driver    runs the five driver checks, exit 0 only if all pass
//   fxprobe running [secs]  polls IsRunningSomewhere / IsRunning of fixomic Mic every 200 ms
//                           and prints every transition (runs until Ctrl-C without secs)
//   fxprobe loopback-test   writes an impulse to Bridge every 500 ms for 5 s, reads Mic,
//                           prints the Bridge->Mic latency in samples and the impulses received
//   fxprobe hold-mic <secs> (internal) keeps an input IOProc running on Mic, used by check 4
//   fxprobe aggregate-test <micUid> [--no-tap] [--seconds N] [--buffer N] [--channel N]
//                           runs fixomic's AggregateIO (mic + Bridge + system audio tap) for N s
//                           (default 10), passes the mic to Bridge and prints the gate-2 criteria:
//                           callback cadence, reference peak, overloads, effective rate
//   fxprobe engine-test <micUid> [--seconds N] [--no-aec] [--buffer N] [--channel N]
//                           runs the on-demand engine (AecStage + empty PluginChain + ProcessingGraph +
//                           MacEngineBackend + DemandMonitor + EngineController) for N s (default 30)
//                           and prints every state transition, timed against a 5 ms raw poll of
//                           fixomic Mic's IsRunningSomewhere
//
// fxprobe links fixomic_core only for aggregate-test and engine-test; the driver
// checks use plain Core Audio. The binary embeds tools/fxprobe/Info.plist (microphone and system
// audio usage strings): TCC needs them when fxprobe is its own responsible process (launchd job).
//
// Sample times of the two devices share one timeline (the driver publishes the same zero time
// stamps for both and indexes its ring buffer by sample time), so latency is measured as
// "input sample time at which the impulse is read" minus "output sample time it was written at".

#import <AVFoundation/AVFoundation.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <random>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dsp/AecStage.h"
#include "dsp/PluginChain.h"
#include "dsp/ProcessingGraph.h"
#include "engine/EngineController.h"
#include "platform/mac/AggregateIO.h"
#include "platform/mac/DemandMonitor.h"
#include "platform/mac/FixomicDevices.h"
#include "platform/mac/MacEngineBackend.h"
#include "platform/mac/SystemAudioTap.h"
#include "platform/mac/WakeObserver.h"

#include <mach/mach_time.h>

extern char** environ;

namespace {

using namespace std::chrono_literals;
namespace devices = fixomic::devices;

// ---- property helpers ------------------------------------------------------------------------

AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    return {selector, scope, kAudioObjectPropertyElementMain};
}

template <typename T>
std::optional<T> getScalar(AudioObjectID object, AudioObjectPropertySelector selector,
                           AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    const auto addr = address(selector, scope);
    T value{};
    UInt32 size = sizeof(T);
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &value) != noErr)
        return std::nullopt;
    return value;
}

std::string toStdString(CFStringRef string)
{
    if (string == nullptr)
        return {};
    const CFIndex length = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<size_t>(length), '\0');
    if (!CFStringGetCString(string, result.data(), length, kCFStringEncodingUTF8))
        return {};
    result.resize(std::strlen(result.c_str()));
    return result;
}

std::string getString(AudioObjectID object, AudioObjectPropertySelector selector)
{
    const auto addr = address(selector);
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &value) != noErr || value == nullptr)
        return {};
    std::string result = toStdString(value);
    CFRelease(value);
    return result;
}

std::vector<AudioObjectID> getObjectList(AudioObjectID object, AudioObjectPropertySelector selector)
{
    const auto addr = address(selector);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(object, &addr, 0, nullptr, &size) != noErr || size == 0)
        return {};
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, ids.data()) != noErr)
        return {};
    ids.resize(size / sizeof(AudioObjectID));
    return ids;
}

UInt32 channelCount(AudioObjectID device, AudioObjectPropertyScope scope)
{
    const auto addr = address(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) != noErr || size == 0)
        return 0;
    std::vector<std::byte> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, list) != noErr)
        return 0;
    UInt32 channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        channels += list->mBuffers[i].mNumberChannels;
    return channels;
}

// kAudioHardwarePropertyTranslateUIDToDevice is the only way to reach hidden devices.
std::pair<OSStatus, AudioObjectID> translateUid(const char* uid)
{
    const auto addr = address(kAudioHardwarePropertyTranslateUIDToDevice);
    CFStringRef uidString = CFStringCreateWithCString(kCFAllocatorDefault, uid, kCFStringEncodingUTF8);
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    const OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, sizeof(uidString),
                                                       &uidString, &size, &device);
    CFRelease(uidString);
    return {status, device};
}

AudioObjectID findDevice(const char* uid)
{
    const auto [status, device] = translateUid(uid);
    return status == noErr ? device : kAudioObjectUnknown;
}

std::string fourCC(OSStatus status)
{
    char chars[5] = {};
    const UInt32 be = CFSwapInt32HostToBig(static_cast<UInt32>(status));
    std::memcpy(chars, &be, 4);
    for (char c : std::string(chars, 4))
        if (c < 32 || c > 126)
            return std::to_string(status);
    return "'" + std::string(chars, 4) + "' (" + std::to_string(status) + ")";
}

UInt32 isRunningSomewhere(AudioObjectID device)
{
    return getScalar<UInt32>(device, kAudioDevicePropertyDeviceIsRunningSomewhere).value_or(UINT32_MAX);
}

UInt32 isRunning(AudioObjectID device)
{
    return getScalar<UInt32>(device, kAudioDevicePropertyDeviceIsRunning).value_or(UINT32_MAX);
}

double nominalRate(AudioObjectID device)
{
    return getScalar<Float64>(device, kAudioDevicePropertyNominalSampleRate).value_or(0.0);
}

const char* micAuthorization()
{
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusAuthorized: return "authorized";
    case AVAuthorizationStatusDenied: return "denied";
    case AVAuthorizationStatusRestricted: return "restricted";
    case AVAuthorizationStatusNotDetermined: return "not determined";
    }
    return "unknown";
}

// ---- IO engine -------------------------------------------------------------------------------

constexpr int kMaxEvents = 256;
constexpr double kDetectThreshold = 0.5;

enum class WriteMode { Silence, Impulses, Noise };

// Shared between the main thread and the two IOProcs. Only atomics and fixed-size storage, so
// the IO threads never allocate or lock.
struct Probe {
    // writer (Bridge)
    std::atomic<int> writeMode{static_cast<int>(WriteMode::Silence)};
    std::atomic<double> firstOutputTime{-1.0};
    std::atomic<double> lastWrittenEnd{-1.0};  // output sample time after the last non-silent frame
    double impulseStart = 0.0;                 // set from firstOutputTime + lead
    double impulsePeriod = 24000.0;
    int impulseCount = 10;
    std::atomic<bool> impulseScheduleReady{false};
    std::atomic<int> impulsesWritten{0};
    std::atomic<int> writerCallbacks{0};
    std::atomic<UInt32> writerFrames{0};
    std::minstd_rand noise{12345};  // touched by the writer IOProc only

    // reader (Mic)
    std::atomic<int> readerCallbacks{0};
    std::atomic<UInt32> readerFrames{0};
    std::atomic<double> firstInputTime{-1.0};
    std::atomic<double> lastInputEnd{-1.0};
    std::atomic<long long> nonZeroFrames{0};
    std::atomic<long long> nonZeroAfterWriterEnd{0};  // frames read at t >= lastWrittenEnd
    std::atomic<double> firstStaleTime{-1.0};
    std::atomic<double> lastDetection{-1e18};
    std::array<std::atomic<double>, kMaxEvents> detections{};
    std::atomic<int> detectionCount{0};
};

OSStatus writerProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList*, const AudioTimeStamp*,
                    AudioBufferList* outOutputData, const AudioTimeStamp* inOutputTime, void* context)
{
    auto& probe = *static_cast<Probe*>(context);
    const auto mode = static_cast<WriteMode>(probe.writeMode.load(std::memory_order_relaxed));
    const double t0 = inOutputTime->mSampleTime;
    double expected = -1.0;
    probe.firstOutputTime.compare_exchange_strong(expected, t0);
    probe.writerCallbacks.fetch_add(1, std::memory_order_relaxed);

    for (UInt32 b = 0; b < outOutputData->mNumberBuffers; ++b) {
        AudioBuffer& buffer = outOutputData->mBuffers[b];
        auto* samples = static_cast<float*>(buffer.mData);
        const UInt32 channels = std::max<UInt32>(buffer.mNumberChannels, 1);
        const UInt32 frames = buffer.mDataByteSize / (sizeof(float) * channels);
        probe.writerFrames.store(frames, std::memory_order_relaxed);
        bool wroteSomething = false;
        for (UInt32 f = 0; f < frames; ++f) {
            const double t = t0 + f;
            float value = 0.0f;
            if (mode == WriteMode::Noise) {
                // Never exactly zero: uniform in +-[0.1, 0.5].
                const float u = static_cast<float>(probe.noise() % 10000) / 10000.0f;
                value = (0.1f + 0.4f * u) * ((probe.noise() & 1) ? 1.0f : -1.0f);
            } else if (mode == WriteMode::Impulses && probe.impulseScheduleReady.load(std::memory_order_acquire)) {
                const double k = (t - probe.impulseStart) / probe.impulsePeriod;
                if (k >= 0 && k < probe.impulseCount && std::fabs(k - std::round(k)) * probe.impulsePeriod < 0.5) {
                    value = 1.0f;
                    probe.impulsesWritten.fetch_add(1, std::memory_order_relaxed);
                }
            }
            for (UInt32 c = 0; c < channels; ++c)
                samples[f * channels + c] = value;
            wroteSomething = wroteSomething || value != 0.0f;
        }
        if (wroteSomething || mode == WriteMode::Noise)
            probe.lastWrittenEnd.store(t0 + frames, std::memory_order_release);
    }
    return noErr;
}

OSStatus readerProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* inInputData,
                    const AudioTimeStamp* inInputTime, AudioBufferList*, const AudioTimeStamp*, void* context)
{
    auto& probe = *static_cast<Probe*>(context);
    const double t0 = inInputTime->mSampleTime;
    double expected = -1.0;
    probe.firstInputTime.compare_exchange_strong(expected, t0);
    probe.readerCallbacks.fetch_add(1, std::memory_order_relaxed);
    const double writtenEnd = probe.lastWrittenEnd.load(std::memory_order_acquire);

    for (UInt32 b = 0; b < inInputData->mNumberBuffers; ++b) {
        const AudioBuffer& buffer = inInputData->mBuffers[b];
        const auto* samples = static_cast<const float*>(buffer.mData);
        const UInt32 channels = std::max<UInt32>(buffer.mNumberChannels, 1);
        const UInt32 frames = buffer.mDataByteSize / (sizeof(float) * channels);
        probe.readerFrames.store(frames, std::memory_order_relaxed);
        for (UInt32 f = 0; f < frames; ++f) {
            const double t = t0 + f;
            float peak = 0.0f;
            for (UInt32 c = 0; c < channels; ++c)
                peak = std::max(peak, std::fabs(samples[f * channels + c]));
            if (peak == 0.0f)
                continue;
            probe.nonZeroFrames.fetch_add(1, std::memory_order_relaxed);
            if (t >= writtenEnd) {
                probe.nonZeroAfterWriterEnd.fetch_add(1, std::memory_order_relaxed);
                double none = -1.0;
                probe.firstStaleTime.compare_exchange_strong(none, t);
            }
            if (peak > kDetectThreshold && t - probe.lastDetection.load(std::memory_order_relaxed) > 1000.0) {
                probe.lastDetection.store(t, std::memory_order_relaxed);
                const int i = probe.detectionCount.load(std::memory_order_relaxed);
                if (i < kMaxEvents) {
                    probe.detections[i].store(t, std::memory_order_relaxed);
                    probe.detectionCount.store(i + 1, std::memory_order_release);
                }
            }
        }
        probe.lastInputEnd.store(t0 + frames, std::memory_order_release);
    }
    return noErr;
}

class IoRunner {
public:
    IoRunner(AudioObjectID device, AudioDeviceIOProc proc, Probe& probe) : device_(device)
    {
        status_ = AudioDeviceCreateIOProcID(device, proc, &probe, &procId_);
        if (status_ == noErr)
            status_ = AudioDeviceStart(device, procId_);
        started_ = status_ == noErr;
    }
    ~IoRunner() { stop(); }
    IoRunner(const IoRunner&) = delete;
    IoRunner& operator=(const IoRunner&) = delete;

    void stop()
    {
        if (procId_ == nullptr)
            return;
        if (started_)
            AudioDeviceStop(device_, procId_);
        AudioDeviceDestroyIOProcID(device_, procId_);
        procId_ = nullptr;
        started_ = false;
    }
    bool ok() const { return status_ == noErr; }
    OSStatus status() const { return status_; }

private:
    AudioObjectID device_;
    AudioDeviceIOProcID procId_ = nullptr;
    OSStatus status_ = noErr;
    bool started_ = false;
};

struct Devices {
    AudioObjectID mic = kAudioObjectUnknown;
    AudioObjectID bridge = kAudioObjectUnknown;
};

std::optional<Devices> requireDevices()
{
    Devices d{findDevice(devices::kMicUid), findDevice(devices::kBridgeUid)};
    if (d.mic == kAudioObjectUnknown || d.bridge == kAudioObjectUnknown) {
        std::fprintf(stderr, "error: fixomic devices not found (mic %u, bridge %u); is the driver installed?\n",
                     d.mic, d.bridge);
        return std::nullopt;
    }
    return d;
}

// ---- loopback --------------------------------------------------------------------------------

struct LoopbackResult {
    bool ioOk = false;
    int received = 0;
    int expected = 0;
    int written = 0;
    double minLatency = 0;
    double maxLatency = 0;
    int spurious = 0;
    std::string error;
};

LoopbackResult runLoopback(const Devices& d, bool verbose)
{
    LoopbackResult result;
    Probe probe;
    result.expected = probe.impulseCount;
    probe.writeMode = static_cast<int>(WriteMode::Impulses);

    IoRunner reader(d.mic, readerProc, probe);
    if (!reader.ok()) {
        result.error = "starting Mic IO failed: " + fourCC(reader.status());
        return result;
    }
    IoRunner writer(d.bridge, writerProc, probe);
    if (!writer.ok()) {
        result.error = "starting Bridge IO failed: " + fourCC(writer.status());
        return result;
    }
    // First impulse 100 ms after the writer's first cycle, then every 500 ms.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (probe.firstOutputTime.load() < 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    if (probe.firstOutputTime.load() < 0) {
        result.error = "Bridge IOProc never ran";
        return result;
    }
    probe.impulseStart = std::floor(probe.firstOutputTime.load()) + 4800.0;
    probe.impulseScheduleReady.store(true, std::memory_order_release);

    // 10 impulses span 4.5 s; keep reading until the reader is past the last one.
    const double lastImpulse = probe.impulseStart + probe.impulsePeriod * (probe.impulseCount - 1);
    const auto stopBy = std::chrono::steady_clock::now() + 8s;
    while (probe.lastInputEnd.load() < lastImpulse + 4800.0 && std::chrono::steady_clock::now() < stopBy)
        std::this_thread::sleep_for(20ms);
    writer.stop();
    reader.stop();

    result.ioOk = true;
    result.written = probe.impulsesWritten.load();  // one frame per impulse
    std::vector<bool> matched(static_cast<size_t>(probe.impulseCount), false);
    bool first = true;
    const int count = std::min(probe.detectionCount.load(std::memory_order_acquire), kMaxEvents);
    for (int i = 0; i < count; ++i) {
        const double r = probe.detections[i].load();
        const double k = std::floor((r - probe.impulseStart) / probe.impulsePeriod + 0.02);
        const double latency = r - (probe.impulseStart + k * probe.impulsePeriod);
        if (k < 0 || k >= probe.impulseCount || latency < 0 || latency > 12000) {
            ++result.spurious;
            if (verbose)
                std::printf("    detection at %.0f does not match any impulse\n", r);
            continue;
        }
        if (matched[static_cast<size_t>(k)]) {
            ++result.spurious;
            continue;
        }
        matched[static_cast<size_t>(k)] = true;
        ++result.received;
        if (verbose)
            std::printf("    impulse %2d written at %.0f, read at %.0f, latency %.0f samples\n",
                        static_cast<int>(k) + 1, probe.impulseStart + k * probe.impulsePeriod, r, latency);
        result.minLatency = first ? latency : std::min(result.minLatency, latency);
        result.maxLatency = first ? latency : std::max(result.maxLatency, latency);
        first = false;
    }
    if (verbose)
        std::printf("    IO buffer: Bridge %u frames, Mic %u frames\n", probe.writerFrames.load(),
                    probe.readerFrames.load());
    return result;
}

// Shown when the Bridge writer ran but nothing came back on Mic.
void printSilentPathHint()
{
    std::printf("    hint: nothing written to Bridge came back on Mic. Either this process's output is muted\n"
                "          before it reaches the driver (a per-app volume tool such as Vorssaint or SoundSource\n"
                "          taps and mutes the processes of the app that launched fxprobe: run fxprobe from\n"
                "          another app or as a launchd job, e.g. launchctl submit -l fxprobe -o out.txt -- \n"
                "          <path>/fxprobe check-driver), or input is silenced (microphone permission: %s).\n",
                micAuthorization());
}

// ---- checks ----------------------------------------------------------------------------------

bool checkResolveBridge(Devices& d)
{
    std::printf("[1] Bridge resolvable via kAudioHardwarePropertyTranslateUIDToDevice\n");
    const auto [status, bridge] = translateUid(devices::kBridgeUid);
    const auto [micStatus, mic] = translateUid(devices::kMicUid);
    d.bridge = status == noErr ? bridge : kAudioObjectUnknown;
    d.mic = micStatus == noErr ? mic : kAudioObjectUnknown;
    std::printf("    %s -> status %s, device %u\n", devices::kBridgeUid, fourCC(status).c_str(), bridge);
    std::printf("    %s -> status %s, device %u\n", devices::kMicUid, fourCC(micStatus).c_str(), mic);
    bool ok = d.bridge != kAudioObjectUnknown && d.mic != kAudioObjectUnknown;
    if (ok) {
        const std::string bridgeName = getString(d.bridge, kAudioObjectPropertyName);
        const std::string micName = getString(d.mic, kAudioObjectPropertyName);
        const UInt32 bridgeIn = channelCount(d.bridge, kAudioObjectPropertyScopeInput);
        const UInt32 bridgeOut = channelCount(d.bridge, kAudioObjectPropertyScopeOutput);
        const UInt32 micIn = channelCount(d.mic, kAudioObjectPropertyScopeInput);
        const UInt32 micOut = channelCount(d.mic, kAudioObjectPropertyScopeOutput);
        const UInt32 bridgeHidden = getScalar<UInt32>(d.bridge, kAudioDevicePropertyIsHidden).value_or(99);
        const UInt32 micHidden = getScalar<UInt32>(d.mic, kAudioDevicePropertyIsHidden).value_or(99);
        std::printf("    Bridge: name \"%s\", in %u, out %u, hidden %u, rate %.0f\n", bridgeName.c_str(), bridgeIn,
                    bridgeOut, bridgeHidden, nominalRate(d.bridge));
        std::printf("    Mic:    name \"%s\", in %u, out %u, hidden %u, rate %.0f\n", micName.c_str(), micIn, micOut,
                    micHidden, nominalRate(d.mic));
        ok = bridgeName == devices::kBridgeName && micName == devices::kMicName && bridgeIn == 0 && bridgeOut == 2
             && micIn == 2 && micOut == 0 && bridgeHidden == 1 && micHidden == 0;
    }
    std::printf("    => %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool checkLoopback(const Devices& d)
{
    std::printf("[2] loopback: 10 impulses Bridge -> Mic, latency <= 256 samples\n");
    const auto r = runLoopback(d, true);
    if (!r.ioOk) {
        std::printf("    %s\n    => FAIL\n", r.error.c_str());
        return false;
    }
    std::printf("    written %d, received %d/%d, latency min %.0f max %.0f samples, spurious detections %d\n",
                r.written, r.received, r.expected, r.minLatency, r.maxLatency, r.spurious);
    if (r.received == 0)
        printSilentPathHint();
    const bool ok = r.received == r.expected && r.maxLatency <= 256 && r.spurious == 0;
    std::printf("    => %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool checkSilence(const Devices& d)
{
    std::printf("[3] silence without writer\n");
    if (isRunningSomewhere(d.bridge) == 1) {
        std::printf("    Bridge is running in another process, cannot test\n    => FAIL\n");
        return false;
    }
    Probe probe;
    IoRunner reader(d.mic, readerProc, probe);
    if (!reader.ok()) {
        std::printf("    starting Mic IO failed: %s\n    => FAIL\n", fourCC(reader.status()).c_str());
        return false;
    }
    // Phase A: nobody writes for 2 s.
    std::this_thread::sleep_for(2s);
    const long long nonZeroA = probe.nonZeroFrames.load();
    const double framesA = probe.lastInputEnd.load() - probe.firstInputTime.load();
    std::printf("    A) 2 s, no writer: %.0f frames read, %lld non-zero\n", framesA, nonZeroA);

    // Phase B: write noise for 1 s, stop, keep reading 2 s. Frames read at sample times the
    // writer actually wrote are legitimate; every frame at or after the writer's last written
    // sample time must be zero (no stale ring-buffer content).
    probe.writeMode = static_cast<int>(WriteMode::Noise);
    {
        IoRunner writer(d.bridge, writerProc, probe);
        if (!writer.ok()) {
            std::printf("    starting Bridge IO failed: %s\n    => FAIL\n", fourCC(writer.status()).c_str());
            return false;
        }
        std::this_thread::sleep_for(1s);
        writer.stop();
    }
    const double writtenEnd = probe.lastWrittenEnd.load();
    const double readAtStop = probe.lastInputEnd.load();
    std::this_thread::sleep_for(2s);
    const double readEnd = probe.lastInputEnd.load();
    reader.stop();

    const long long noiseRead = probe.nonZeroFrames.load() - nonZeroA - probe.nonZeroAfterWriterEnd.load();
    const long long stale = probe.nonZeroAfterWriterEnd.load();
    std::printf("    B) noise 1 s then stop: writer ended at %.0f, reader was at %.0f, read on until %.0f\n",
                writtenEnd, readAtStop, readEnd);
    std::printf("       noise frames read back %lld, non-zero frames at t >= writer end %lld", noiseRead, stale);
    if (stale > 0)
        std::printf(" (first at %.0f)", probe.firstStaleTime.load());
    std::printf("\n");
    bool ok = nonZeroA == 0 && stale == 0 && framesA > 48000 && readEnd - writtenEnd > 48000;
    if (noiseRead == 0) {
        std::printf("       no noise was read back at all\n");
        printSilentPathHint();
        ok = false;
    }
    std::printf("    => %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

std::string selfPath()
{
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0)
        return "fxprobe";
    return path;
}

bool checkDemand(const Devices& d)
{
    std::printf("[4] demand detection: a second process reads Mic\n");
    struct Sample {
        UInt32 somewhere;
        UInt32 running;
    };
    auto sample = [&] { return Sample{isRunningSomewhere(d.mic), isRunning(d.mic)}; };

    // Our own IO from checks 2-3 must have wound down.
    auto t = std::chrono::steady_clock::now();
    while ((sample().somewhere == 1 || sample().running == 1) && std::chrono::steady_clock::now() - t < 3s)
        std::this_thread::sleep_for(50ms);
    const Sample before = sample();
    if (before.somewhere == 1)
        std::printf("    IsRunningSomewhere is already 1: another app is reading %s, close it and retry\n",
                    devices::kMicName);

    const std::string self = selfPath();
    char arg1[] = "hold-mic";
    char arg2[] = "2";
    char* argv[] = {const_cast<char*>(self.c_str()), arg1, arg2, nullptr};
    pid_t child = 0;
    if (posix_spawn(&child, self.c_str(), nullptr, nullptr, argv, environ) != 0) {
        std::printf("    cannot spawn %s\n    => FAIL\n", self.c_str());
        return false;
    }
    Sample during{0, 0};
    int status = 0;
    bool waitFailed = false;
    bool timedOut = false;
    const auto childDeadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        const pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child)
            break;
        if (r == -1) {
            waitFailed = true;
            break;
        }
        if (std::chrono::steady_clock::now() > childDeadline) {
            timedOut = true;
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            break;
        }
        const Sample s = sample();
        if (s.somewhere == 1)
            during.somewhere = 1;
        if (s.running == 1)
            during.running = 1;
        std::this_thread::sleep_for(50ms);
    }
    t = std::chrono::steady_clock::now();
    Sample after = sample();
    while ((after.somewhere == 1 || after.running == 1) && std::chrono::steady_clock::now() - t < 3s) {
        std::this_thread::sleep_for(50ms);
        after = sample();
    }
    const bool childOk = !waitFailed && !timedOut && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::printf("    reader process %d %s\n", child,
                waitFailed ? "could not be waited for"
                : timedOut ? "did not finish within 10 s and was killed"
                : childOk  ? "exited ok"
                           : "exited with an error");
    std::printf("    IsRunningSomewhere: before %u, during max %u, after %u\n", before.somewhere, during.somewhere,
                after.somewhere);
    std::printf("    IsRunning:          before %u, during max %u, after %u\n", before.running, during.running,
                after.running);
    const bool somewhereOk = before.somewhere == 0 && during.somewhere == 1 && after.somewhere == 0;
    const bool runningOk = before.running == 0 && during.running == 1 && after.running == 0;
    std::printf("    IsRunningSomewhere 0->1->0: %s, IsRunning 0->1->0: %s\n", somewhereOk ? "yes" : "no",
                runningOk ? "yes" : "no");
    const bool ok = childOk && (somewhereOk || runningOk);
    std::printf("    => %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool checkRateLocked(const Devices& d)
{
    std::printf("[5] client cannot set 44100 Hz on Mic\n");
    const auto addr = address(kAudioDevicePropertyNominalSampleRate);
    Boolean settable = false;
    AudioObjectIsPropertySettable(d.mic, &addr, &settable);
    const Float64 requested = 44100.0;
    const OSStatus status = AudioObjectSetPropertyData(d.mic, &addr, 0, nullptr, sizeof(requested), &requested);
    std::this_thread::sleep_for(500ms);
    const double rate = nominalRate(d.mic);
    std::printf("    settable %d, set 44100 -> %s, rate now %.0f\n", settable, fourCC(status).c_str(), rate);
    if (status == noErr || rate != devices::kSampleRate) {
        // Never leave the device at a wrong rate behind.
        const Float64 restore = devices::kSampleRate;
        const OSStatus restored = AudioObjectSetPropertyData(d.mic, &addr, 0, nullptr, sizeof(restore), &restore);
        std::this_thread::sleep_for(500ms);
        std::printf("    restored %.0f -> %s, rate now %.0f\n", restore, fourCC(restored).c_str(), nominalRate(d.mic));
    }
    const bool ok = status != noErr && rate == devices::kSampleRate;
    std::printf("    => %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---- commands --------------------------------------------------------------------------------

int cmdList()
{
    std::vector<AudioObjectID> ids = getObjectList(kAudioObjectSystemObject, kAudioHardwarePropertyDevices);
    // Hidden devices are not in kAudioHardwarePropertyDevices; plug-ins still report them.
    for (AudioObjectID plugIn : getObjectList(kAudioObjectSystemObject, kAudioHardwarePropertyPlugInList))
        for (AudioObjectID device : getObjectList(plugIn, kAudioPlugInPropertyDeviceList))
            if (std::find(ids.begin(), ids.end(), device) == ids.end())
                ids.push_back(device);
    for (const char* uid : {devices::kMicUid, devices::kBridgeUid}) {
        const AudioObjectID device = findDevice(uid);
        if (device != kAudioObjectUnknown && std::find(ids.begin(), ids.end(), device) == ids.end())
            ids.push_back(device);
    }
    std::printf("%-5s %-36s %-48s %3s %3s %6s %7s\n", "id", "name", "uid", "in", "out", "hidden", "rate");
    for (AudioObjectID device : ids) {
        std::printf("%-5u %-36s %-48s %3u %3u %6u %7.0f\n", device, getString(device, kAudioObjectPropertyName).c_str(),
                    getString(device, kAudioDevicePropertyDeviceUID).c_str(),
                    channelCount(device, kAudioObjectPropertyScopeInput),
                    channelCount(device, kAudioObjectPropertyScopeOutput),
                    getScalar<UInt32>(device, kAudioDevicePropertyIsHidden).value_or(0), nominalRate(device));
    }
    return 0;
}

int cmdCheckDriver()
{
    std::printf("fixomic driver checks (microphone permission: %s)\n", micAuthorization());
    Devices d;
    const bool c1 = checkResolveBridge(d);
    if (d.mic == kAudioObjectUnknown || d.bridge == kAudioObjectUnknown) {
        std::printf("devices missing, remaining checks skipped\nRESULT: FAIL\n");
        return 1;
    }
    const bool c2 = checkLoopback(d);
    const bool c3 = checkSilence(d);
    const bool c4 = checkDemand(d);
    const bool c5 = checkRateLocked(d);
    const bool all = c1 && c2 && c3 && c4 && c5;
    std::printf("RESULT: %s (1 %s, 2 %s, 3 %s, 4 %s, 5 %s)\n", all ? "PASS" : "FAIL", c1 ? "pass" : "FAIL",
                c2 ? "pass" : "FAIL", c3 ? "pass" : "FAIL", c4 ? "pass" : "FAIL", c5 ? "pass" : "FAIL");
    return all ? 0 : 1;
}

std::atomic<bool> gInterrupted{false};

int cmdRunning(double seconds)
{
    const AudioObjectID mic = findDevice(devices::kMicUid);
    if (mic == kAudioObjectUnknown) {
        std::fprintf(stderr, "error: %s not found\n", devices::kMicName);
        return 1;
    }
    std::signal(SIGINT, [](int) { gInterrupted = true; });
    const auto start = std::chrono::steady_clock::now();
    UInt32 lastSomewhere = UINT32_MAX - 1, lastRunning = UINT32_MAX - 1;
    std::printf("polling %s (device %u) every 200 ms%s\n", devices::kMicName, mic, seconds > 0 ? "" : ", Ctrl-C to stop");
    while (!gInterrupted) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (seconds > 0 && elapsed > seconds)
            break;
        const UInt32 somewhere = isRunningSomewhere(mic);
        const UInt32 running = isRunning(mic);
        if (somewhere != lastSomewhere || running != lastRunning) {
            std::printf("%8.1f s  IsRunningSomewhere=%u  IsRunning=%u\n", elapsed, somewhere, running);
            std::fflush(stdout);
            lastSomewhere = somewhere;
            lastRunning = running;
        }
        std::this_thread::sleep_for(200ms);
    }
    return 0;
}

int cmdLoopback()
{
    const auto d = requireDevices();
    if (!d)
        return 1;
    std::printf("loopback: impulse every 500 ms for 5 s on %s, reading %s (microphone permission: %s)\n",
                devices::kBridgeName, devices::kMicName, micAuthorization());
    const auto r = runLoopback(*d, true);
    if (!r.ioOk) {
        std::fprintf(stderr, "error: %s\n", r.error.c_str());
        return 1;
    }
    if (r.received == 0)
        printSilentPathHint();
    std::printf("impulses received: %d/%d\n", r.received, r.expected);
    std::printf("latency: min %.0f, max %.0f samples\n", r.minLatency, r.maxLatency);
    return r.received == r.expected ? 0 : 1;
}

int cmdHoldMic(double seconds)
{
    const AudioObjectID mic = findDevice(devices::kMicUid);
    if (mic == kAudioObjectUnknown)
        return 1;
    Probe probe;
    IoRunner reader(mic, readerProc, probe);
    if (!reader.ok())
        return 2;
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    reader.stop();
    return probe.readerCallbacks.load() > 0 ? 0 : 3;
}

// ---- aggregate-test --------------------------------------------------------------------------

double hostTicksToSeconds(std::uint64_t ticks)
{
    static const double scale = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return static_cast<double>(info.numer) / info.denom * 1e-9;
    }();
    return static_cast<double>(ticks) * scale;
}

double toDb(float peak)
{
    return peak > 0.0f ? 20.0 * std::log10(peak) : -200.0;
}

void atomicMax(std::atomic<float>& target, float value)
{
    float current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

// Written by the AggregateIO callback (IO thread): atomics only, no allocation.
struct AggregateProbe {
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> lastHost{0};
    std::atomic<std::uint64_t> maxIntervalTicks{0};
    std::atomic<std::uint64_t> intervalsOverTwoBuffers{0};
    std::atomic<bool> measuring{false};  // false during warm-up: intervals are not judged
    std::atomic<float> refPeak{0.0f};    // since the last 1 s report
    std::atomic<float> micPeak{0.0f};
    std::atomic<float> refPeakTotal{0.0f};
    std::atomic<std::uint64_t> nonFiniteSamples{0};
    // First callback after warm-up: host time and frames delivered up to and including it.
    std::atomic<std::uint64_t> firstMeasuredHost{0};
    std::atomic<std::uint64_t> firstMeasuredFrames{0};
    double bufferTicks = 0.0;  // one IO buffer in host ticks, set before start
};

struct AggregateTestOptions {
    std::string micUid;
    bool tap = true;
    double seconds = 10.0;
    int buffer = 128;
    int channel = 0;
};

std::optional<AggregateTestOptions> parseAggregateTest(int argc, const char* argv[])
{
    if (argc < 3 || argv[2][0] == '-')
        return std::nullopt;
    AggregateTestOptions o;
    o.micUid = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--no-tap")
            o.tap = false;
        else if (arg == "--seconds" && i + 1 < argc)
            o.seconds = std::atof(argv[++i]);
        else if (arg == "--buffer" && i + 1 < argc)
            o.buffer = std::atoi(argv[++i]);
        else if (arg == "--channel" && i + 1 < argc)
            o.channel = std::atoi(argv[++i]);
        else
            return std::nullopt;
    }
    if (o.seconds <= 0 || o.buffer <= 0 || o.channel < 0)
        return std::nullopt;
    return o;
}

void runMainLoopFor(double seconds, const std::function<bool()>& stopEarly)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end && !gInterrupted && !(stopEarly && stopEarly())) {
        // A timer keeps the run loop alive so the main queue (property listeners) is serviced.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    }
}

int cmdAggregateTest(const AggregateTestOptions& o)
{
    namespace mac = fixomic::mac;
    std::signal(SIGINT, [](int) { gInterrupted = true; });
    CFRunLoopTimerRef keepAlive = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 3600, 3600, 0, 0, ^(CFRunLoopTimerRef) {
        });
    CFRunLoopAddTimer(CFRunLoopGetMain(), keepAlive, kCFRunLoopDefaultMode);

    std::printf("aggregate-test: mic %s, channel %d, tap %s, buffer %d, %.0f s (microphone permission: %s)\n",
                o.micUid.c_str(), o.channel, o.tap ? "yes" : "no", o.buffer, o.seconds, micAuthorization());
    juce::String error;
    std::unique_ptr<mac::SystemAudioTap> tap;
    if (o.tap) {
        tap = mac::SystemAudioTap::create(error);
        if (!tap) {
            std::printf("error: %s\n", error.toRawUTF8());
            return 1;
        }
        std::printf("tap: id %u, uid %s, %.0f Hz, %d ch\n", tap->id(), tap->uid().toRawUTF8(), tap->sampleRate(),
                    tap->channels());
    }

    AggregateProbe probe;
    auto callback = [&probe](const float* mic, const float* ref, float* outL, float* outR, int n) {
        const std::uint64_t now = mach_absolute_time();
        const std::uint64_t previous = probe.lastHost.exchange(now, std::memory_order_relaxed);
        if (previous != 0 && probe.measuring.load(std::memory_order_relaxed)) {
            const std::uint64_t interval = now - previous;
            if (interval > probe.maxIntervalTicks.load(std::memory_order_relaxed))
                probe.maxIntervalTicks.store(interval, std::memory_order_relaxed);
            if (static_cast<double>(interval) > 2.0 * probe.bufferTicks)
                probe.intervalsOverTwoBuffers.fetch_add(1, std::memory_order_relaxed);
        }
        float refPeak = 0.0f, micPeak = 0.0f;
        std::uint64_t nonFinite = 0;
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(mic[i]) || !std::isfinite(ref[i]))
                ++nonFinite;
            refPeak = std::max(refPeak, std::fabs(ref[i]));
            micPeak = std::max(micPeak, std::fabs(mic[i]));
            outL[i] = mic[i];
            outR[i] = mic[i];
        }
        atomicMax(probe.refPeak, refPeak);
        atomicMax(probe.micPeak, micPeak);
        atomicMax(probe.refPeakTotal, refPeak);
        if (nonFinite != 0)
            probe.nonFiniteSamples.fetch_add(nonFinite, std::memory_order_relaxed);
        probe.callbacks.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t frames =
            probe.frames.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed) + n;
        if (probe.measuring.load(std::memory_order_relaxed)
            && probe.firstMeasuredHost.load(std::memory_order_relaxed) == 0) {
            probe.firstMeasuredFrames.store(frames, std::memory_order_relaxed);
            probe.firstMeasuredHost.store(now, std::memory_order_relaxed);
        }
    };

    auto io = mac::AggregateIO::create({juce::String(o.micUid), o.channel, tap.get(), o.buffer}, callback, error);
    if (!io) {
        std::printf("error: %s\n", error.toRawUTF8());
        return 1;
    }
    int deviceLost = 0;
    io->onDeviceLost = [&deviceLost] {
        ++deviceLost;
        std::printf("  ! onDeviceLost\n");
    };
    std::printf("aggregate: id %u, buffer %d frames\n%s\n", io->deviceId(), io->bufferFrames(),
                io->describe().toRawUTF8());
    const double bufferSeconds = io->bufferFrames() / devices::kSampleRate;
    probe.bufferTicks = bufferSeconds / hostTicksToSeconds(1);

    if (!io->start()) {
        std::printf("error: %s\n", io->lastError().toRawUTF8());
        return 1;
    }
    // Warm-up: the first cycles after AudioDeviceStart are not representative.
    runMainLoopFor(0.5, {});
    if (probe.callbacks.load() == 0) {
        runMainLoopFor(2.0, [&] { return probe.callbacks.load() > 0; });
        if (probe.callbacks.load() == 0) {
            std::printf("error: no IO callbacks 2.5 s after start (compare with --no-tap; check the microphone and "
                        "\"System Audio Recording\" permissions of the app that launched fxprobe)\n");
            io->stop();
            return 1;
        }
    }
    const auto warm = io->stats();
    probe.maxIntervalTicks = 0;
    probe.measuring = true;

    int secondsWithRef = 0, reports = 0;
    const double total = o.seconds;
    for (int second = 1; second <= static_cast<int>(std::ceil(total)) && !gInterrupted; ++second) {
        runMainLoopFor(std::min(1.0, total - (second - 1)), {});
        const auto st = io->stats();
        const float ref = probe.refPeak.exchange(0.0f);
        const float mic = probe.micPeak.exchange(0.0f);
        ++reports;
        if (toDb(ref) > -60.0)
            ++secondsWithRef;
        std::printf("%4d s  callbacks %7llu  frames %9llu  ref peak %7.1f dBFS  mic peak %7.1f dBFS  overloads %llu"
                    "  sample-time gaps %llu  max interval %.2f ms\n",
                    second, static_cast<unsigned long long>(probe.callbacks.load()),
                    static_cast<unsigned long long>(probe.frames.load()), toDb(ref), toDb(mic),
                    static_cast<unsigned long long>(st.overloads),
                    static_cast<unsigned long long>(st.sampleTimeGaps - warm.sampleTimeGaps),
                    hostTicksToSeconds(probe.maxIntervalTicks.load()) * 1000.0);
        std::fflush(stdout);
    }
    const double actualRate = io->actualSampleRate();
    const auto lat = io->latency();
    io->stop();  // no more callbacks: the probe values below are consistent
    const auto st = io->stats();

    // Frames delivered by the callbacks after the first measured one, over the host time between
    // that callback and the last one: unbiased (each side counts whole callbacks).
    const std::uint64_t hostAtStart = probe.firstMeasuredHost.load();
    const double elapsed = hostAtStart != 0 ? hostTicksToSeconds(probe.lastHost.load() - hostAtStart) : 0.0;
    const double measuredRate =
        elapsed > 0 ? static_cast<double>(probe.frames.load() - probe.firstMeasuredFrames.load()) / elapsed : 0.0;
    const double maxIntervalMs = hostTicksToSeconds(probe.maxIntervalTicks.load()) * 1000.0;
    const std::uint64_t gaps = st.sampleTimeGaps - warm.sampleTimeGaps;
    const std::uint64_t overloads = st.overloads;
    const float refTotal = probe.refPeakTotal.load();

    std::printf("\nlatency(): rate %.0f, input device %d, output device %d, buffer %d frames (aec %d, plugins %d) "
                "= %.2f ms\n",
                lat.sampleRate, lat.inputDeviceFrames, lat.outputDeviceFrames, lat.bufferFrames, lat.aecFrames,
                lat.pluginFrames, lat.totalMs());
    std::printf("callbacks %llu, buffer frames last %u max %u, non-finite samples %llu, device lost %d\n",
                static_cast<unsigned long long>(st.callbacks), st.lastFrames, st.maxFrames,
                static_cast<unsigned long long>(probe.nonFiniteSamples.load()), deviceLost);

    const bool cadenceOk = probe.intervalsOverTwoBuffers.load() == 0 && gaps == 0;
    const bool refOk = !o.tap || toDb(refTotal) > -60.0;
    const bool overloadOk = overloads == 0;
    const bool rateOk = std::abs(io->nominalSampleRate() - devices::kSampleRate) < 0.5
                        && std::abs(measuredRate - devices::kSampleRate) < 0.001 * devices::kSampleRate;
    const bool lostOk = deviceLost == 0;
    std::printf("[a] cadence: max callback interval %.2f ms (buffer %.2f ms, limit %.2f ms), intervals > 2 buffers "
                "%llu, sample-time gaps %llu => %s\n",
                maxIntervalMs, bufferSeconds * 1000.0, 2 * bufferSeconds * 1000.0,
                static_cast<unsigned long long>(probe.intervalsOverTwoBuffers.load()),
                static_cast<unsigned long long>(gaps), cadenceOk ? "PASS" : "FAIL");
    if (o.tap)
        std::printf("[b] reference: peak %.1f dBFS over the run, %d/%d seconds above -60 dBFS => %s\n", toDb(refTotal),
                    secondsWithRef, reports, refOk ? "PASS" : "FAIL");
    else
        std::printf("[b] reference: no tap (--no-tap) => skipped\n");
    std::printf("[c] kAudioDeviceProcessorOverload notifications: %llu => %s\n",
                static_cast<unsigned long long>(overloads), overloadOk ? "PASS" : "FAIL");
    std::printf("[d] rate: nominal %.0f, measured %.2f Hz over %.2f s, HAL actual %.2f => %s\n",
                io->nominalSampleRate(), measuredRate, elapsed, actualRate, rateOk ? "PASS" : "FAIL");
    std::printf("[e] device lost notifications: %d => %s\n", deviceLost, lostOk ? "PASS" : "FAIL");
    const bool all = cadenceOk && refOk && overloadOk && rateOk && lostOk;
    std::printf("RESULT: %s\n", all ? "PASS" : "FAIL");

    io.reset();
    tap.reset();
    CFRunLoopRemoveTimer(CFRunLoopGetMain(), keepAlive, kCFRunLoopDefaultMode);
    CFRelease(keepAlive);
    return all ? 0 : 1;
}


// ---- engine-test -----------------------------------------------------------------------------

struct EngineTestOptions {
    std::string micUid;
    double seconds = 30.0;
    bool aec = true;
    int buffer = 128;
    int channel = 0;
};

std::optional<EngineTestOptions> parseEngineTest(int argc, const char* argv[])
{
    if (argc < 3 || argv[2][0] == '-')
        return std::nullopt;
    EngineTestOptions o;
    o.micUid = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--no-aec")
            o.aec = false;
        else if (arg == "--seconds" && i + 1 < argc)
            o.seconds = std::atof(argv[++i]);
        else if (arg == "--buffer" && i + 1 < argc)
            o.buffer = std::atoi(argv[++i]);
        else if (arg == "--channel" && i + 1 < argc)
            o.channel = std::atoi(argv[++i]);
        else
            return std::nullopt;
    }
    if (o.seconds <= 0 || o.buffer <= 0 || o.channel < 0)
        return std::nullopt;
    return o;
}

double epochSeconds()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int cmdEngineTest(const EngineTestOptions& o)
{
    namespace mac = fixomic::mac;
    using fixomic::EngineController;
    using fixomic::EngineState;
    std::signal(SIGINT, [](int) { gInterrupted = true; });
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [t0] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

    fixomic::Config config;
    config.inputDeviceUid = o.micUid;
    config.inputChannel = o.channel;
    config.bufferSize = o.buffer;
    config.aecEnabled = o.aec;

    fixomic::AecStage aec;
    fixomic::PluginChain chain;
    fixomic::ProcessingGraph graph(aec, chain);
    mac::MacEngineBackend backend(graph, aec, chain, [&config] { return config; });
    EngineController engine(backend, elapsed);

    std::printf("engine-test: mic %s, channel %d, buffer %d, AEC %s, %.0f s (microphone permission: %s)\n",
                o.micUid.c_str(), o.channel, o.buffer, o.aec ? "on" : "off", o.seconds, micAuthorization());
    std::printf("times: s since start (epoch s)\n");

    // Raw demand, polled every 5 ms on a background thread: the reference the controller's 200 ms
    // DemandMonitor is timed against.
    std::atomic<bool> pollerStop{false};
    std::mutex eventsMutex;
    struct Event {
        double t;
        bool rawDemand;   // true: raw IsRunningSomewhere flip; false: engine state change
        bool value;
        EngineState state;
    };
    std::vector<Event> events;
    std::thread poller([&] {
        const AudioObjectID mic = findDevice(devices::kMicUid);
        UInt32 last = 0;
        while (!pollerStop) {
            const UInt32 now = mic != kAudioObjectUnknown ? isRunningSomewhere(mic) : 0;
            if (now != last) {
                last = now;
                const double t = elapsed();
                std::printf("%8.3f s (%.3f)  raw IsRunningSomewhere=%u\n", t, epochSeconds(), now);
                std::lock_guard<std::mutex> lock(eventsMutex);
                events.push_back({t, true, now != 0, EngineState::Idle});
            }
            std::this_thread::sleep_for(5ms);
        }
    });

    engine.onStateChanged = [&](EngineState state) {
        const double t = elapsed();
        std::printf("%8.3f s (%.3f)  state -> %s%s%s\n", t, epochSeconds(), fixomic::toString(state),
                    state == EngineState::Error ? ": " : "",
                    state == EngineState::Error ? engine.lastError().toRawUTF8() : "");
        std::lock_guard<std::mutex> lock(eventsMutex);
        events.push_back({t, false, false, state});
    };
    backend.onWarning = [&](juce::String warning) {
        std::printf("%8.3f s  warning: %s\n", elapsed(), warning.toRawUTF8());
    };
    backend.onDeviceLost = [&] {
        std::printf("%8.3f s  device lost\n", elapsed());
        engine.onDeviceLost();
    };
    mac::DemandMonitor demand([&](bool inUse) {
        std::printf("%8.3f s (%.3f)  DemandMonitor inUse=%d\n", elapsed(), epochSeconds(), inUse ? 1 : 0);
        engine.onDemandChanged(inUse);
    });
    mac::WakeObserver wake([&] {
        std::printf("%8.3f s  system wake\n", elapsed());
        engine.onSystemWake();
    });

    engine.setConfig(config);
    std::printf("%8.3f s  initial state %s\n", elapsed(), fixomic::toString(engine.state()));
    demand.start();

    // tick() every 200 ms, a status line every second while the IO is open. (Blocks copy captured
    // C++ objects: capture pointers.)
    EngineController* enginePtr = &engine;
    mac::MacEngineBackend* backendPtr = &backend;
    fixomic::ProcessingGraph* graphPtr = &graph;
    __block int tickCount = 0;
    CFRunLoopTimerRef ticker = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.2, 0.2, 0, 0, ^(CFRunLoopTimerRef) {
            enginePtr->tick();
            if (++tickCount % 5 != 0 || !backendPtr->ioOpen())
                return;
            const auto st = backendPtr->io()->stats();
            const auto lat = backendPtr->latency();
            std::printf("%8.3f s    running: callbacks %llu, in peak %6.1f dBFS, out peak %6.1f dBFS, AEC %s, "
                        "latency %.2f ms, overloads %llu\n",
                        elapsed(), static_cast<unsigned long long>(st.callbacks), graphPtr->inputMeter.peakDb(),
                        graphPtr->outputMeter.peakDb(), graphPtr->aecActive() ? "active" : "inactive",
                        lat ? lat->totalMs() : 0.0, static_cast<unsigned long long>(st.overloads));
        });
    CFRunLoopAddTimer(CFRunLoopGetMain(), ticker, kCFRunLoopDefaultMode);
    runMainLoopFor(o.seconds, {});
    CFRunLoopRemoveTimer(CFRunLoopGetMain(), ticker, kCFRunLoopDefaultMode);
    CFRelease(ticker);

    demand.stop();
    pollerStop = true;
    poller.join();
    const EngineState finalState = engine.state();
    engine.onStateChanged = nullptr;

    // Each raw demand flip against the first matching engine transition after it.
    std::printf("\nsummary (final state %s):\n", fixomic::toString(finalState));
    for (size_t i = 0; i < events.size(); ++i) {
        if (!events[i].rawDemand)
            continue;
        const EngineState wanted = events[i].value ? EngineState::Running : EngineState::Idle;
        double after = -1.0;
        for (size_t j = i + 1; j < events.size(); ++j) {
            if (events[j].rawDemand)
                break;
            if (events[j].state == wanted) {
                after = events[j].t - events[i].t;
                break;
            }
        }
        if (after >= 0)
            std::printf("  demand %s at %.3f s -> %s %.0f ms later\n", events[i].value ? "on " : "off",
                        events[i].t, fixomic::toString(wanted), after * 1000.0);
        else
            std::printf("  demand %s at %.3f s -> no %s before the next demand change\n",
                        events[i].value ? "on " : "off", events[i].t, fixomic::toString(wanted));
    }
    return 0;
}

int usage()
{
    std::fprintf(stderr, "usage: fxprobe list | check-driver | running [seconds] | loopback-test\n"
                         "       fxprobe aggregate-test <micUid> [--no-tap] [--seconds N] [--buffer N] [--channel N]\n"
                         "       fxprobe engine-test <micUid> [--seconds N] [--no-aec] [--buffer N] [--channel N]\n");
    return 2;
}

} // namespace

int main(int argc, const char* argv[])
{
    @autoreleasepool {
        if (argc < 2)
            return usage();
        const std::string command = argv[1];
        if (command == "list")
            return cmdList();
        if (command == "check-driver")
            return cmdCheckDriver();
        if (command == "running")
            return cmdRunning(argc > 2 ? std::atof(argv[2]) : 0.0);
        if (command == "loopback-test")
            return cmdLoopback();
        if (command == "hold-mic" && argc > 2)
            return cmdHoldMic(std::atof(argv[2]));
        if (command == "aggregate-test") {
            const auto options = parseAggregateTest(argc, argv);
            return options ? cmdAggregateTest(*options) : usage();
        }
        if (command == "engine-test") {
            const auto options = parseEngineTest(argc, argv);
            return options ? cmdEngineTest(*options) : usage();
        }
        return usage();
    }
}
