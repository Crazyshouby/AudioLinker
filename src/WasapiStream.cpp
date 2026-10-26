#include "WasapiStream.h"
#include "Log.h"

#include <ksmedia.h>
#include <mmreg.h>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

bool IsFloatFormat(const WAVEFORMATEX* wfx) {
    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

bool IsSupportedFormat(const WAVEFORMATEX* wfx) {
    return IsFloatFormat(wfx) || wfx->wBitsPerSample == 16;
}

StreamFormat DescribeFormat(const WAVEFORMATEX* wfx) {
    StreamFormat f;
    f.channels = wfx->nChannels;
    f.rate = static_cast<int>(wfx->nSamplesPerSec);
    f.isFloat = IsFloatFormat(wfx);
    return f;
}

SharedClient::~SharedClient() {
    if (mix) CoTaskMemFree(mix);
}

SharedClient::SharedClient(SharedClient&& other) noexcept
    : device(std::move(other.device)), client(std::move(other.client)),
      format(other.format), mix(other.mix) {
    other.mix = nullptr;
}

SharedClient& SharedClient::operator=(SharedClient&& other) noexcept {
    if (this != &other) {
        if (mix) CoTaskMemFree(mix);
        device = std::move(other.device);
        client = std::move(other.client);
        format = other.format;
        mix = other.mix;
        other.mix = nullptr;
    }
    return *this;
}

bool OpenSharedClient(IMMDeviceEnumerator* enumerator, const std::wstring& deviceId,
                      SharedClient& out, const wchar_t* label) {
    std::wstring tag = label ? label : deviceId;
    SharedClient sc;
    HRESULT hr = enumerator->GetDevice(deviceId.c_str(), sc.device.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR(L"GetDevice (" + tag + L") : " + log_detail::Hr(hr));
        return false;
    }
    hr = sc.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(sc.client.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ERROR(L"Activate IAudioClient (" + tag + L") : " + log_detail::Hr(hr));
        return false;
    }
    hr = sc.client->GetMixFormat(&sc.mix);
    if (FAILED(hr) || !sc.mix) {
        LOG_ERROR(L"GetMixFormat (" + tag + L") : " + log_detail::Hr(hr));
        return false;
    }
    if (!IsSupportedFormat(sc.mix)) {
        LOG_WARN(L"Format non géré (" + tag + L") : bits/éch = " +
                 std::to_wstring(sc.mix->wBitsPerSample) + L" — flux ignoré.");
        return false;
    }
    sc.format = DescribeFormat(sc.mix);
    out = std::move(sc);
    return true;
}

bool InitRenderStream(IAudioClient* client, const WAVEFORMATEX* fmt, HANDLE evt, bool lowLatency,
                      UINT32* bufferFrames, const std::wstring& label) {
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

void ToFloat(const BYTE* src, size_t samples, const StreamFormat& fmt, float* dst) {
    if (fmt.isFloat) {
        std::memcpy(dst, src, samples * sizeof(float));
        return;
    }
    const int16_t* s = reinterpret_cast<const int16_t*>(src);
    for (size_t i = 0; i < samples; ++i) dst[i] = s[i] / 32768.0f;
}

void FromFloat(const float* src, size_t samples, const StreamFormat& fmt, BYTE* dst) {
    if (fmt.isFloat) {
        std::memcpy(dst, src, samples * sizeof(float));
        return;
    }
    int16_t* d = reinterpret_cast<int16_t*>(dst);
    for (size_t i = 0; i < samples; ++i) {
        d[i] = static_cast<int16_t>(std::clamp(src[i], -1.0f, 1.0f) * 32767.0f);
    }
}

float PeakAbs(const BYTE* data, size_t samples, const StreamFormat& fmt) {
    float peak = 0.0f;
    if (fmt.isFloat) {
        const float* f = reinterpret_cast<const float*>(data);
        for (size_t i = 0; i < samples; ++i) {
            float a = f[i] < 0.0f ? -f[i] : f[i];
            if (a > peak) peak = a;
        }
    } else {
        const int16_t* p = reinterpret_cast<const int16_t*>(data);
        for (size_t i = 0; i < samples; ++i) {
            float a = (p[i] < 0 ? -p[i] : p[i]) / 32768.0f;
            if (a > peak) peak = a;
        }
    }
    return peak;
}
