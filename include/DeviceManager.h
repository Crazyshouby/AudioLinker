#pragma once

#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <string>
#include <vector>

// Coarse physical category, guessed from the endpoint's enumerator name
// (Bluetooth) and PKEY_AudioEndpoint_FormFactor (everything else) -- good
// enough to pick a representative icon, not a guaranteed hardware truth
// (some drivers just report Speakers/UnknownFormFactor for everything).
enum class AudioDeviceKind { Speaker, Headphones, Hdmi, Digital, Bluetooth };

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
    bool isRender = false; // true = output (playback), false = capture (input)
    AudioDeviceKind kind = AudioDeviceKind::Speaker;
};

class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    bool isValid() const { return enumerator_ != nullptr; }

    std::vector<AudioDeviceInfo> enumerateRenderDevices();
    std::vector<AudioDeviceInfo> enumerateCaptureDevices();

    // Returns a live, ref-counted IMMDevice for the given endpoint id, or
    // nullptr if the device is no longer present (unplugged, disabled...).
    Microsoft::WRL::ComPtr<IMMDevice> getDeviceById(const std::wstring& id);

    // Hot-plug notifications (device added/removed/enabled/disabled).
    // Callbacks arrive on an arbitrary COM worker thread -- the client must
    // marshal to its own thread (e.g. PostMessage) before touching UI state.
    bool registerNotifications(IMMNotificationClient* client);
    void unregisterNotifications(IMMNotificationClient* client);

private:
    std::vector<AudioDeviceInfo> enumerate(EDataFlow flow);

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    bool comInitializedHere_ = false;
};
