#define INITGUID
#include "DeviceManager.h"
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

using Microsoft::WRL::ComPtr;

DeviceManager::DeviceManager() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        comInitializedHere_ = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        return; // some other failure; enumerator_ stays null
    }

    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                      __uuidof(IMMDeviceEnumerator),
                      reinterpret_cast<void**>(enumerator_.GetAddressOf()));
}

DeviceManager::~DeviceManager() {
    enumerator_.Reset();
    if (comInitializedHere_) {
        CoUninitialize();
    }
}

std::vector<AudioDeviceInfo> DeviceManager::enumerate(EDataFlow flow) {
    std::vector<AudioDeviceInfo> result;
    if (!enumerator_) return result;

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.GetAddressOf()))) {
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);

    std::wstring defaultId;
    ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, defaultDevice.GetAddressOf()))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.GetAddressOf()))) continue;

        LPWSTR idRaw = nullptr;
        if (FAILED(device->GetId(&idRaw))) continue;
        std::wstring id = idRaw;
        CoTaskMemFree(idRaw);

        ComPtr<IPropertyStore> props;
        std::wstring name = L"(nom inconnu)";
        AudioDeviceKind kind = AudioDeviceKind::Speaker;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.GetAddressOf()))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR) {
                name = varName.pwszVal;
            }
            PropVariantClear(&varName);

            // Bluetooth takes priority over the form factor below: a BT
            // headset reports Headphones/Headset just like a wired one, but
            // its wireless link is the more useful thing to flag at a glance.
            PROPVARIANT varEnum;
            PropVariantInit(&varEnum);
            bool isBluetooth = false;
            if (SUCCEEDED(props->GetValue(PKEY_Device_EnumeratorName, &varEnum)) && varEnum.vt == VT_LPWSTR) {
                isBluetooth = _wcsnicmp(varEnum.pwszVal, L"BTH", 3) == 0;
            }
            PropVariantClear(&varEnum);

            if (isBluetooth) {
                kind = AudioDeviceKind::Bluetooth;
            } else {
                PROPVARIANT varForm;
                PropVariantInit(&varForm);
                if (SUCCEEDED(props->GetValue(PKEY_AudioEndpoint_FormFactor, &varForm)) && varForm.vt == VT_UI4) {
                    switch (static_cast<EndpointFormFactor>(varForm.uintVal)) {
                        case Headphones:
                        case Headset:
                            kind = AudioDeviceKind::Headphones;
                            break;
                        case DigitalAudioDisplayDevice:
                            kind = AudioDeviceKind::Hdmi;
                            break;
                        case SPDIF:
                        case UnknownDigitalPassthrough:
                            kind = AudioDeviceKind::Digital;
                            break;
                        default:
                            kind = AudioDeviceKind::Speaker;
                            break;
                    }
                }
                PropVariantClear(&varForm);
            }
        }

        AudioDeviceInfo info;
        info.id = id;
        info.name = name;
        info.isDefault = (id == defaultId);
        info.isRender = (flow == eRender);
        info.kind = kind;
        result.push_back(std::move(info));
    }

    return result;
}

std::vector<AudioDeviceInfo> DeviceManager::enumerateRenderDevices() {
    return enumerate(eRender);
}

std::vector<AudioDeviceInfo> DeviceManager::enumerateCaptureDevices() {
    return enumerate(eCapture);
}

ComPtr<IMMDevice> DeviceManager::getDeviceById(const std::wstring& id) {
    ComPtr<IMMDevice> device;
    if (!enumerator_) return device;
    enumerator_->GetDevice(id.c_str(), device.GetAddressOf());
    return device;
}

bool DeviceManager::registerNotifications(IMMNotificationClient* client) {
    if (!enumerator_ || !client) return false;
    return SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(client));
}

void DeviceManager::unregisterNotifications(IMMNotificationClient* client) {
    if (enumerator_ && client) {
        enumerator_->UnregisterEndpointNotificationCallback(client);
    }
}
