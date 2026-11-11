#include "dsp/AecStage.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>

#include <api/audio/audio_processing.h>

#include "dsp/FrameFifo.h"

namespace fixomic {

namespace {
// Frames with a far-end reference quieter than this (mean square, = -60 dBFS RMS) carry no echo
// worth measuring: ERLE holds its last value.
constexpr float kMinReferenceEnergy = 1.0e-6f;
// One-pole smoothing of the per-frame energies (about 200 ms at 10 ms frames).
constexpr float kErleSmoothing = 0.05f;
constexpr float kEnergyFloor = 1.0e-12f;

float meanSquare(const float* x, int n) noexcept {
    float acc = 0.0f;
    for (int i = 0; i < n; ++i) acc += x[i] * x[i];
    return acc / static_cast<float>(n);
}
} // namespace

struct AecStage::Impl {
    Impl(int frameSize, int maxBlock)
        : frame(frameSize),
          maxBlockSize(maxBlock),
          micIn(maxBlock + frameSize),
          refIn(maxBlock + frameSize),
          out(maxBlock + 2 * frameSize),
          micFrame(static_cast<size_t>(frameSize)),
          refFrame(static_cast<size_t>(frameSize)),
          outFrame(static_cast<size_t>(frameSize)) {}

    static rtc::scoped_refptr<webrtc::AudioProcessing> makeApm() {
        webrtc::AudioProcessing::Config config;
        config.echo_canceller.enabled = true;
        config.echo_canceller.mobile_mode = false;
        config.high_pass_filter.enabled = true;
        config.noise_suppression.enabled = false;
        config.gain_controller1.enabled = false;
        config.gain_controller2.enabled = false;

        auto instance = webrtc::AudioProcessingBuilder().SetConfig(config).Create();
        instance->set_stream_delay_ms(0);
        return instance;
    }

    // Creates an APM that is fully initialised for the stream format. The APM otherwise
    // initialises lazily (and allocates) on its first frame, so the format is set explicitly and a
    // silent frame pair is pushed through here, off the audio thread.
    static rtc::scoped_refptr<webrtc::AudioProcessing> makeWarmApm(const webrtc::StreamConfig& sc) {
        auto instance = makeApm();
        webrtc::ProcessingConfig processingConfig;
        for (auto& stream : processingConfig.streams) stream = sc;
        instance->Initialize(processingConfig);

        std::vector<float> silence(sc.num_frames(), 0.0f);
        const float* in[1] = {silence.data()};
        float* outPtr[1] = {silence.data()};
        instance->ProcessReverseStream(in, sc, sc, outPtr);
        instance->ProcessStream(in, sc, sc, outPtr);
        return instance;
    }

    void createApm() {
        streamConfig = webrtc::StreamConfig(sampleRate, 1);
        apm = makeWarmApm(streamConfig);
    }

    // The APM itself delays the capture path (filter banks + AEC3 block processing). Measure it
    // once per sample rate with a throw-away instance: feed an impulse and locate the peak.
    int measureApmDelay() const {
        auto probe = makeWarmApm(streamConfig);
        std::vector<float> in(static_cast<size_t>(frame), 0.0f);
        std::vector<float> reverse(static_cast<size_t>(frame), 0.0f);
        std::vector<float> res(static_cast<size_t>(frame));
        in[0] = 1.0f;

        int best = 0;
        float bestValue = 0.0f;
        for (int f = 0; f < 4; ++f) {
            const float* revIn[1] = {reverse.data()};
            float* revOut[1] = {reverse.data()};
            probe->ProcessReverseStream(revIn, streamConfig, streamConfig, revOut);
            const float* src[1] = {in.data()};
            float* dst[1] = {res.data()};
            probe->ProcessStream(src, streamConfig, streamConfig, dst);
            for (int i = 0; i < frame; ++i) {
                if (std::abs(res[static_cast<size_t>(i)]) > std::abs(bestValue)) {
                    bestValue = res[static_cast<size_t>(i)];
                    best = f * frame + i;
                }
            }
            in[0] = 0.0f;
        }
        return best;
    }

    void resetState() {
        micIn.clear();
        refIn.clear();
        out.clear();
        out.pushZeros(frame); // fixed one-frame latency
        smoothedMicEnergy = 0.0f;
        smoothedOutEnergy = 0.0f;
        erleValid = false;
        erle.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
    }

    // Consumes every complete frame sitting in the input FIFOs.
    void processFrames() noexcept {
        while (micIn.size() >= frame && refIn.size() >= frame) {
            micIn.pop(micFrame.data(), frame);
            refIn.pop(refFrame.data(), frame);

            const float refEnergy = meanSquare(refFrame.data(), frame);

            const float* refChannel[1] = {refFrame.data()};
            float* refOutChannel[1] = {refFrame.data()};
            apm->ProcessReverseStream(refChannel, streamConfig, streamConfig, refOutChannel);

            const float* micChannel[1] = {micFrame.data()};
            float* outChannel[1] = {outFrame.data()};
            const int result = apm->ProcessStream(micChannel, streamConfig, streamConfig, outChannel);
            if (result != webrtc::AudioProcessing::kNoError)
                std::copy_n(micFrame.data(), frame, outFrame.data()); // never re-emit a stale frame

            out.push(outFrame.data(), frame);
            updateErle(refEnergy, meanSquare(micFrame.data(), frame), meanSquare(outFrame.data(), frame));
        }
    }

    // Own ERLE measurement: smoothed 10*log10(E_mic / E_out), only while a reference is playing.
    void updateErle(float refEnergy, float micEnergy, float outEnergy) noexcept {
        if (refEnergy <= kMinReferenceEnergy) return;
        if (!erleValid) {
            smoothedMicEnergy = micEnergy;
            smoothedOutEnergy = outEnergy;
            erleValid = true;
        } else {
            smoothedMicEnergy += kErleSmoothing * (micEnergy - smoothedMicEnergy);
            smoothedOutEnergy += kErleSmoothing * (outEnergy - smoothedOutEnergy);
        }
        erle.store(10.0f * std::log10((smoothedMicEnergy + kEnergyFloor) / (smoothedOutEnergy + kEnergyFloor)),
                   std::memory_order_relaxed);
    }

    int frame;
    int maxBlockSize;
    FrameFifo<float> micIn;
    FrameFifo<float> refIn;
    FrameFifo<float> out;
    std::vector<float> micFrame;
    std::vector<float> refFrame;
    std::vector<float> outFrame;
    int sampleRate = 0;
    int apmDelay = 0;
    float smoothedMicEnergy = 0.0f;
    float smoothedOutEnergy = 0.0f;
    bool erleValid = false;
    std::atomic<float> erle{std::numeric_limits<float>::quiet_NaN()};
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig streamConfig;
};

AecStage::AecStage() = default;
AecStage::~AecStage() = default;

void AecStage::prepare(double sampleRate, int maxBlockSize) {
    impl_.reset();
    // Outside the range the APM can handle (or a frame of 0): stay in pass-through.
    if (!(sampleRate >= 8000.0 && sampleRate <= 192000.0)) return;
    const int rate = static_cast<int>(std::lround(sampleRate));
    if (rate / 100 <= 0) return;
    impl_ = std::make_unique<Impl>(rate / 100, std::max(maxBlockSize, 1));
    impl_->sampleRate = rate;
    impl_->streamConfig = webrtc::StreamConfig(rate, 1);
    impl_->apmDelay = impl_->measureApmDelay();
    impl_->createApm();
    impl_->resetState();
}

void AecStage::reset() {
    if (!impl_) return;
    impl_->createApm();
    impl_->resetState();
}

void AecStage::process(const float* mic, const float* ref, float* out, int n) noexcept {
    if (!impl_) {
        // Not prepared: behave as a pass-through rather than emit garbage.
        if (out != mic) std::copy_n(mic, std::max(n, 0), out);
        return;
    }

    Impl& s = *impl_;
    int done = 0;
    while (done < n) {
        const int chunk = std::min(n - done, s.maxBlockSize);
        s.micIn.push(mic + done, chunk);
        s.refIn.push(ref + done, chunk);
        s.processFrames();
        s.out.pop(out + done, chunk); // always available: out holds >= 1 frame of priming
        done += chunk;
    }
}

int AecStage::latencySamples() const noexcept { return impl_ ? impl_->frame + impl_->apmDelay : 0; }

int AecStage::apmDelaySamples() const noexcept { return impl_ ? impl_->apmDelay : 0; }

float AecStage::erleDb() const noexcept {
    return impl_ ? impl_->erle.load(std::memory_order_relaxed) : std::numeric_limits<float>::quiet_NaN();
}

} // namespace fixomic
