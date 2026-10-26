#include "AudioEngine.h"
#include "Resampler.h"
#include "Eq.h"
#include "Log.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <avrt.h>
#include <timeapi.h>
#include <algorithm>
#include <chrono>
#include <climits>

#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

namespace {

bool isFloatFormat(const WAVEFORMATEX* wfx) {
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

// The convert paths below only handle float32 and 16-bit PCM. Shared-mode
// mix formats are float32 in practice, so this should never trip -- but if
// a driver ever hands back something else (24/32-bit int PCM), refusing it
// beats reinterpreting its samples as int16 and playing corrupted noise.
bool isSupportedFormat(const WAVEFORMATEX* wfx) {
    return isFloatFormat(wfx) || wfx->wBitsPerSample == 16;
}

// Linear fade-in applied to the first few ms of audio played right after a
// hard drift-correction drop, so the jump to a different point in the stream
// isn't an audible click.
constexpr int kDeclickFadeMs = 5;

// Process-loopback capture: how long to wait for the app's next packet
// before topping the rings up with silence (see runCaptureSession). Must
// stay well under the smallest cushion (kMinBaseCushionMs) or the render
// side drains the ring before the pad lands.
constexpr DWORD kProcessIdleWaitMs = 40;

// Soft clip above this level: the render thread hands float samples to the
// Windows mixer unclamped, so an EQ boost on a loud passage used to clip
// hard there. Gentle knee, reaches exactly 1.0 only asymptotically.
constexpr float kSoftClipKnee = 0.95f;
inline float SoftClip(float x) {
    float a = x < 0.0f ? -x : x;
    if (a <= kSoftClipKnee) return x;
    float e = a - kSoftClipKnee;
    float y = kSoftClipKnee + e / (1.0f + e * 8.0f);
    if (y > 1.0f) y = 1.0f;
    return x < 0.0f ? -y : y;
}

struct ComScope {
    bool ownsInit = false;
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ownsInit = (hr == S_OK || hr == S_FALSE);
    }
    ~ComScope() {
        if (ownsInit) CoUninitialize();
    }
};

// Without this, Sleep(10) in the capture loop is bound by the default system
// timer granularity (commonly ~15ms on modern Windows when nothing has
// requested finer), so a "10ms" poll can silently take longer -- eating into
// the ring buffer's cushion and causing underruns. Only the capture thread
// needs this: render threads are event-driven (SetEventHandle) and never
// sleep on a timer.
struct TimerResolutionScope {
    bool active = false;
    TimerResolutionScope() { active = (timeBeginPeriod(1) == TIMERR_NOERROR); }
    ~TimerResolutionScope() { if (active) timeEndPeriod(1); }
};

// Registers the calling thread with MMCSS's "Pro Audio" category, which both
// elevates its priority and coordinates scheduling across every thread
// registered with it. Falls back to a raw priority bump only if that
// registration fails (older systems) -- forcing THREAD_PRIORITY_TIME_CRITICAL
// on top of a successful registration made things worse, not better, once
// there's a capture thread and multiple render threads all fighting at the
// OS's most aggressive priority simultaneously instead of letting MMCSS
// arbitrate between them.
HANDLE EnableProAudioMmcss() {
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!mmcssHandle) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    return mmcssHandle;
}

ComPtr<IMMDeviceEnumerator> CreateDeviceEnumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator),
                     reinterpret_cast<void**>(enumerator.GetAddressOf()));
    return enumerator;
}

// Ring-buffer cushion (in samples, i.e. frames * channels) kept on top of an
// output's configured extra latency. The base cushion absorbs scheduling
// jitter between the capture thread and each independent render thread (not
// clock drift, which the resampler corrects); it applies uniformly to every
// output, so it never affects sync between them -- only the group's
// absolute latency vs. the source (see kDefaultBaseCushionMs). Shared by
// the initial pre-roll, the post-reconnect cushion rebuild, the idle top-up
// and the periodic drift-check target so they can never drift out of sync
// with each other over a future rounding/unit change.
size_t CushionSamples(int baseCushionMs, int latencyMs, int sampleRate, int channels) {
    int64_t ms = static_cast<int64_t>(baseCushionMs) + latencyMs;
    return static_cast<size_t>(ms * sampleRate / 1000) * static_cast<size_t>(channels);
}

// Mirrors the Windows-controlled master volume/mute of one endpoint into a
// pair of atomics. Registered on the virtual cable's render endpoint so the
// system volume control (which that driver doesn't actually apply itself)
// still has an audible effect. Stack-lifetime: not self-deleting on Release,
// owned for exactly as long as it stays registered in captureThreadProc.
class EndpointVolumeCallback : public IAudioEndpointVolumeCallback {
public:
    EndpointVolumeCallback(std::atomic<float>* scalar, std::atomic<bool>* muted)
        : scalar_(scalar), muted_(muted) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override { return ref_.fetch_sub(1) - 1; }

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
        if (!data) return S_OK;
        scalar_->store(data->fMasterVolume);
        muted_->store(data->bMuted != FALSE);
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    std::atomic<float>* scalar_;
    std::atomic<bool>* muted_;
};

// Bridges ActivateAudioInterfaceAsync (used for process loopback) back to a
// plain event wait. IAgileObject spares COM the marshaling of the callback
// (we run in the MTA). Stack lifetime, like the other callback shims here.
class ActivateCompletionHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    ActivateCompletionHandler() { event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivateCompletionHandler() { if (event_) CloseHandle(event_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override { return ref_.fetch_sub(1) - 1; }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(event_);
        return S_OK;
    }

    bool wait(DWORD timeoutMs) const {
        return event_ && WaitForSingleObject(event_, timeoutMs) == WAIT_OBJECT_0;
    }

private:
    HANDLE event_ = nullptr;
    std::atomic<ULONG> ref_{1};
};

// NOTE on avoiding double audio in process-loopback mode: muting the app's
// mixer sessions does NOT work -- the loopback tap sits *after* the session
// volume, so muting the app also silences the capture (same limitation as
// OBS's application capture). The GUI instead re-routes the captured app's
// output to the virtual cable (per-app endpoint routing) for the session's
// duration; this engine only captures.

} // namespace

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
    stop();
    stopMicMetering();
    joinIdentifyJobs(false);
}

bool AudioEngine::start(const std::wstring& sourceId, bool sourceIsLoopback,
                         const std::wstring& masterVolumeDeviceId,
                         const std::vector<OutputLinkConfig>& outputConfigs,
                         std::wstring& errorOut) {
    if (running_.load() || starting_.load()) {
        errorOut = L"Le moteur est déjà démarré.";
        return false;
    }
    if (outputConfigs.empty()) {
        errorOut = L"Aucune sortie sélectionnée.";
        return false;
    }
    // A previous attempt's starterThread_ is guaranteed finished by now
    // (starting_ is false), but still needs joining before we can reuse it.
    if (starterThread_.joinable()) starterThread_.join();

    {
        std::lock_guard<std::mutex> lock(outputsMutex_);
        outputs_.clear();
        for (const auto& cfg : outputConfigs) {
            auto state = std::make_unique<OutputState>();
            state->deviceId = cfg.deviceId;
            state->latencyMs = cfg.latencyMs;
            state->channelMode = cfg.channelMode;
            state->volumePercent = cfg.volumePercent;
            for (int b = 0; b < kEqBandCount; ++b) state->eqDb[b] = cfg.eqDb[b];
            state->muted = cfg.muted;
            state->sourcePid = cfg.sourcePid;
            // ~4M float samples: several seconds of headroom at 48kHz stereo,
            // generous enough for large manual latency offsets too.
            state->ring = std::make_unique<RingBuffer>(1 << 22);
            outputs_.push_back(std::move(state));
        }
    }

    running_ = true;
    starting_ = true;
    hasStartError_ = false;
    masterVolumeScalar_ = 1.0f;
    masterMuted_ = false;

    for (auto& state : outputs_) {
        state->thread = std::thread(&AudioEngine::renderThreadProc, this, state.get());
    }

    // Device negotiation (waiting for every render thread's formatReady, then
    // starting capture) used to happen right here, blocking whichever thread
    // called start() -- the GUI thread -- for up to ~3.2s. Moved to its own
    // thread so the caller gets control back immediately; poll isStarting().
    starterThread_ = std::thread(&AudioEngine::starterThreadProc, this, sourceId, sourceIsLoopback,
                                  masterVolumeDeviceId);
    return true;
}

void AudioEngine::starterThreadProc(std::wstring sourceId, bool sourceIsLoopback,
                                     std::wstring masterVolumeDeviceId) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (auto& state : outputs_) {
        while (!state->formatReady.load() && running_.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    if (!running_.load()) {
        // stop() was called while we were still waiting; it owns cleanup.
        starting_ = false;
        return;
    }

    bool anyFailed = false;
    for (auto& state : outputs_) {
        if (state->failed.load() || !state->formatReady.load()) anyFailed = true;
    }
    if (anyFailed) {
        failStart(L"Impossible d'initialiser une ou plusieurs sorties audio.");
        return;
    }

    // One capture thread per distinct source: the system capture (if any
    // output uses it) plus one process-loopback capture per distinct app.
    bool needSystem = false;
    std::vector<unsigned long> appPids;
    for (auto& state : outputs_) {
        if (state->sourcePid == 0) {
            needSystem = true;
        } else if (std::find(appPids.begin(), appPids.end(), state->sourcePid) == appPids.end()) {
            appPids.push_back(state->sourcePid);
        }
    }
    {
        std::lock_guard<std::mutex> lock(startMutex_);
        captureInitPending_ = (needSystem ? 1 : 0) + static_cast<int>(appPids.size());
        captureInitFailed_ = false;
    }
    if (needSystem) {
        captureThreads_.emplace_back(&AudioEngine::captureThreadProc, this, sourceId,
                                     sourceIsLoopback, masterVolumeDeviceId, 0ul);
    }
    for (unsigned long pid : appPids) {
        captureThreads_.emplace_back(&AudioEngine::captureThreadProc, this, std::wstring(),
                                     false, std::wstring(), pid);
    }

    // Wait for every capture thread's first stream init to report in (see
    // reportCaptureInit) rather than peeking after a fixed delay: a
    // process-loopback activation alone can take up to 3s, and a failure
    // landing after such a window used to leave a "running" group whose
    // source was silently dead for good.
    bool captureFailed;
    {
        std::unique_lock<std::mutex> lock(startMutex_);
        startCv_.wait_for(lock, std::chrono::seconds(5), [&] {
            return captureInitPending_ == 0 || !running_.load();
        });
        captureFailed = captureInitFailed_ || captureInitPending_ > 0;
    }
    if (!running_.load()) {
        starting_ = false; // stop() was called meanwhile; it owns cleanup
        return;
    }
    if (captureFailed) {
        failStart(L"Impossible d'initialiser le périphérique source.");
        return;
    }

    starting_ = false; // success: isRunning() now reports true
}

void AudioEngine::reportCaptureInit(bool ok) {
    {
        std::lock_guard<std::mutex> lock(startMutex_);
        if (!ok) captureInitFailed_ = true;
        if (captureInitPending_ > 0) --captureInitPending_;
    }
    startCv_.notify_all();
}

void AudioEngine::joinAndClearOutputs() {
    std::vector<std::unique_ptr<OutputState>> doomed;
    {
        std::lock_guard<std::mutex> lock(outputsMutex_);
        doomed.swap(outputs_);
    }
    for (auto& state : doomed) {
        if (state->thread.joinable()) state->thread.join();
    }
}

void AudioEngine::failStart(const std::wstring& error) {
    running_ = false;
    for (auto& t : captureThreads_) {
        if (t.joinable()) t.join();
    }
    captureThreads_.clear();
    joinAndClearOutputs();
    {
        std::lock_guard<std::mutex> lock(startErrorMutex_);
        startError_ = error;
    }
    hasStartError_ = true;
    starting_ = false;
}

bool AudioEngine::takeStartError(std::wstring& errorOut) {
    if (!hasStartError_.exchange(false)) return false;
    std::lock_guard<std::mutex> lock(startErrorMutex_);
    errorOut = startError_;
    return true;
}

void AudioEngine::stop() {
    running_ = false;
    // Wake a starter still waiting on the capture threads' init reports
    // (taking the mutex first so the wake-up can't slip between its
    // predicate check and its wait).
    {
        std::lock_guard<std::mutex> lock(startMutex_);
    }
    startCv_.notify_all();
    // Join the starter first: it may still be mid-negotiation and touches
    // captureThreads_/outputs_ itself on failure -- must not race with the
    // cleanup below.
    if (starterThread_.joinable()) starterThread_.join();
    for (auto& t : captureThreads_) {
        if (t.joinable()) t.join();
    }
    captureThreads_.clear();
    joinAndClearOutputs();
    starting_ = false;
}

void AudioEngine::setLatencyMs(const std::wstring& outputId, int ms) {
    std::lock_guard<std::mutex> lock(outputsMutex_);
    for (auto& state : outputs_) {
        if (state->deviceId == outputId) {
            state->latencyMs = ms;
        }
    }
}

void AudioEngine::setChannelMode(const std::wstring& outputId, int mode) {
    std::lock_guard<std::mutex> lock(outputsMutex_);
    for (auto& state : outputs_) {
        if (state->deviceId == outputId) {
            state->channelMode = mode;
        }
    }
}

void AudioEngine::setVolumePercent(const std::wstring& outputId, int percent) {
    std::lock_guard<std::mutex> lock(outputsMutex_);
    for (auto& state : outputs_) {
        if (state->deviceId == outputId) {
            state->volumePercent = percent;
        }
    }
}

void AudioEngine::setEqBand(const std::wstring& outputId, int band, int db) {
    if (band < 0 || band >= kEqBandCount) return;
    std::lock_guard<std::mutex> lock(outputsMutex_);
    for (auto& state : outputs_) {
        if (state->deviceId == outputId) state->eqDb[band] = db;
    }
}

void AudioEngine::setMuted(const std::wstring& outputId, bool muted) {
    std::lock_guard<std::mutex> lock(outputsMutex_);
    for (auto& state : outputs_) {
        if (state->deviceId == outputId) {
            state->muted = muted;
        }
    }
}

void AudioEngine::resyncOutput(const std::wstring& outputId) {
    std::lock_guard<std::mutex> lock(outputsMutex_);
    for (auto& state : outputs_) {
        if (state->deviceId == outputId) {
            state->forceResync = true;
        }
    }
}

void AudioEngine::setBaseCushionMs(int ms) {
    baseCushionMs_.store(std::clamp(ms, kMinBaseCushionMs, kMaxBaseCushionMs));
}

void AudioEngine::setDeviceFillMs(int ms) {
    deviceFillMs_.store(std::clamp(ms, kMinDeviceFillMs, kMaxDeviceFillMs));
}

void AudioEngine::setLowLatency(bool on) {
    lowLatency_.store(on);
}

void AudioEngine::setSpectrumTarget(const std::wstring& outputId) {
    std::lock_guard<std::mutex> lock(outputsMutex_);
    for (auto& state : outputs_) {
        state->spectrumEnabled = (state->deviceId == outputId);
    }
}

void AudioEngine::resyncAllOutputs() {
    std::lock_guard<std::mutex> lock(outputsMutex_);
    for (auto& state : outputs_) {
        state->forceResync = true;
    }
}

std::vector<OutputStatus> AudioEngine::getStatus() {
    std::vector<OutputStatus> result;
    std::lock_guard<std::mutex> lock(outputsMutex_);
    for (auto& state : outputs_) {
        OutputStatus s;
        s.deviceId = state->deviceId;
        s.active = state->active.load();
        s.sourceLost = state->sourceLost.load();
        s.sourcePid = state->sourcePid;
        s.underruns = state->underruns.load();
        s.driftCorrections = state->driftCorrections.load();
        s.deviceLagMs = state->deviceLagMs.load();
        result.push_back(std::move(s));
    }
    return result;
}

std::vector<OutputPeaks> AudioEngine::takePeaks() {
    std::vector<OutputPeaks> result;
    std::lock_guard<std::mutex> lock(outputsMutex_);
    for (auto& state : outputs_) {
        OutputPeaks p;
        p.deviceId = state->deviceId;
        p.peak = state->peakMilli.exchange(0) / 1000.0;
        for (int b = 0; b < kSpectrumBandCount; ++b) {
            p.spectrumBands[b] = state->spectrumMilli[b].exchange(0) / 1000.0;
        }
        result.push_back(std::move(p));
    }
    return result;
}

void AudioEngine::joinIdentifyJobs(bool finishedOnly) {
    std::lock_guard<std::mutex> lock(identifyMutex_);
    for (auto it = identifyJobs_.begin(); it != identifyJobs_.end();) {
        if (finishedOnly && !it->done->load()) { ++it; continue; }
        if (it->thread.joinable()) it->thread.join();
        it = identifyJobs_.erase(it);
    }
}

void AudioEngine::identifyOutput(const std::wstring& deviceId) {
    joinIdentifyJobs(true); // reap the ones already done
    auto done = std::make_shared<std::atomic<bool>>(false);
    IdentifyJob job;
    job.done = done;
    job.thread = std::thread([deviceId, done]() {
        // Flagged on every exit path (including the early returns below) so
        // the next identifyOutput() can reap this thread.
        struct Finish {
            std::shared_ptr<std::atomic<bool>> flag;
            ~Finish() { flag->store(true); }
        } finish{ done };
        ComScope com;
        if (!com.ownsInit) return;

        auto enumerator = CreateDeviceEnumerator();
        if (!enumerator) return;
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDevice(deviceId.c_str(), device.GetAddressOf()))) return;

        ComPtr<IAudioClient> audioClient;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(audioClient.GetAddressOf())))) {
            return;
        }

        WAVEFORMATEX* mixFormat = nullptr;
        if (FAILED(audioClient->GetMixFormat(&mixFormat)) || !mixFormat) return;
        if (!isSupportedFormat(mixFormat)) {
            CoTaskMemFree(mixFormat);
            return;
        }
        int channels = mixFormat->nChannels;
        int sampleRate = mixFormat->nSamplesPerSec;
        bool dstIsFloat = isFloatFormat(mixFormat);

        constexpr REFERENCE_TIME kBufferDuration = 3'000'000; // 300ms, in 100ns units
        HRESULT hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, mixFormat, nullptr);
        CoTaskMemFree(mixFormat);
        if (FAILED(hr)) return;

        ComPtr<IAudioRenderClient> renderClient;
        if (FAILED(audioClient->GetService(__uuidof(IAudioRenderClient),
                                            reinterpret_cast<void**>(renderClient.GetAddressOf())))) {
            return;
        }

        UINT32 bufferFrameCount = 0;
        audioClient->GetBufferSize(&bufferFrameCount);

        // Two short beeps an octave apart (with a brief silent gap) so this
        // reads as a deliberate "which one are you" ping, not a generic
        // click -- easy to tell apart from normal playback.
        struct Beep { double freq; int ms; };
        const Beep beeps[] = { {880.0, 180}, {0.0, 90}, {1320.0, 180} };

        if (FAILED(audioClient->Start())) return;
        double phase = 0.0;
        constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
        for (const auto& beep : beeps) {
            int framesTotal = sampleRate * beep.ms / 1000;
            int framesWritten = 0;
            while (framesWritten < framesTotal) {
                UINT32 padding = 0;
                audioClient->GetCurrentPadding(&padding);
                UINT32 framesAvailable = std::min<UINT32>(bufferFrameCount - padding,
                                                            static_cast<UINT32>(framesTotal - framesWritten));
                if (framesAvailable == 0) { Sleep(5); continue; }
                BYTE* buf = nullptr;
                if (FAILED(renderClient->GetBuffer(framesAvailable, &buf))) break;
                for (UINT32 f = 0; f < framesAvailable; ++f) {
                    float sample = 0.0f;
                    if (beep.freq > 0.0) {
                        sample = 0.25f * static_cast<float>(std::sin(phase));
                        phase += kTwoPi * beep.freq / sampleRate;
                        if (phase > kTwoPi) phase -= kTwoPi;
                    }
                    for (int c = 0; c < channels; ++c) {
                        if (dstIsFloat) {
                            reinterpret_cast<float*>(buf)[f * channels + c] = sample;
                        } else {
                            reinterpret_cast<int16_t*>(buf)[f * channels + c] =
                                static_cast<int16_t>(sample * 32767.0f);
                        }
                    }
                }
                renderClient->ReleaseBuffer(framesAvailable, 0);
                framesWritten += framesAvailable;
            }
        }
        Sleep(150); // let the tail actually finish playing before Stop() cuts it off
        audioClient->Stop();
    });
    std::lock_guard<std::mutex> lock(identifyMutex_);
    identifyJobs_.push_back(std::move(job));
}

void AudioEngine::startMicMetering(const std::vector<std::wstring>& deviceIds) {
    stopMicMetering();
    auto bank = std::make_shared<std::vector<std::unique_ptr<MicMeterEntry>>>();
    for (const auto& id : deviceIds) {
        auto e = std::make_unique<MicMeterEntry>();
        e->deviceId = id;
        bank->push_back(std::move(e));
    }
    {
        std::lock_guard<std::mutex> lock(micMeterMutex_);
        micMeterBank_ = bank;
    }
    micMeterRun_ = true;
    micMeterThread_ = std::thread([this, bank]() {
        ComScope com;
        if (!com.ownsInit) return;
        auto enumerator = CreateDeviceEnumerator();
        if (!enumerator) return;

        struct Stream {
            MicMeterEntry* entry = nullptr;
            ComPtr<IAudioClient> client;
            ComPtr<IAudioCaptureClient> capture;
            int channels = 2;
            bool isFloat = true;
        };
        std::vector<Stream> streams;
        for (auto& e : *bank) {
            Stream s;
            s.entry = e.get();
            ComPtr<IMMDevice> device;
            if (FAILED(enumerator->GetDevice(e->deviceId.c_str(), device.GetAddressOf()))) continue;
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**>(s.client.GetAddressOf())))) {
                continue;
            }
            WAVEFORMATEX* mix = nullptr;
            if (FAILED(s.client->GetMixFormat(&mix)) || !mix) continue;
            if (!isSupportedFormat(mix)) { CoTaskMemFree(mix); continue; }
            s.channels = mix->nChannels;
            s.isFloat = isFloatFormat(mix);
            HRESULT hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                              2'000'000 /*200ms*/, 0, mix, nullptr);
            CoTaskMemFree(mix);
            if (FAILED(hr)) continue;
            if (FAILED(s.client->GetService(__uuidof(IAudioCaptureClient),
                                             reinterpret_cast<void**>(s.capture.GetAddressOf())))) {
                continue;
            }
            if (FAILED(s.client->Start())) continue;
            streams.push_back(std::move(s));
        }

        // 50ms polling is plenty for a VU refreshed by the 250ms status
        // timer, and the 200ms stream buffer never overflows between polls.
        while (micMeterRun_.load()) {
            for (auto& s : streams) {
                float peak = 0.0f;
                UINT32 packet = 0;
                while (SUCCEEDED(s.capture->GetNextPacketSize(&packet)) && packet > 0) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    if (FAILED(s.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                        size_t n = static_cast<size_t>(frames) * s.channels;
                        if (s.isFloat) {
                            const float* f = reinterpret_cast<const float*>(data);
                            for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::abs(f[i]));
                        } else {
                            const int16_t* p = reinterpret_cast<const int16_t*>(data);
                            for (size_t i = 0; i < n; ++i) {
                                peak = std::max(peak, std::abs(p[i]) / 32768.0f);
                            }
                        }
                    }
                    s.capture->ReleaseBuffer(frames);
                }
                int milli = static_cast<int>(peak * 1000.0f);
                int prev = s.entry->peakMilli.load();
                while (milli > prev && !s.entry->peakMilli.compare_exchange_weak(prev, milli)) {}
            }
            Sleep(50);
        }
        for (auto& s : streams) s.client->Stop();
    });
}

void AudioEngine::stopMicMetering() {
    micMeterRun_ = false;
    if (micMeterThread_.joinable()) micMeterThread_.join();
    std::lock_guard<std::mutex> lock(micMeterMutex_);
    micMeterBank_.reset();
}

std::vector<std::pair<std::wstring, double>> AudioEngine::getMicPeaks() {
    std::shared_ptr<std::vector<std::unique_ptr<MicMeterEntry>>> bank;
    {
        std::lock_guard<std::mutex> lock(micMeterMutex_);
        bank = micMeterBank_;
    }
    std::vector<std::pair<std::wstring, double>> out;
    if (!bank) return out;
    out.reserve(bank->size());
    for (auto& e : *bank) {
        out.emplace_back(e->deviceId, e->peakMilli.exchange(0) / 1000.0);
    }
    return out;
}

namespace {

// Whether the process-loopback target still exists -- distinguishes "the
// captured app closed" (permanent: PIDs never come back) from a transient
// WASAPI failure worth retrying.
bool ProcessStillAlive(unsigned long pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED; // exists, just protected
    DWORD wait = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return wait == WAIT_TIMEOUT;
}

} // namespace

void AudioEngine::markSourceLost(unsigned long sourcePid, bool lost) {
    std::lock_guard<std::mutex> lock(outputsMutex_);
    for (auto& out : outputs_) {
        if (out->sourcePid == sourcePid) out->sourceLost = lost;
    }
}

void AudioEngine::captureThreadProc(std::wstring sourceId, bool loopback, std::wstring masterVolumeDeviceId,
                                     unsigned long sourcePid) {
    ComScope com;
    TimerResolutionScope timerRes;
    HANDLE mmcssHandle = EnableProAudioMmcss();
    const bool processMode = (sourcePid != 0);

    bool firstSession = true;
    while (running_.load()) {
        CaptureSessionEnd end = runCaptureSession(sourceId, loopback, masterVolumeDeviceId,
                                                  sourcePid, firstSession);
        if (end == CaptureSessionEnd::Stopped || !running_.load()) break;
        if (firstSession && end == CaptureSessionEnd::InitFailed) {
            // The first session failing to init fails the whole group start:
            // runCaptureSession already reported it to the starter, which is
            // about to tear everything down -- nothing to retry here.
            markSourceLost(sourcePid, true);
            break;
        }
        firstSession = false;

        // Mid-session loss: only THIS source's outputs go quiet (silence +
        // « source perdue » in the UI); the rest of the group keeps playing.
        markSourceLost(sourcePid, true);
        if (processMode && !ProcessStillAlive(sourcePid)) {
            LOG_WARN(L"L'application capturée (PID " + std::to_wstring(sourcePid) +
                     L") s'est terminée : ses sorties passent en silence, le reste du groupe continue.");
            break; // a PID never comes back -- stays marked until stop()
        }
        LOG_WARN(processMode
            ? L"Capture de l'application (PID " + std::to_wstring(sourcePid) +
                  L") perdue — nouvelle tentative dans 2 s."
            : L"Capture système perdue — nouvelle tentative dans 2 s.");
        for (int i = 0; i < 20 && running_.load(); ++i) Sleep(100);
    }

    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
}

AudioEngine::CaptureSessionEnd AudioEngine::runCaptureSession(
        const std::wstring& sourceId, bool loopback,
        const std::wstring& masterVolumeDeviceId,
        unsigned long sourcePid, bool firstSession) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> audioClient;
    ComPtr<IAudioCaptureClient> captureClient;
    WAVEFORMATEX* mixFormat = nullptr;
    HANDLE captureEvent = nullptr;
    const bool processMode = (sourcePid != 0);
    bool ok = false;

    // Process loopback has no GetMixFormat -- the caller dictates the format.
    WAVEFORMATEX processFmt = {};
    processFmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    processFmt.nChannels = 2;
    processFmt.nSamplesPerSec = 48000;
    processFmt.wBitsPerSample = 32;
    processFmt.nBlockAlign = static_cast<WORD>(processFmt.nChannels * processFmt.wBitsPerSample / 8);
    processFmt.nAvgBytesPerSec = processFmt.nSamplesPerSec * processFmt.nBlockAlign;

    do {
        enumerator = CreateDeviceEnumerator();
        if (!enumerator) break;

        if (processMode) {
            // Capture one process tree's audio via the virtual
            // process-loopback device (Windows 10 2004+).
            AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
            activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
            activationParams.ProcessLoopbackParams.TargetProcessId = static_cast<DWORD>(sourcePid);
            activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
                PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
            PROPVARIANT activateParams = {};
            activateParams.vt = VT_BLOB;
            activateParams.blob.cbSize = sizeof(activationParams);
            activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);

            ActivateCompletionHandler completion;
            ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
            if (FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                                    __uuidof(IAudioClient), &activateParams,
                                                    &completion, asyncOp.GetAddressOf()))) break;
            if (!completion.wait(3000)) break;
            HRESULT hrActivate = E_FAIL;
            ComPtr<IUnknown> activated;
            if (FAILED(asyncOp->GetActivateResult(&hrActivate, activated.GetAddressOf())) ||
                FAILED(hrActivate) || !activated) break;
            if (FAILED(activated.As(&audioClient))) break;

            // Event-driven is mandatory here: without a render stream on the
            // virtual device there is nothing to pace a polling loop.
            captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!captureEvent) break;
            // 20ms buffer: the value Microsoft's ApplicationLoopback sample
            // uses -- the virtual process-loopback device is picky, stay on
            // the validated path. The ring cushion absorbs jitter anyway.
            if (FAILED(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                200000, 0, &processFmt, nullptr))) break;
            if (FAILED(audioClient->SetEventHandle(captureEvent))) break;
        } else {
            if (FAILED(enumerator->GetDevice(sourceId.c_str(), device.GetAddressOf()))) break;
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**>(audioClient.GetAddressOf())))) break;
            if (FAILED(audioClient->GetMixFormat(&mixFormat))) break;
            if (!isSupportedFormat(mixFormat)) break;

            DWORD streamFlags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
            REFERENCE_TIME bufferDuration = 1500000; // 150ms: margin for the polling loop under CPU load
            if (FAILED(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                                bufferDuration, 0, mixFormat, nullptr))) break;
        }
        if (FAILED(audioClient->GetService(__uuidof(IAudioCaptureClient),
                                            reinterpret_cast<void**>(captureClient.GetAddressOf())))) break;
        if (FAILED(audioClient->Start())) break;
        ok = true;
    } while (false);

    if (!ok) {
        LOG_ERROR(processMode
            ? L"Capture par application (PID " + std::to_wstring(sourcePid) + L") impossible à démarrer."
            : L"Capture système (câble virtuel) impossible à démarrer.");
        if (mixFormat) CoTaskMemFree(mixFormat);
        if (captureEvent) CloseHandle(captureEvent);
        if (firstSession) reportCaptureInit(false);
        return CaptureSessionEnd::InitFailed;
    }
    if (firstSession) reportCaptureInit(true);

    // Follow the Windows-controlled volume/mute of the virtual cable's render
    // endpoint so the system volume control still does something -- purely
    // best-effort, playback proceeds normally if any of this fails.
    ComPtr<IMMDevice> volumeDevice;
    ComPtr<IAudioEndpointVolume> endpointVolume;
    EndpointVolumeCallback volCallback(&masterVolumeScalar_, &masterMuted_);
    bool volCallbackRegistered = false;
    if (enumerator && SUCCEEDED(enumerator->GetDevice(masterVolumeDeviceId.c_str(), volumeDevice.GetAddressOf())) &&
        SUCCEEDED(volumeDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                          reinterpret_cast<void**>(endpointVolume.GetAddressOf())))) {
        float vol = 1.0f;
        BOOL muted = FALSE;
        if (SUCCEEDED(endpointVolume->GetMasterVolumeLevelScalar(&vol))) masterVolumeScalar_.store(vol);
        if (SUCCEEDED(endpointVolume->GetMute(&muted))) masterMuted_.store(muted != FALSE);
        volCallbackRegistered = SUCCEEDED(endpointVolume->RegisterControlChangeNotify(&volCallback));
    }

    bool srcIsFloat;
    int srcChannels, srcRate;
    if (processMode) {
        srcIsFloat = true;
        srcChannels = processFmt.nChannels;
        srcRate = static_cast<int>(processFmt.nSamplesPerSec);
    } else {
        srcIsFloat = isFloatFormat(mixFormat);
        srcChannels = mixFormat->nChannels;
        srcRate = mixFormat->nSamplesPerSec;
        CoTaskMemFree(mixFormat);
        mixFormat = nullptr;
    }


    // Output formats are known here: start() waits for every render thread's
    // formatReady before spawning this thread. Only this source's outputs
    // are touched -- their resamplers belong to this thread and no other.
    {
        std::lock_guard<std::mutex> lock(outputsMutex_);
        for (auto& out : outputs_) {
            if (out->sourcePid != sourcePid) continue;
            out->resampler.configure(srcChannels, srcRate,
                                     out->channels.load(), out->sampleRate.load());
            if (!firstSession) {
                // Recovery from a lost session: pad requests accumulated
                // while there was no writer are stale, and the ring must
                // snap back to its cushion target at the next drift tick.
                out->silencePadFrames.store(0);
                out->forceResync = true;
                out->sourceLost = false;
            }
        }
    }

    std::vector<float> floatBuf;
    std::vector<float> converted;

    // Pushes one chunk of source audio through every output fed by this
    // source: resample/remap into the output's format, write it to the ring
    // (whole frames only), then service any pad request from the render
    // thread -- this capture thread is each ring's sole writer.
    // outputs_ is stable here: it is only modified in start()/stop()/
    // failStart(), with every capture thread joined first. Individual
    // settings are std::atomic, making lock-free iteration safe.
    auto feedOutputs = [&](const float* data, UINT32 frames) {
        for (auto& out : outputs_) {
            if (out->sourcePid != sourcePid) continue; // fed by another capture thread
            // A render reconnect session may have landed on a different
            // mix format; this thread owns the resampler, so the
            // reconfiguration happens here.
            if (out->formatDirty.exchange(false)) {
                out->resampler.configure(srcChannels, srcRate,
                                         out->channels.load(), out->sampleRate.load());
            }
            const size_t outCh = static_cast<size_t>(out->channels.load());
            out->resampler.setSpeedAdjustment(out->speedAdjustment.load());
            out->resampler.process(data, frames, out->channelMode.load(), converted);
            // A short write means the ring was too full to take everything
            // (render thread stalled for seconds: frozen Bluetooth link,
            // device gone) -- force a resync instead of silently discarding
            // the tail every packet. Counted once per episode, and only for
            // a live output: a disconnected one would otherwise rack up a
            // hundred "corrections" per second until it comes back.
            size_t written = out->ring->writeAligned(converted.data(), converted.size(), outCh);
            if (written < converted.size()) {
                if (!out->ringOverflow.exchange(true) && out->active.load()) {
                    out->driftCorrections.fetch_add(1);
                }
                out->forceResync = true;
            }

            // Service any pad request from the render thread here, since
            // this (capture) thread is the ring's only legitimate writer.
            size_t pad = out->silencePadFrames.exchange(0);
            if (pad > 0) out->ring->writeSilenceAligned(pad, outCh);
        }
    };

    // Process loopback delivers nothing at all while the captured app is
    // silent (a device loopback keeps streaming zeros instead), so the rings
    // would drain and every render callback would count an underrun -- and
    // the drift controller would then pad them back, logging "corrections"
    // for what is just a paused song. Top each ring back up to its cushion
    // target with silence instead: no glitch counted, and the latency
    // alignment is intact when the audio resumes. Reading the ring's fill
    // level from the writer side is safe (SPSC).
    auto topUpRingsWithSilence = [&]() {
        for (auto& out : outputs_) {
            if (out->sourcePid != sourcePid) continue;
            const int channels = out->channels.load();
            size_t target = CushionSamples(baseCushionMs_.load(), out->latencyMs.load(),
                                           out->sampleRate.load(), channels);
            size_t avail = out->ring->availableToRead();
            if (avail < target) {
                out->ring->writeSilenceAligned(target - avail, static_cast<size_t>(channels));
            }
        }
    };

    CaptureSessionEnd end = CaptureSessionEnd::Stopped;
    while (running_.load()) {
        UINT32 packetLength = 0;
        if (FAILED(captureClient->GetNextPacketSize(&packetLength))) {
            end = CaptureSessionEnd::Lost;
            break;
        }

        while (packetLength != 0 && running_.load()) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            if (FAILED(captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr))) {
                end = CaptureSessionEnd::Lost;
                break;
            }

            floatBuf.resize(static_cast<size_t>(numFrames) * srcChannels);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(floatBuf.begin(), floatBuf.end(), 0.0f);
            } else if (srcIsFloat) {
                memcpy(floatBuf.data(), data, floatBuf.size() * sizeof(float));
            } else {
                const int16_t* src = reinterpret_cast<const int16_t*>(data);
                for (size_t i = 0; i < floatBuf.size(); ++i) {
                    floatBuf[i] = src[i] / 32768.0f;
                }
            }

            captureClient->ReleaseBuffer(numFrames);
            feedOutputs(floatBuf.data(), numFrames);

            if (FAILED(captureClient->GetNextPacketSize(&packetLength))) {
                end = CaptureSessionEnd::Lost;
                packetLength = 0;
            }
        }
        if (end == CaptureSessionEnd::Lost) break;

        if (processMode) {
            // Signaled once per period while the app plays; a timeout means
            // the app has gone quiet (see topUpRingsWithSilence).
            if (WaitForSingleObject(captureEvent, kProcessIdleWaitMs) == WAIT_TIMEOUT) {
                topUpRingsWithSilence();
            }
        } else {
            Sleep(10);
        }
    }

    if (volCallbackRegistered) endpointVolume->UnregisterControlChangeNotify(&volCallback);
    audioClient->Stop();
    if (captureEvent) CloseHandle(captureEvent);
    return end;
}

void AudioEngine::renderThreadProc(OutputState* state) {
    ComScope com;
    HANDLE mmcssHandle = EnableProAudioMmcss();

    bool firstSession = true;
    while (running_.load()) {
        RenderSessionEnd end = runRenderSession(state, firstSession);
        if (firstSession) {
            state->formatReady = true; // unblock the starter even on init failure
            if (end == RenderSessionEnd::InitFailed) {
                // First attempt failing means the whole group start fails,
                // exactly as before -- retries are only for devices that
                // die *after* a successful start.
                state->failed = true;
                break;
            }
            firstSession = false;
        }
        if (end == RenderSessionEnd::Stopped || !running_.load()) break;
        // Device lost mid-play (or a reconnect attempt failed): retry every
        // ~2s until it comes back or the engine stops. The capture thread
        // keeps feeding the ring meanwhile; the reconnect path drops that
        // backlog and resumes at the live edge.
        for (int i = 0; i < 20 && running_.load(); ++i) Sleep(100);
    }

    state->active = false;
    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
}

namespace {

// Initializes an event-driven shared-mode render stream. When low-latency
// mode is on and the driver exposes IAudioClient3, requests its minimum
// engine period (often ~2.7ms / 128 frames vs. the classic ~10ms) so
// callbacks come faster and the device-buffer floor is smaller. Returns the
// negotiated buffer size in *bufferFrames. Falls back to the classic 150ms
// Initialize on any failure -- many drivers report no sub-10ms period, and
// InitializeSharedAudioStream is picky about the exact engine format.
// `label` is just for the log line.
bool InitLowLatencyRender(IAudioClient* client, const WAVEFORMATEX* fmt, HANDLE evt,
                          UINT32* bufferFrames, const std::wstring& label, bool lowLatency) {
    if (lowLatency) {
        ComPtr<IAudioClient3> client3;
        if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3),
                                             reinterpret_cast<void**>(client3.GetAddressOf())))) {
            UINT32 def = 0, fundamental = 0, minPeriod = 0, maxPeriod = 0;
            if (SUCCEEDED(client3->GetSharedModeEnginePeriod(fmt, &def, &fundamental,
                                                             &minPeriod, &maxPeriod)) &&
                minPeriod > 0 && minPeriod < def) {
                HRESULT hr = client3->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minPeriod, fmt, nullptr);
                if (SUCCEEDED(hr) && SUCCEEDED(client->SetEventHandle(evt)) &&
                    SUCCEEDED(client->GetBufferSize(bufferFrames))) {
                    LOG_INFO(L"Basse latence OK sur " + label + L" : période " +
                             std::to_wstring(minPeriod) + L" trames (défaut " +
                             std::to_wstring(def) + L"), tampon " + std::to_wstring(*bufferFrames));
                    return true;
                }
                LOG_WARN(L"Basse latence refusée sur " + label + L" (" + log_detail::Hr(hr) +
                         L") — repli sur le mode standard.");
            }
        }
    }
    // Classic path: 150ms allocation, only the fill target kept queued.
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    1500000, 0, fmt, nullptr);
    if (FAILED(hr)) {
        LOG_ERROR(L"Initialize (rendu) a échoué sur " + label + L" : " + log_detail::Hr(hr));
        return false;
    }
    if (FAILED(client->SetEventHandle(evt))) return false;
    return SUCCEEDED(client->GetBufferSize(bufferFrames));
}

} // namespace

AudioEngine::RenderSessionEnd AudioEngine::runRenderSession(OutputState* state, bool firstSession) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> audioClient;
    ComPtr<IAudioRenderClient> renderClient;
    ComPtr<IAudioClock> audioClock;
    UINT64 clockFreq = 0;
    UINT64 framesWritten = 0; // total frames submitted this session
    WAVEFORMATEX* mixFormat = nullptr;
    UINT32 bufferFrameCount = 0;
    // Auto-reset: WASAPI signals it once per device period (~10ms) when
    // buffer space is available, replacing the old Sleep(5) polling loop.
    HANDLE renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = false;

    do {
        if (!renderEvent) break;
        enumerator = CreateDeviceEnumerator();
        if (!enumerator) break;
        HRESULT hrGet = enumerator->GetDevice(state->deviceId.c_str(), device.GetAddressOf());
        if (FAILED(hrGet)) { LOG_ERROR(L"GetDevice (rendu) : " + log_detail::Hr(hrGet)); break; }
        HRESULT hrAct = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**>(audioClient.GetAddressOf()));
        if (FAILED(hrAct)) { LOG_ERROR(L"Activate IAudioClient (rendu) : " + log_detail::Hr(hrAct)); break; }
        if (FAILED(audioClient->GetMixFormat(&mixFormat))) break;
        if (!isSupportedFormat(mixFormat)) {
            LOG_WARN(L"Format non géré sur une sortie (bits/éch = " +
                     std::to_wstring(mixFormat->wBitsPerSample) + L") — sortie ignorée.");
            break;
        }

        state->channels = mixFormat->nChannels;
        state->sampleRate = mixFormat->nSamplesPerSec;

        if (!InitLowLatencyRender(audioClient.Get(), mixFormat, renderEvent, &bufferFrameCount,
                                  state->deviceId, lowLatency_.load())) break;
        if (FAILED(audioClient->GetService(__uuidof(IAudioRenderClient),
                                            reinterpret_cast<void**>(renderClient.GetAddressOf())))) break;
        // Best-effort: feeds the device-lag estimate used by auto-alignment.
        audioClient->GetService(__uuidof(IAudioClock),
                                reinterpret_cast<void**>(audioClock.GetAddressOf()));
        if (audioClock && FAILED(audioClock->GetFrequency(&clockFreq))) audioClock.Reset();

        // Pre-roll silence up to the fill target only (not the whole
        // allocation): anything above the target would just be extra
        // one-time latency that the capped fill loop then drains anyway.
        UINT32 prerollFrames = static_cast<UINT32>(
            static_cast<int64_t>(deviceFillMs_.load()) * mixFormat->nSamplesPerSec / 1000);
        if (prerollFrames > bufferFrameCount) prerollFrames = bufferFrameCount;
        BYTE* data = nullptr;
        if (FAILED(renderClient->GetBuffer(prerollFrames, &data))) break;
        renderClient->ReleaseBuffer(prerollFrames, AUDCLNT_BUFFERFLAGS_SILENT);

        if (firstSession) {
            state->ring->writeSilence(CushionSamples(baseCushionMs_.load(), state->latencyMs.load(),
                                                       state->sampleRate.load(), state->channels.load()));

            // Only now is it safe for the capture thread to start writing to
            // this output's ring buffer: the pre-roll write above is done, so
            // there is no window where both threads act as producers on the
            // same buffer.
            state->formatReady = true;
        } else {
            // Reconnect: the device may have come back with a different mix
            // format (formatDirty tells the capture thread, the resampler's
            // owner, to reconfigure it), and the ring holds all the audio
            // accumulated while the device was gone -- drop that backlog to
            // resume at the live edge, and rebuild the timing cushion from
            // silence, padded by the capture thread, the ring's sole writer.
            state->formatDirty = true;
            state->ring->drop(state->ring->availableToRead());
            state->silencePadFrames.fetch_add(CushionSamples(baseCushionMs_.load(), state->latencyMs.load(),
                                                               state->sampleRate.load(), state->channels.load()));
        }

        if (FAILED(audioClient->Start())) break;
        ok = true;
    } while (false);

    if (!ok) {
        if (mixFormat) CoTaskMemFree(mixFormat);
        if (renderEvent) CloseHandle(renderEvent);
        return RenderSessionEnd::InitFailed;
    }

    bool dstIsFloat = isFloatFormat(mixFormat);
    CoTaskMemFree(mixFormat);
    mixFormat = nullptr;

    state->active = true;
    std::vector<float> scratch;
    ULONGLONG lastDriftCheck = GetTickCount64();

    BiquadFilter eqFilters[kEqBandCount];
    int curEqDb[kEqBandCount];
    std::fill(std::begin(curEqDb), std::end(curEqDb), INT_MIN);

    // Per-band level meter (see OutputStatus::spectrumBands): independent
    // bandpass analysis filters at the 10 ISO spectrum frequencies (finer
    // grained than the 5-band shaping EQ above), applied to a scratch copy
    // of the actual post-EQ/post-volume signal -- a real spectrum for the
    // UI, without touching what's actually sent to the device. Reconfigured
    // only when the format actually changes, not every callback.
    BiquadFilter meterFilters[kSpectrumBandCount];
    int meterConfiguredChannels = -1, meterConfiguredRate = -1;
    std::vector<float> meterScratch;

    // Set after a buffer underrun or a hard "buffer too full" drop; ramps the
    // next kDeclickFadeMs of playback in from silence instead of jump-cutting
    // straight to it. Local to this thread, touched only here and further down.
    int fadeInFramesRemaining = 0;
    int fadeInFramesTotal = 0;

    // Gain actually applied to the previous sample, carried across callbacks
    // so a volume/mute change (this output's own slider, or the Windows
    // volume control) is ramped in per-sample instead of stepped instantly at
    // a callback boundary -- an instant step is a classic "zipper" click.
    // Starts at 0 so playback also fades in cleanly instead of popping in at
    // full volume on the very first buffer.
    float currentGain = 0.0f;

    RenderSessionEnd end = RenderSessionEnd::Stopped;
    while (running_.load()) {
        // Wait for the device to ask for data instead of polling on a timer.
        // The 200ms timeout only bounds shutdown latency (and lets the drift
        // check below still run if a device stops signaling for a while).
        DWORD waitResult = WaitForSingleObject(renderEvent, 200);
        if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT) {
            end = RenderSessionEnd::Lost;
            break;
        }
        if (!running_.load()) break;

        UINT32 padding = 0;
        if (FAILED(audioClient->GetCurrentPadding(&padding))) {
            end = RenderSessionEnd::Lost; // typically AUDCLNT_E_DEVICE_INVALIDATED
            break;
        }
        // Empty device buffer mid-session = the device ran dry (this thread
        // stalled longer than the fill target): WASAPI played silence, an
        // audible gap. Surfaces in the same ⚠ counter as ring underruns so
        // a fill target set too low for this machine shows up in the UI.
        if (padding == 0 && !state->sourceLost.load()) state->underruns.fetch_add(1);
        // Top the device buffer up to the configured fill target only, not
        // to the whole 150ms allocation: what sits in this buffer is flat
        // latency on every output, while the allocation beyond it is free
        // glitch headroom. Read per callback so the Options slider applies
        // live (a resync then snaps the ring to the shifted total).
        UINT32 fillFrames = static_cast<UINT32>(
            static_cast<int64_t>(deviceFillMs_.load()) * state->sampleRate.load() / 1000);
        if (fillFrames > bufferFrameCount) fillFrames = bufferFrameCount;
        UINT32 framesAvailable = (padding < fillFrames) ? fillFrames - padding : 0;

        if (framesAvailable > 0) {
            BYTE* buf = nullptr;
            if (FAILED(renderClient->GetBuffer(framesAvailable, &buf))) {
                end = RenderSessionEnd::Lost;
                break;
            }
            {
                int channels = state->channels.load();
                int sampleRate = state->sampleRate.load();
                scratch.resize(static_cast<size_t>(framesAvailable) * channels);
                size_t got = state->ring->read(scratch.data(), scratch.size());

                // Ramp in any fade-in requested by a *previous* iteration's
                // underrun/drop (see below) before this iteration can request
                // a new one -- must not act on the same buffer that requests it.
                if (fadeInFramesRemaining > 0) {
                    int framesToFade = std::min(fadeInFramesRemaining, static_cast<int>(framesAvailable));
                    for (int f = 0; f < framesToFade; ++f) {
                        int doneBefore = fadeInFramesTotal - fadeInFramesRemaining;
                        float g = static_cast<float>(doneBefore + f + 1) / fadeInFramesTotal;
                        for (int c = 0; c < channels; ++c) scratch[f * channels + c] *= g;
                    }
                    fadeInFramesRemaining -= framesToFade;
                }

                if (got < scratch.size()) {
                    // Expected silence while the source is gone, a glitch otherwise.
                    if (!state->sourceLost.load()) state->underruns.fetch_add(1);
                    // Fade the tail of the real audio we did get down to zero
                    // instead of cutting it off mid-waveform, and ramp the
                    // next callback's audio back in once real data resumes.
                    int gotFrames = static_cast<int>(got / channels);
                    int fadeOutFrames = std::min(gotFrames, std::max(1, sampleRate * kDeclickFadeMs / 1000));
                    for (int f = 0; f < fadeOutFrames; ++f) {
                        int frameIdx = gotFrames - fadeOutFrames + f;
                        float g = 1.0f - static_cast<float>(f + 1) / fadeOutFrames;
                        for (int c = 0; c < channels; ++c) scratch[frameIdx * channels + c] *= g;
                    }
                    std::fill(scratch.begin() + got, scratch.end(), 0.0f);
                    fadeInFramesTotal = std::max(1, sampleRate * kDeclickFadeMs / 1000);
                    fadeInFramesRemaining = fadeInFramesTotal;
                }

                // EQ before the gain stage: a band boost on a loud passage is
                // exactly what the soft clip after the gain has to catch.
                for (int b = 0; b < kEqBandCount; ++b) {
                    int db = state->eqDb[b].load();
                    if (db != curEqDb[b]) {
                        BiquadFilter::Type type = (b == 0) ? BiquadFilter::Type::LowShelf
                                                 : (b == kEqBandCount - 1) ? BiquadFilter::Type::HighShelf
                                                                           : BiquadFilter::Type::Peaking;
                        eqFilters[b].configure(channels, state->sampleRate.load(), type, kEqBandFreqs[b], db);
                        curEqDb[b] = db;
                    }
                    eqFilters[b].process(scratch.data(), framesAvailable);
                }

                int volPct = state->volumePercent.load();
                // Virtual-cable drivers generally don't apply their own
                // endpoint volume to the audio, which is why the Windows
                // volume control otherwise has no effect while a group is
                // active -- masterVolumeScalar_/masterMuted_ (kept live by
                // captureThreadProc's endpoint volume callback) fold it in here.
                // The Windows master volume mirrors the virtual cable's
                // endpoint -- it only makes sense for outputs fed by the
                // system source; app-sourced outputs ignore it.
                float masterGain = 1.0f;
                if (state->sourcePid == 0) {
                    masterGain = masterMuted_.load() ? 0.0f : masterVolumeScalar_.load();
                }
                // Per-output mute rides the same smoothed gain as the volume
                // slider, so muting fades over one buffer instead of clicking.
                // The output's own percentage sits on a quadratic curve for
                // logarithmic perception; the Windows master scalar is
                // already perceptual (the endpoint's volume taper) and is
                // applied linearly -- squaring it too made the group play
                // at a quarter of native level with Windows at 50%.
                float own = state->muted.load() ? 0.0f : volPct / 100.0f;
                float targetGain = own * own * masterGain;
                size_t frameCount = channels > 0 ? scratch.size() / channels : 0;
                if (frameCount > 0) {
                    if (currentGain == targetGain) {
                        // Steady state (the common case: volume 100%, no
                        // master attenuation) -- skip the per-sample ramp
                        // loop entirely instead of paying for it on every
                        // single callback just to multiply by an unchanging
                        // value, most of the time 1.0.
                        if (targetGain != 1.0f) {
                            for (float& s : scratch) s *= targetGain;
                        }
                    } else {
                        float step = (targetGain - currentGain) / static_cast<float>(frameCount);
                        for (size_t f = 0; f < frameCount; ++f) {
                            currentGain += step;
                            for (int c = 0; c < channels; ++c) scratch[f * channels + c] *= currentGain;
                        }
                        currentGain = targetGain; // land exactly on target, don't drift from rounding
                    }
                }

                // Peak for the VU, then clip protection: whatever went past
                // the knee (EQ boost on a hot passage) is soft-clipped here
                // instead of handed to the Windows mixer to clip hard.
                float pk = 0.0f;
                for (float s : scratch) {
                    float a = s < 0 ? -s : s;
                    if (a > pk) pk = a;
                }
                if (pk > kSoftClipKnee) {
                    for (float& s : scratch) s = SoftClip(s);
                    if (pk > 1.0f) pk = 1.0f;
                }
                int pkMilli = static_cast<int>(pk * 1000.0f);

                // 10-band spectrum, only for the output whose EQ panel is on
                // screen (see setSpectrumTarget): ten biquad passes over a
                // copy of every callback is not worth paying for in the tray.
                if (state->spectrumEnabled.load()) {
                    if (channels != meterConfiguredChannels || sampleRate != meterConfiguredRate) {
                        for (int b = 0; b < kSpectrumBandCount; ++b) {
                            meterFilters[b].configure(channels, sampleRate, BiquadFilter::Type::BandPass,
                                                       kSpectrumBandFreqs[b], 0.0);
                        }
                        meterConfiguredChannels = channels;
                        meterConfiguredRate = sampleRate;
                    }
                    for (int b = 0; b < kSpectrumBandCount; ++b) {
                        meterScratch.assign(scratch.begin(), scratch.end());
                        meterFilters[b].process(meterScratch.data(), framesAvailable);
                        float bpk = 0.0f;
                        for (float s : meterScratch) {
                            float a = s < 0 ? -s : s;
                            if (a > bpk) bpk = a;
                        }
                        int bMilli = static_cast<int>((bpk > 1.0f ? 1.0f : bpk) * 1000.0f);
                        int prevB = state->spectrumMilli[b].load();
                        while (bMilli > prevB &&
                               !state->spectrumMilli[b].compare_exchange_weak(prevB, bMilli)) {
                        }
                    }
                }
                // Atomic compare-exchange instead of load-then-store: the
                // separate load/store raced with getStatus()'s exchange(0),
                // letting a reset land in between and get clobbered by a
                // stale comparison.
                int prevPeak = state->peakMilli.load();
                while (pkMilli > prevPeak &&
                       !state->peakMilli.compare_exchange_weak(prevPeak, pkMilli)) {
                }

                if (dstIsFloat) {
                    memcpy(buf, scratch.data(), scratch.size() * sizeof(float));
                } else {
                    int16_t* dst = reinterpret_cast<int16_t*>(buf);
                    for (size_t i = 0; i < scratch.size(); ++i) {
                        float v = std::clamp(scratch[i], -1.0f, 1.0f);
                        dst[i] = static_cast<int16_t>(v * 32767.0f);
                    }
                }
                renderClient->ReleaseBuffer(framesAvailable, 0);
                framesWritten += framesAvailable;
            }
        }

        ULONGLONG now = GetTickCount64();
        if (now - lastDriftCheck > 1000) { // Check every 1 second
            lastDriftCheck = now;
            // Nothing to rate-match and no writer to service pad requests
            // while the source is gone; the capture session's re-init
            // clears the flag and forces a resync itself when it comes back.
            if (state->sourceLost.load()) {
                state->speedAdjustment.store(1.0);
                continue;
            }
            int channels = state->channels.load();
            int sampleRate = state->sampleRate.load();
            size_t targetSamples = CushionSamples(baseCushionMs_.load(), state->latencyMs.load(),
                                                  sampleRate, channels);
            size_t avail = state->ring->availableToRead();

            // Device-side lag estimate (submitted vs. actually played, as
            // reported by the endpoint's clock): WASAPI buffer + driver/
            // Bluetooth buffering. Smoothed; feeds the auto-align feature.
            if (audioClock && clockFreq > 0) {
                UINT64 pos = 0, qpcPos = 0;
                if (SUCCEEDED(audioClock->GetPosition(&pos, &qpcPos))) {
                    double playedSec = static_cast<double>(pos) / static_cast<double>(clockFreq);
                    double writtenSec = static_cast<double>(framesWritten) / sampleRate;
                    double lagMs = (writtenSec - playedSec) * 1000.0;
                    if (lagMs >= 0.0 && lagMs < 3000.0) {
                        int prev = state->deviceLagMs.load();
                        int smoothed = (prev == 0) ? static_cast<int>(lagMs)
                                                   : static_cast<int>(prev * 0.7 + lagMs * 0.3);
                        state->deviceLagMs.store(smoothed);
                    }
                }
            }

            // Deliberate latency change (slider commit / auto-align): jump
            // straight to the new target instead of drifting there at
            // ±0.15% -- a 200ms move would otherwise take over two minutes.
            if (state->forceResync.exchange(false)) {
                state->ringOverflow = false; // next overflow episode counts again
                if (avail > targetSamples) {
                    size_t drop = avail - targetSamples;
                    drop -= drop % channels;
                    if (drop > 0) {
                        state->ring->drop(drop);
                        // Jumping forward in the stream: ramp back in.
                        fadeInFramesTotal = std::max(1, sampleRate * kDeclickFadeMs / 1000);
                        fadeInFramesRemaining = fadeInFramesTotal;
                    }
                } else if (avail < targetSamples) {
                    size_t add = targetSamples - avail;
                    add -= add % channels;
                    if (add > 0) state->silencePadFrames.fetch_add(add);
                }
                state->speedAdjustment.store(1.0);
                continue; // fresh P-controller measurement next tick
            }

            // P-controller for smooth clock drift correction:
            // Calculate buffer error in milliseconds.
            double errorMs = (static_cast<double>(avail) - static_cast<double>(targetSamples)) / 
                             (static_cast<double>(sampleRate * channels) / 1000.0);

            // Kp proportional gain: 20ms error -> 0.05% (0.0005) speed adjustment.
            // Positive error: buffer is too full, speed up resampler (decrease outputs samples rate -> steps input faster)
            // Negative error: buffer is too empty, slow down resampler (increase outputs samples rate -> steps input slower)
            constexpr double Kp = 0.000025; 
            double adj = 1.0 + errorMs * Kp;

            // Clamp to +/- 0.15% to ensure it's completely inaudible
            if (adj < 0.9985) adj = 0.9985;
            if (adj > 1.0015) adj = 1.0015;

            state->speedAdjustment.store(adj);

            // Hard safety net fallback for extreme desynchronization (e.g. system sleep/resume)
            if (avail > targetSamples * 3) {
                size_t drop = (avail - targetSamples);
                drop -= drop % channels;
                if (drop > 0) {
                    state->ring->drop(drop);
                    state->ringOverflow = false;
                    state->driftCorrections.fetch_add(1);
                    state->speedAdjustment.store(1.0);
                    // The very next read() jumps to a different point in the
                    // stream; ramp it in instead of cutting straight to it.
                    fadeInFramesTotal = std::max(1, sampleRate * kDeclickFadeMs / 1000);
                    fadeInFramesRemaining = fadeInFramesTotal;
                }
            } else if (avail < targetSamples / 4) {
                size_t add = targetSamples - avail;
                add -= add % channels;
                if (add > 0) {
                    // This thread only reads from the ring; only the capture
                    // thread (the ring's sole writer) may call writeSilence.
                    state->silencePadFrames.fetch_add(add);
                    state->driftCorrections.fetch_add(1);
                    state->speedAdjustment.store(1.0);
                }
            }
        }
    }

    audioClient->Stop();
    state->active = false;
    CloseHandle(renderEvent);
    return end;
}
