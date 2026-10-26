#pragma once

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <string>

// The one shared-mode negotiation every stream in this app performs (group
// render/capture, identify beeps, mic meters, the calibrator's recorder and
// sweep player), in one place: find the endpoint, activate IAudioClient,
// read its mix format and make sure the sample layout is one we convert.
// Initialize() stays with the caller -- each site has its own buffer size,
// flags and low-latency policy.

// Sample layout of a shared-mode stream, reduced to what the engine needs.
// Shared-mode mix formats are float32 in practice; 16-bit PCM is the only
// other layout handled (anything else is refused rather than reinterpreted
// as the wrong type and played as noise).
struct StreamFormat {
    int channels = 2;
    int rate = 48000;
    bool isFloat = true;
};

bool IsFloatFormat(const WAVEFORMATEX* wfx);
bool IsSupportedFormat(const WAVEFORMATEX* wfx);
StreamFormat DescribeFormat(const WAVEFORMATEX* wfx);

struct SharedClient {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    Microsoft::WRL::ComPtr<IAudioClient> client;
    StreamFormat format;
    WAVEFORMATEX* mix = nullptr; // the negotiated mix format, owned (CoTaskMem)

    SharedClient() = default;
    ~SharedClient();
    SharedClient(SharedClient&& other) noexcept;
    SharedClient& operator=(SharedClient&& other) noexcept;
    SharedClient(const SharedClient&) = delete;
    SharedClient& operator=(const SharedClient&) = delete;
};

// False on any failure (device gone, activation refused, unsupported
// format); the cause is written to the log with `label` (the device id by
// default) so a "won't start" report has something attached.
bool OpenSharedClient(IMMDeviceEnumerator* enumerator, const std::wstring& deviceId,
                      SharedClient& out, const wchar_t* label = nullptr);

// Interleaved sample conversion, `samples` = frames * channels.
void ToFloat(const BYTE* src, size_t samples, const StreamFormat& fmt, float* dst);
void FromFloat(const float* src, size_t samples, const StreamFormat& fmt, BYTE* dst); // clamps for int16
float PeakAbs(const BYTE* data, size_t samples, const StreamFormat& fmt);
