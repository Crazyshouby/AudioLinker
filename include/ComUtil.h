#pragma once

#include <mmdeviceapi.h>
#include <objbase.h>
#include <wrl/client.h>

// COM lifetime for a worker thread (MTA). ownsInit is false when the thread
// was already initialized in an incompatible mode -- callers bail out, since
// nothing audio-related would work anyway.
struct ComScope {
    bool ownsInit = false;
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ownsInit = (hr == S_OK || hr == S_FALSE);
    }
    ~ComScope() {
        if (ownsInit) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

inline Microsoft::WRL::ComPtr<IMMDeviceEnumerator> CreateDeviceEnumerator() {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator),
                     reinterpret_cast<void**>(enumerator.GetAddressOf()));
    return enumerator;
}
