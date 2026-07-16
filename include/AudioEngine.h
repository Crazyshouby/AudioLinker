#pragma once

#include <array>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include "RingBuffer.h"
#include "Resampler.h"

// 5-band graphic EQ: band 0 is a low shelf, the last band a high shelf, the
// ones in between peaking (bell) filters -- see Eq.h/BiquadFilter. Shared
// between AudioEngine and the GUI so band count/frequencies can't drift
// apart between the two.
constexpr int kEqBandCount = 5;
constexpr double kEqBandFreqs[kEqBandCount] = {60.0, 250.0, 1000.0, 4000.0, 12000.0};

// Separate, finer-grained set of analysis-only bands for the EQ panel's
// spectrum visualization (standard ISO 10-band graphic EQ/analyzer center
// frequencies). Purely metering -- unrelated to the 5-band shaping EQ above,
// never applied to the actual signal, just measured from it.
constexpr int kSpectrumBandCount = 10;
constexpr double kSpectrumBandFreqs[kSpectrumBandCount] = {
    31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0
};

// Baseline ring-buffer cushion (ms) kept on top of every output's own latency
// setting -- see CushionSamples() in AudioEngine.cpp. User-adjustable
// tradeoff: lower means less total latency, higher means more headroom
// against crackling when the capture/render threads occasionally don't get
// scheduled promptly (e.g. under CPU load).
constexpr int kDefaultBaseCushionMs = 300;
constexpr int kMinBaseCushionMs = 50;
constexpr int kMaxBaseCushionMs = 800;

struct OutputLinkConfig {
    std::wstring deviceId;
    int latencyMs = 0;     // extra delay for this output relative to the others
    int channelMode = 0;   // 0 = stereo, 1 = left only, 2 = right only
    int volumePercent = 100;
    std::array<int, kEqBandCount> eqDb{}; // per-band gain, -12..+12 dB each
    bool muted = false;
    // 0 = fed by the system-wide capture (sourceId); otherwise the PID of
    // the app captured for this output via process loopback. One capture
    // thread is spawned per distinct source across the group.
    unsigned long sourcePid = 0;
};

struct OutputStatus {
    std::wstring deviceId;
    bool active = false;
    uint64_t underruns = 0;
    uint64_t driftCorrections = 0;
    double peak = 0.0; // peak amplitude 0..1 since the previous getStatus()
    // Device-side lag in ms (frames submitted vs. frames the endpoint reports
    // as played, via IAudioClock): WASAPI buffer + driver/Bluetooth buffering.
    // 0 until the first measurement (~1s after the output starts).
    int deviceLagMs = 0;
    // Peak amplitude 0..1 since the previous getStatus(), same as peak but
    // measured after a bandpass filter centered on each kSpectrumBandFreqs
    // entry -- a real 10-band spectrum of what's actually being sent to this
    // output, for the EQ panel's spectrum animation.
    std::array<double, kSpectrumBandCount> spectrumBands{};
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // sourceIsLoopback: true if sourceId names a render (output) device and
    // we should capture "what you hear" from it instead of a physical input.
    // masterVolumeDeviceId: the render endpoint whose volume/mute the Windows
    // volume control (taskbar slider, volume keys) actually manipulates while
    // the group is active -- i.e. the virtual cable's render side. This is
    // followed live and applied as an extra gain on top of each output's own
    // volume, since virtual-cable drivers generally don't apply that
    // attenuation to the audio themselves.
    //
    // start() launches device negotiation on a background thread and returns
    // immediately -- errorOut is only used for the few checks that fail
    // synchronously (already running, no outputs selected). Poll isStarting()
    // until it goes back to false, then call takeStartError() to find out
    // whether that attempt actually succeeded.
    //
    // Sources are per output (OutputLinkConfig::sourcePid): outputs with
    // sourcePid 0 are fed from sourceId (the virtual cable), the others from
    // a process-loopback capture of their PID. Avoiding double audio for
    // captured apps is the caller's job (per-app endpoint routing) -- session
    // muting is NOT an option, the loopback tap sits after session volume.
    // sourceId/masterVolumeDeviceId may be empty when no output uses the
    // system source.
    bool start(const std::wstring& sourceId, bool sourceIsLoopback,
               const std::wstring& masterVolumeDeviceId,
               const std::vector<OutputLinkConfig>& outputConfigs,
               std::wstring& errorOut);
    void stop();
    bool isRunning() const { return running_.load() && !starting_.load(); }
    bool isStarting() const { return starting_.load(); }
    // Valid to call once isStarting() has gone from true back to false.
    // Returns true (and fills errorOut) if that start attempt failed.
    bool takeStartError(std::wstring& errorOut);

    void setLatencyMs(const std::wstring& outputId, int ms);
    void setChannelMode(const std::wstring& outputId, int mode);
    void setVolumePercent(const std::wstring& outputId, int percent);
    void setEqBand(const std::wstring& outputId, int band, int db);
    void setMuted(const std::wstring& outputId, bool muted);
    // Snaps the output's ring buffer to its current latency target within
    // ~1s (drop or pad) instead of the inaudible-but-slow ±0.15% correction.
    // Called after deliberate latency changes (slider commit, auto-align).
    void resyncOutput(const std::wstring& outputId);
    // Global, applies to every output's cushion on top of its own latency
    // (see kDefaultBaseCushionMs). Just updates the target -- call
    // resyncAllOutputs() afterwards to snap to it immediately.
    void setBaseCushionMs(int ms);
    // Same idea as resyncOutput() but for every output at once -- used after
    // a base-cushion change (slider commit) instead of one output at a time.
    void resyncAllOutputs();
    std::vector<OutputStatus> getStatus();

    // Plays two short identifying beeps directly to this one physical
    // device (a brief standalone WASAPI shared-mode stream, independent of
    // start()/the fan-out group), so a user can tell which list entry is
    // which. Fire-and-forget: spawns its own thread and returns immediately.
    void identifyOutput(const std::wstring& deviceId);

private:
    struct OutputState {
        std::wstring deviceId;
        std::unique_ptr<RingBuffer> ring;
        std::atomic<int> latencyMs{0};
        std::thread thread;
        std::atomic<bool> formatReady{false};
        std::atomic<bool> active{false};
        std::atomic<bool> failed{false};
        std::atomic<uint64_t> underruns{0};
        std::atomic<uint64_t> driftCorrections{0};
        std::atomic<int> channels{2};
        std::atomic<int> sampleRate{48000};
        std::atomic<int> channelMode{0};
        std::atomic<int> volumePercent{100};
        std::atomic<int> eqDb[kEqBandCount] = {};
        std::atomic<bool> muted{false};
        // Set by a render reconnect session after updating channels/
        // sampleRate: tells the capture thread (the resampler's owner) to
        // reconfigure it for the device's possibly-changed mix format.
        std::atomic<bool> formatDirty{false};
        std::atomic<int> peakMilli{0}; // peak * 1000 since last status read
        std::atomic<int> spectrumMilli[kSpectrumBandCount] = {}; // see OutputStatus::spectrumBands
        std::atomic<int> deviceLagMs{0}; // see OutputStatus::deviceLagMs
        std::atomic<bool> forceResync{false};
        std::atomic<double> speedAdjustment{1.0};
        // Frames (as raw sample count) the render thread wants the capture
        // thread to pad the ring with on its next write. Once formatReady is
        // set, the render thread is this ring's sole reader and must never
        // call its write-side methods itself -- doing so raced with the
        // capture thread's own writes and corrupted playback; this flag is
        // how it asks instead. The one exception is the initial pre-roll in
        // runRenderSession(), which writes silence directly before
        // formatReady is published -- at that point the capture thread has
        // not started touching this ring yet, so there is no second producer
        // to race with.
        std::atomic<size_t> silencePadFrames{0};
        StreamResampler resampler; // owned by this output's capture thread once running
        // Which capture thread feeds this ring (0 = system source). Fixed for
        // the whole session: written in start() before any thread spawns.
        unsigned long sourcePid = 0;
    };

    // One instance runs per distinct source: sourcePid 0 captures the device
    // sourceId (with the master-volume mirror), a non-zero pid captures that
    // process tree. Each instance writes only to the rings of the outputs
    // whose sourcePid matches -- every ring keeps a single producer.
    void captureThreadProc(std::wstring sourceId, bool loopback, std::wstring masterVolumeDeviceId,
                           unsigned long sourcePid);
    // Outer loop: runs one device session at a time and, once the group has
    // started successfully, keeps retrying after a device loss (unplugged
    // Bluetooth, disabled endpoint...) until stop() -- the output rejoins
    // the group by itself when its device comes back.
    void renderThreadProc(OutputState* state);
    enum class RenderSessionEnd { InitFailed, Lost, Stopped };
    RenderSessionEnd runRenderSession(OutputState* state, bool firstSession);
    // Waits for every render thread's formatReady, then starts capture --
    // the blocking part of what start() used to do inline on the caller's
    // thread (freezing the UI for up to ~3s). Runs on starterThread_ instead.
    void starterThreadProc(std::wstring sourceId, bool sourceIsLoopback, std::wstring masterVolumeDeviceId);
    // Cleans up after a failed start attempt (join capture/render threads,
    // clear outputs_, record the error) -- everything stop() would do, minus
    // touching starterThread_ itself since this runs ON that thread.
    void failStart(const std::wstring& error);

    std::atomic<bool> running_{false};
    std::atomic<bool> starting_{false};
    std::thread starterThread_;
    std::vector<std::thread> captureThreads_; // one per distinct source

    std::mutex startErrorMutex_;
    std::wstring startError_;
    std::atomic<bool> hasStartError_{false};

    // Mirrors the Windows-controlled volume/mute of masterVolumeDeviceId,
    // kept live by an IAudioEndpointVolumeCallback registered from
    // captureThreadProc. Applied by every render thread alongside its own
    // output's volume.
    std::atomic<float> masterVolumeScalar_{1.0f};
    std::atomic<bool> masterMuted_{false};

    std::mutex outputsMutex_;
    std::vector<std::unique_ptr<OutputState>> outputs_;
};
