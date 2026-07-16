#include "Calibrator.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace {

constexpr double kPi = 3.14159265358979323846;
// Sweep parameters: 500Hz -> 4kHz sits inside every speaker's and mic's
// usable band (small BT speakers roll off hard below ~200Hz); 150ms is long
// enough for a sharp correlation peak yet keeps the whole pass quick.
constexpr double kChirpF0 = 500.0, kChirpF1 = 4000.0;
constexpr double kChirpSec = 0.15;
constexpr double kChirpAmp = 0.5;
// Silence played before the sweep: Bluetooth links ramp up / unmute over the
// first few hundred ms of a fresh stream, which would swallow the sweep.
constexpr double kWarmupSec = 1.0;
// Listening window after the sweep's nominal DAC time: must exceed the
// largest plausible device latency (BT can reach ~500ms) plus the room's
// reverb tail.
constexpr double kListenSec = 1.5;
// The correlation peak must stand this far above the window's mean |corr|
// to count as a detection rather than noise.
constexpr double kMinPeakRatio = 6.0;

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

// QPC in 100ns units -- the same unit IAudioCaptureClient::GetBuffer reports
// its packet timestamps in, so the two are directly comparable. Split
// arithmetic avoids the int64 overflow of a naive (qpc * 1e7) on machines
// with weeks of uptime.
INT64 NowHns() {
    static const INT64 freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / freq) * 10'000'000LL + (c.QuadPart % freq) * 10'000'000LL / freq;
}

bool IsFloatFormat(const WAVEFORMATEX* wfx) {
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

// Logarithmic sine sweep with short fades so it starts/ends without clicks.
std::vector<float> MakeChirp(int rate) {
    const int n = static_cast<int>(rate * kChirpSec);
    const int fade = std::max(1, rate * 5 / 1000);
    const double k = std::log(kChirpF1 / kChirpF0);
    std::vector<float> s(n);
    for (int i = 0; i < n; ++i) {
        double t = static_cast<double>(i) / rate;
        double phase = 2.0 * kPi * kChirpF0 * kChirpSec / k * (std::exp(t / kChirpSec * k) - 1.0);
        double env = 1.0;
        if (i < fade) env = static_cast<double>(i) / fade;
        else if (i >= n - fade) env = static_cast<double>(n - 1 - i) / fade;
        s[i] = static_cast<float>(kChirpAmp * env * std::sin(phase));
    }
    return s;
}

// Mono recording of the calibration mic, annotated with the QPC timestamp of
// each packet's first frame so sample indices convert to/from wall-clock.
class MicRecorder {
public:
    bool open(IMMDeviceEnumerator* enumerator, const std::wstring& micId) {
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDevice(micId.c_str(), device.GetAddressOf()))) return false;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(client_.GetAddressOf())))) return false;
        WAVEFORMATEX* fmt = nullptr;
        if (FAILED(client_->GetMixFormat(&fmt)) || !fmt) return false;
        isFloat_ = IsFloatFormat(fmt);
        channels_ = fmt->nChannels;
        rate_ = static_cast<int>(fmt->nSamplesPerSec);
        bool supported = isFloat_ || fmt->wBitsPerSample == 16;
        HRESULT hr = supported
            ? client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 5'000'000 /*500ms*/, 0, fmt, nullptr)
            : E_FAIL;
        CoTaskMemFree(fmt);
        if (FAILED(hr)) return false;
        if (FAILED(client_->GetService(__uuidof(IAudioCaptureClient),
                                        reinterpret_cast<void**>(capture_.GetAddressOf())))) return false;
        return SUCCEEDED(client_->Start());
    }

    void stop() {
        if (client_) client_->Stop();
    }

    int rate() const { return rate_; }

    // Drains everything currently buffered; call every few ms while playing.
    bool drain() {
        // Runaway guard: a pass is tens of seconds at most.
        if (samples_.size() > static_cast<size_t>(rate_) * 180) return false;
        UINT32 packet = 0;
        if (FAILED(capture_->GetNextPacketSize(&packet))) return false;
        while (packet != 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devPos = 0, qpc = 0;
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, &devPos, &qpc))) return false;
            INT64 hns;
            if (qpc != 0 && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                hns = static_cast<INT64>(qpc);
            } else if (!marks_.empty()) {
                // Driver timestamp unusable: extrapolate from the previous
                // packet at the nominal rate.
                hns = marks_.back().hns +
                      static_cast<INT64>(samples_.size() - marks_.back().sample) * 10'000'000LL / rate_;
            } else {
                hns = NowHns();
            }
            marks_.push_back({ samples_.size(), hns });
            const size_t base = samples_.size();
            samples_.resize(base + frames);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(samples_.begin() + base, samples_.end(), 0.0f);
            } else if (isFloat_) {
                const float* src = reinterpret_cast<const float*>(data);
                for (UINT32 f = 0; f < frames; ++f) {
                    float sum = 0.0f;
                    for (int c = 0; c < channels_; ++c) sum += src[static_cast<size_t>(f) * channels_ + c];
                    samples_[base + f] = sum / channels_;
                }
            } else {
                const int16_t* src = reinterpret_cast<const int16_t*>(data);
                for (UINT32 f = 0; f < frames; ++f) {
                    int sum = 0;
                    for (int c = 0; c < channels_; ++c) sum += src[static_cast<size_t>(f) * channels_ + c];
                    samples_[base + f] = static_cast<float>(sum) / (channels_ * 32768.0f);
                }
            }
            capture_->ReleaseBuffer(frames);
            if (FAILED(capture_->GetNextPacketSize(&packet))) return false;
        }
        return true;
    }

    size_t hnsToSample(INT64 hns) const {
        for (size_t i = marks_.size(); i-- > 0;) {
            if (marks_[i].hns <= hns) {
                size_t s = marks_[i].sample +
                           static_cast<size_t>((hns - marks_[i].hns) * rate_ / 10'000'000LL);
                return std::min(s, samples_.size());
            }
        }
        return 0; // before the first packet (or nothing recorded yet)
    }

    INT64 sampleToHns(size_t sample) const {
        for (size_t i = marks_.size(); i-- > 0;) {
            if (marks_[i].sample <= sample) {
                return marks_[i].hns +
                       static_cast<INT64>(sample - marks_[i].sample) * 10'000'000LL / rate_;
            }
        }
        return 0;
    }

    const std::vector<float>& samples() const { return samples_; }

private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    int channels_ = 2;
    int rate_ = 48000;
    bool isFloat_ = true;
    std::vector<float> samples_;
    struct Mark { size_t sample; INT64 hns; };
    std::vector<Mark> marks_;
};

// Plays warmup silence + the sweep + a listening tail on one output (its own
// standalone shared-mode stream), keeping the mic drained throughout. The
// device buffer is topped up to fillMs only, replicating the engine's capped
// fill. On success, dacHnsOut is when the sweep's first sample reached the
// DAC (per GetCurrentPadding at submit time) and backlogMsOut the fill
// actually applied -- the backlog group playback keeps, added back so the
// measured delay models it.
bool PlaySweep(IMMDeviceEnumerator* enumerator, const std::wstring& deviceId,
               MicRecorder& mic, const std::atomic<bool>& cancel, int fillMs,
               INT64& dacHnsOut, int& backlogMsOut) {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    if (FAILED(enumerator->GetDevice(deviceId.c_str(), device.GetAddressOf()))) return false;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(client.GetAddressOf())))) return false;
    WAVEFORMATEX* fmt = nullptr;
    if (FAILED(client->GetMixFormat(&fmt)) || !fmt) return false;
    const bool isFloat = IsFloatFormat(fmt);
    const int channels = fmt->nChannels;
    const int rate = static_cast<int>(fmt->nSamplesPerSec);
    bool supported = isFloat || fmt->wBitsPerSample == 16;
    // Same 150ms buffer the engine's render sessions request, so bufferMsOut
    // (the "kept full" backlog) matches what group playback will really use.
    HRESULT hr = supported
        ? client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1'500'000, 0, fmt, nullptr)
        : E_FAIL;
    CoTaskMemFree(fmt);
    if (FAILED(hr)) return false;
    UINT32 bufferFrames = 0;
    if (FAILED(client->GetBufferSize(&bufferFrames)) || bufferFrames == 0) return false;
    if (FAILED(client->GetService(__uuidof(IAudioRenderClient),
                                   reinterpret_cast<void**>(render.GetAddressOf())))) return false;
    UINT32 fillFrames = static_cast<UINT32>(static_cast<INT64>(fillMs) * rate / 1000);
    if (fillFrames > bufferFrames) fillFrames = bufferFrames;
    backlogMsOut = static_cast<int>(static_cast<INT64>(fillFrames) * 1000 / rate);

    const std::vector<float> chirp = MakeChirp(rate);
    INT64 silenceLeft = static_cast<INT64>(rate * kWarmupSec);
    size_t chirpPos = 0;
    INT64 tailLeft = static_cast<INT64>(rate * (kListenSec + 0.2));
    bool dacSet = false;

    if (FAILED(client->Start())) return false;
    while (!cancel.load()) {
        if (!mic.drain()) break;
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        UINT32 avail = (padding < fillFrames) ? fillFrames - padding : 0;
        if (avail > 0) {
            if (silenceLeft > 0) {
                UINT32 n = static_cast<UINT32>(std::min<INT64>(avail, silenceLeft));
                BYTE* buf = nullptr;
                if (FAILED(render->GetBuffer(n, &buf))) break;
                render->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
                silenceLeft -= n;
            } else if (chirpPos < chirp.size()) {
                UINT32 n = static_cast<UINT32>(std::min<size_t>(avail, chirp.size() - chirpPos));
                BYTE* buf = nullptr;
                if (FAILED(render->GetBuffer(n, &buf))) break;
                if (chirpPos == 0) {
                    // `padding` frames are queued ahead of this write; the
                    // sweep's first sample reaches the DAC once they play out.
                    dacHnsOut = NowHns() + static_cast<INT64>(padding) * 10'000'000LL / rate;
                    dacSet = true;
                }
                for (UINT32 f = 0; f < n; ++f) {
                    float v = chirp[chirpPos + f];
                    if (isFloat) {
                        float* dst = reinterpret_cast<float*>(buf) + static_cast<size_t>(f) * channels;
                        for (int c = 0; c < channels; ++c) dst[c] = v;
                    } else {
                        auto s16 = static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
                        int16_t* dst = reinterpret_cast<int16_t*>(buf) + static_cast<size_t>(f) * channels;
                        for (int c = 0; c < channels; ++c) dst[c] = s16;
                    }
                }
                render->ReleaseBuffer(n, 0);
                chirpPos += n;
            } else if (tailLeft > 0) {
                UINT32 n = static_cast<UINT32>(std::min<INT64>(avail, tailLeft));
                BYTE* buf = nullptr;
                if (FAILED(render->GetBuffer(n, &buf))) break;
                render->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
                tailLeft -= n;
            } else {
                break; // whole plan submitted; the sweep played long ago
            }
        }
        Sleep(4);
    }
    client->Stop();
    return dacSet && !cancel.load();
}

// Locates the sweep in the recording within [dacHns - 50ms, dacHns +
// kListenSec] by cross-correlation against the reference and returns its
// arrival time. False if no peak stands convincingly above the noise.
bool FindSweep(const MicRecorder& mic, const std::vector<float>& ref,
               INT64 dacHns, const std::atomic<bool>& cancel, INT64& peakHnsOut) {
    const auto& s = mic.samples();
    size_t begin = mic.hnsToSample(dacHns - 500'000);
    size_t end = std::min(s.size(), mic.hnsToSample(dacHns + static_cast<INT64>(kListenSec * 1e7)));
    if (ref.size() < 100 || end <= begin + ref.size()) return false;
    const size_t range = end - begin - ref.size();
    double best = 0.0, sumAbs = 0.0;
    size_t bestK = 0;
    for (size_t k = 0; k < range; ++k) {
        if ((k & 0x3FFF) == 0 && cancel.load()) return false;
        const float* w = s.data() + begin + k;
        double acc = 0.0;
        for (size_t j = 0; j < ref.size(); ++j) acc += static_cast<double>(ref[j]) * w[j];
        double a = std::abs(acc);
        sumAbs += a;
        if (a > best) {
            best = a;
            bestK = k;
        }
    }
    double mean = sumAbs / static_cast<double>(range);
    if (mean <= 0.0 || best / mean < kMinPeakRatio) return false;
    peakHnsOut = mic.sampleToHns(begin + bestK);
    return true;
}

} // namespace

Calibrator::~Calibrator() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

bool Calibrator::start(const std::wstring& micId, const std::vector<std::wstring>& outputIds,
                       int deviceFillMs) {
    if (running_.load()) return false;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    deviceFillMs_ = deviceFillMs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        outcomes_.clear();
        for (const auto& id : outputIds) outcomes_.push_back({ id });
    }
    running_ = true;
    worker_ = std::thread(&Calibrator::worker, this, micId);
    return true;
}

void Calibrator::cancel() {
    cancel_ = true;
}

Calibrator::Snapshot Calibrator::snapshot() {
    Snapshot s;
    s.running = running_.load();
    s.cancelled = cancel_.load();
    std::lock_guard<std::mutex> lock(mutex_);
    s.error = error_;
    s.outcomes = outcomes_;
    return s;
}

void Calibrator::setState(size_t idx, CalState st, int delayMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (idx < outcomes_.size()) {
        outcomes_[idx].state = st;
        outcomes_[idx].delayMs = delayMs;
    }
}

void Calibrator::fail(const std::wstring& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = error;
}

void Calibrator::worker(std::wstring micId) {
    ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (com.ownsInit) {
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator),
                         reinterpret_cast<void**>(enumerator.GetAddressOf()));
    }
    if (!enumerator) {
        fail(L"Initialisation audio impossible pour le calage micro.");
        running_ = false;
        return;
    }

    MicRecorder mic;
    if (!mic.open(enumerator.Get(), micId)) {
        fail(L"Impossible d'ouvrir le microphone sélectionné.");
        running_ = false;
        return;
    }
    const std::vector<float> ref = MakeChirp(mic.rate());

    std::vector<std::wstring> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& o : outcomes_) ids.push_back(o.deviceId);
    }

    for (size_t i = 0; i < ids.size() && !cancel_.load(); ++i) {
        setState(i, CalState::Measuring);
        INT64 dacHns = 0;
        int backlogMs = 0;
        bool played = PlaySweep(enumerator.Get(), ids[i], mic, cancel_, deviceFillMs_,
                                dacHns, backlogMs);
        if (played && !cancel_.load()) {
            // Let the recording catch up with the sweep's tail end.
            Sleep(150);
            mic.drain();
        }
        INT64 peakHns = 0;
        if (played && !cancel_.load() && FindSweep(mic, ref, dacHns, cancel_, peakHns)) {
            // Post-buffer delay measured by the mic, plus the backlog a
            // group render session keeps in the device buffer -- PlaySweep
            // replicated the same capped fill.
            INT64 ms = (peakHns - dacHns) / 10'000 + backlogMs;
            setState(i, CalState::Done, static_cast<int>(std::clamp<INT64>(ms, 0, 5000)));
        } else if (!cancel_.load()) {
            setState(i, CalState::Failed);
        }
    }

    mic.stop();
    running_ = false;
}
