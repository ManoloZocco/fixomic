#pragma once

namespace fixomic::mac {

// The fallback check for a denied System Audio Recording permission (no public status API): a
// tap without the permission delivers digital silence. update() is fed about once a second with
// whether the tap delivered any non-zero sample since the last call and whether the default output
// device is running (some app plays audio). It returns true once (per watch) when the tap stayed
// silent for kSilentSeconds of running output (not necessarily contiguous: output that stops
// pauses the count). A single non-zero sample proves the permission works: the watch then never
// warns. A warning is not proof of a denial: a running output that plays digital silence (a call
// where nobody speaks) looks the same, so its text never claims one (MacEngineBackend). Counting
// only after a first non-zero sample is no way out: a denied tap never delivers one.
class TapSilenceWatch {
public:
    static constexpr double kSilentSeconds = 10.0;

    bool update(bool heardSignal, bool outputRunning, double elapsedSeconds)
    {
        if (settled_)
            return false;
        if (heardSignal) {
            settled_ = true;
            return false;
        }
        if (outputRunning)
            silentRunning_ += elapsedSeconds;
        if (silentRunning_ < kSilentSeconds)
            return false;
        settled_ = true;
        return true;
    }

    void reset() { *this = {}; }

private:
    double silentRunning_ = 0.0;
    bool settled_ = false;   // warned, or heard a signal
};

} // namespace fixomic::mac
