#pragma once

#include <mmdeviceapi.h>
#include <mmreg.h>

// Undocumented COM interface used by the Windows sound control panel to set
// the default audio endpoint and edit endpoint properties. Stable since
// Windows 7 and relied upon by many audio switchers (SoundSwitch, etc.).
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8"))
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    // Windows 10/11 variant: bFxStore selects the effects property store
    // (pass 0 for the normal store). Verified working on this machine; the
    // older 3-argument variant returns S_OK but writes nothing useful.
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, INT bFxStore, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, INT bFxStore, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

struct __declspec(uuid("870af99c-171d-4f9e-af0d-e63df40c2bc9")) CPolicyConfigClient;
