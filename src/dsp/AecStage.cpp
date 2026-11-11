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
// ERLE is refreshed about every 0.5 s of audio; GetStatistics() is not free.
constexpr int kStatsIntervalFrames = 50;
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

    void createApm() {
        apm = makeApm();
        streamConfig = webrtc::StreamConfig(sampleRate, 1);
    }

    // The APM itself delays the capture path (filter banks + AEC3 block processing). Measure it
    // once per sample rate with a throw-away instance: feed an impulse and locate the peak.
    int measureApmDelay() const {
        auto probe = makeApm();
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
        framesSinceStats = 0;
        erle.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
    }

    // Consumes every complete frame sitting in the input FIFOs.
    void processFrames() noexcept {
        while (micIn.size() >= frame && refIn.size() >= frame) {
            micIn.pop(micFrame.data(), frame);
            refIn.pop(refFrame.data(), frame);

            const float* refChannel[1] = {refFrame.data()};
            float* refOutChannel[1] = {refFrame.data()};
            apm->ProcessReverseStream(refChannel, streamConfig, streamConfig, refOutChannel);

            const float* micChannel[1] = {micFrame.data()};
            float* outChannel[1] = {outFrame.data()};
            apm->ProcessStream(micChannel, streamConfig, streamConfig, outChannel);

            out.push(outFrame.data(), frame);

            if (++framesSinceStats >= kStatsIntervalFrames) {
                framesSinceStats = 0;
                const auto stats = apm->GetStatistics();
                erle.store(stats.echo_return_loss_enhancement.has_value()
                               ? static_cast<float>(*stats.echo_return_loss_enhancement)
                               : std::numeric_limits<float>::quiet_NaN(),
                           std::memory_order_relaxed);
            }
        }
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
    int framesSinceStats = 0;
    std::atomic<float> erle{std::numeric_limits<float>::quiet_NaN()};
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig streamConfig;
};

AecStage::AecStage() = default;
AecStage::~AecStage() = default;

void AecStage::prepare(double sampleRate, int maxBlockSize) {
    const int rate = static_cast<int>(std::lround(sampleRate));
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

int AecStage::latencySamples() const noexcept { return impl_ ? impl_->frame : 0; }

int AecStage::apmDelaySamples() const noexcept { return impl_ ? impl_->apmDelay : 0; }

float AecStage::erleDb() const noexcept {
    return impl_ ? impl_->erle.load(std::memory_order_relaxed) : std::numeric_limits<float>::quiet_NaN();
}

} // namespace fixomic
