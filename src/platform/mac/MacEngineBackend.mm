#include "platform/mac/MacEngineBackend.h"

#include "dsp/AecStage.h"
#include "dsp/PluginChain.h"
#include "dsp/ProcessingGraph.h"
#include "platform/mac/AggregateIO.h"
#include "platform/mac/CoreAudioDevices.h"
#include "platform/mac/CoreAudioUtil.h"
#include "platform/mac/DriverInstaller.h"
#include "platform/mac/FixomicDevices.h"
#include "platform/mac/SystemAudioTap.h"
#include "platform/mac/TapSilenceWatch.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>

namespace fixomic::mac {

namespace {

constexpr const char* kSystemAudioSettings =
    "Impostazioni di Sistema \u2192 Privacy e sicurezza \u2192 Registrazione schermo e audio di sistema";

bool defaultOutputRunning()
{
    const auto device = util::getScalar<AudioObjectID>(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice);
    return device && util::getScalar<UInt32>(*device, kAudioDevicePropertyDeviceIsRunningSomewhere).value_or(0) != 0;
}

} // namespace

const char* const MacEngineBackend::kWaitingForMicrophone = "In attesa del permesso per il microfono";
const char* const MacEngineBackend::kWaitingForSystemAudio = "In attesa del permesso per l'audio di sistema";

juce::String MacEngineBackend::systemAudioDeniedWarning()
{
    return juce::String::fromUTF8("Permesso audio di sistema negato: la cancellazione dell'eco \u00e8 disattivata. "
                                  "Abilitalo in ")
           + juce::String::fromUTF8(kSystemAudioSettings) + ".";
}

juce::String MacEngineBackend::systemAudioUnavailableWarning(const juce::String& reason)
{
    return juce::String::fromUTF8("Audio di sistema non disponibile, la cancellazione dell'eco \u00e8 disattivata: ")
           + reason;
}

juce::String MacEngineBackend::systemAudioSilentWarning()
{
    return juce::String::fromUTF8("Il permesso audio di sistema \u00e8 probabilmente negato: fixomic riceve solo "
                                  "silenzio mentre il Mac riproduce audio, quindi la cancellazione dell'eco non "
                                  "funziona. Controlla ")
           + juce::String::fromUTF8(kSystemAudioSettings) + ".";
}

// A main-queue timer feeding a TapSilenceWatch while an IO with a tap is open.
struct MacEngineBackend::SilenceTimer {
    dispatch_source_t source = nil;
    TapSilenceWatch watch;
    ~SilenceTimer()
    {
        if (source != nil)
            dispatch_source_cancel(source);   // main thread: no handler runs after this
    }
};

MacEngineBackend::MacEngineBackend(ProcessingGraph& graph, AecStage& aec, PluginChain& chain,
                                   std::function<Config()> configProvider, MicrophoneAccess micAccess,
                                   SystemAudioAccess audioAccess)
    : graph_(graph), aec_(aec), chain_(chain), configProvider_(std::move(configProvider)),
      micAccess_(std::move(micAccess)), audioAccess_(std::move(audioAccess))
{
}

MacEngineBackend::~MacEngineBackend()
{
    *alive_ = false;
    closeIO();
}

bool MacEngineBackend::driverInstalled()
{
    return mac::driverInstalled();
}

bool MacEngineBackend::micAvailable(const juce::String& uid)
{
    if (uid.isEmpty() || uid.startsWith("fixomic_"))
        return false;
    const auto device = findDeviceByUid(uid);
    return device && util::channelCount(*device, kAudioObjectPropertyScopeInput) > 0;
}

bool MacEngineBackend::micSupports48k(const juce::String& uid)
{
    const auto device = findDeviceByUid(uid);
    if (!device)
        return false;
    const auto rates = util::nominalRates(*device);
    return std::any_of(rates.begin(), rates.end(),
                       [](double rate) { return std::abs(rate - devices::kSampleRate) < 0.5; });
}

bool MacEngineBackend::openIO(bool withTap, juce::String& error)
{
    closeIO();
    pending_ = PendingPermission::None;

    // Never reach the HAL without microphone access: while the TCC prompt is up, creating the IO
    // proc blocks the calling (main) thread until the user answers.
    switch (micAccess_.status ? micAccess_.status() : Permission::Granted) {
    case Permission::Granted: break;
    case Permission::NotDetermined:
        if (!micRequested_ && micAccess_.request) {
            micRequested_ = true;
            micAccess_.request([this, alive = alive_](bool granted) {
                if (*alive && onMicrophonePermission)
                    onMicrophonePermission(granted);
            });
        }
        error = kWaitingForMicrophone;
        pending_ = PendingPermission::Microphone;
        return false;
    case Permission::Denied:
        error = juce::String::fromUTF8("Permesso microfono negato: abilitalo in Impostazioni di Sistema \u2192 "
                                       "Privacy e sicurezza \u2192 Microfono");
        return false;
    }
    if (!checkSystemAudio(withTap, error))
        return false;

    const Config config = configProvider_ ? configProvider_() : Config{};
    const int block = std::max(config.bufferSize, 1);

    // The IO is closed: nothing runs process() while these (allocating) calls run. Re-preparing
    // the chain would call prepareToPlay on every plugin (slow, and it resets their state) at every
    // start, so it happens only when the block size changes (the rate is always 48 kHz). The AEC
    // is reset instead of re-prepared: same buffers, fresh APM state.
    if (block != preparedBlock_) {
        aec_.prepare(devices::kSampleRate, block);
        chain_.prepare(devices::kSampleRate, block);
        preparedBlock_ = block;   // prepares cannot fail: retries after a failed open skip them
    } else {
        aec_.reset();
    }
    graph_.prepare(devices::kSampleRate, block);
    graph_.setAecEnabled(config.aecEnabled);
    graph_.setBypass(config.globalBypass);
    graph_.setMicPresent(true);

    if (withTap) {
        juce::String tapError;
        tap_ = SystemAudioTap::create(tapError);
        if (!tap_ && onWarning)
            onWarning(systemAudioUnavailableWarning(tapError));
    }

    // Without a tap the aggregate hands the callback a silent reference: pass none to the graph so
    // the AEC stays inactive (and adds no delay).
    ProcessingGraph* graph = &graph_;
    std::atomic<bool>* heard = &refHeard_;
    const bool hasReference = tap_ != nullptr;
    refHeard_ = false;
    auto io = AggregateIO::create(
        {config.inputDeviceUid, config.inputChannel, tap_.get(), block},
        [graph, heard, hasReference](const float* mic, const float* ref, float* outL, float* outR, int n) {
            if (hasReference && !heard->load(std::memory_order_relaxed)) {
                for (int i = 0; i < n; ++i)
                    if (ref[i] != 0.0f) {
                        heard->store(true, std::memory_order_relaxed);
                        break;
                    }
            }
            graph->process(mic, hasReference ? ref : nullptr, outL, outR, n);
        },
        error);
    if (!io) {
        tap_.reset();
        return false;
    }
    io->onDeviceLost = [this] {
        const auto handler = onDeviceLost;   // a copy: the handler may close the IO
        if (handler)
            handler();
    };

    graph_.armStartupMute(kStartupMuteSamples, kStartupFadeSamples);
    if (!io->start()) {
        error = io->lastError();
        io.reset();
        tap_.reset();
        return false;
    }
    io_ = std::move(io);
    if (tap_ && (audioAccess_.status ? audioAccess_.status() : Permission::Granted) != Permission::Granted)
        startSilenceWatch();
    return true;
}

bool MacEngineBackend::checkSystemAudio(bool& withTap, juce::String& error)
{
    if (!withTap)
        return true;
    switch (audioAccess_.status ? audioAccess_.status() : Permission::Granted) {
    case Permission::Granted: return true;
    case Permission::Denied:
        withTap = false;
        if (onWarning)
            onWarning(systemAudioDeniedWarning());
        return true;
    case Permission::NotDetermined: break;
    }
    // Not determined, or unknown (no preflight, or its value is stale after the prompt in this
    // process): after one prompt the real tap is created as usual.
    if (audioRequest_ == Request::Done || !audioAccess_.request)
        return true;
    if (audioRequest_ == Request::Failed) {
        // The throwaway IO failed, so the prompt may still be pending: a tap IOProc created here
        // would raise it on the main thread. This open goes without the tap; the next one asks again.
        audioRequest_ = Request::None;
        withTap = false;
        if (onWarning)
            onWarning(systemAudioUnavailableWarning(audioRequestError_));
        return true;
    }
    if (audioRequest_ == Request::None) {
        audioRequest_ = Request::Pending;
        audioAccess_.request([this, alive = alive_](bool ok, juce::String requestError) {
            if (!*alive)
                return;
            audioRequest_ = ok ? Request::Done : Request::Failed;
            audioRequestError_ = requestError.isNotEmpty() ? requestError : juce::String("unknown error");
            if (onSystemAudioPermission)
                onSystemAudioPermission();   // re-evaluated at once: with the tap, or without it
        });
    }
    error = kWaitingForSystemAudio;
    pending_ = PendingPermission::SystemAudio;
    return false;
}

void MacEngineBackend::startSilenceWatch()
{
    auto timer = std::make_unique<SilenceTimer>();
    timer->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    const auto interval = static_cast<int64_t>(kSilenceCheckSeconds * NSEC_PER_SEC);
    dispatch_source_set_timer(timer->source, dispatch_time(DISPATCH_TIME_NOW, interval), static_cast<uint64_t>(interval),
                              static_cast<uint64_t>(interval / 10));
    SilenceTimer* raw = timer.get();
    dispatch_source_set_event_handler(timer->source, ^{
        // Main queue; the timer is cancelled (closeIO) before `this` or `raw` go away.
        const bool heardSignal = refHeard_.exchange(false);
        const bool running = audioAccess_.outputRunning ? audioAccess_.outputRunning() : defaultOutputRunning();
        if (raw->watch.update(heardSignal, running, kSilenceCheckSeconds) && onWarning)
            onWarning(systemAudioSilentWarning());
    });
    dispatch_resume(timer->source);
    silenceTimer_ = std::move(timer);
}

void MacEngineBackend::stopSilenceWatch()
{
    silenceTimer_.reset();
}

void MacEngineBackend::forgetPrepared()
{
    preparedBlock_ = 0;
}

void MacEngineBackend::applyFlags(bool aecEnabled, bool bypass)
{
    graph_.setAecEnabled(aecEnabled);
    graph_.setBypass(bypass);
}

void MacEngineBackend::closeIO()
{
    stopSilenceWatch();
    if (io_) {
        io_->stop();
        io_.reset();
    }
    tap_.reset();
}

std::optional<LatencyBreakdown> MacEngineBackend::latency() const
{
    if (!io_)
        return std::nullopt;
    LatencyBreakdown breakdown = io_->latency();
    breakdown.aecFrames = graph_.aecActive() ? aec_.latencySamples() : 0;
    const bool bypassed = configProvider_ && configProvider_().globalBypass;
    breakdown.pluginFrames = bypassed ? 0 : chain_.totalLatencySamples();
    return breakdown;
}

bool MacEngineBackend::ioOpen() const
{
    return io_ != nullptr;
}

bool MacEngineBackend::tapActive() const
{
    return io_ != nullptr && tap_ != nullptr;
}

PendingPermission MacEngineBackend::pendingPermission() const
{
    return pending_;
}

const AggregateIO* MacEngineBackend::io() const
{
    return io_.get();
}

} // namespace fixomic::mac
