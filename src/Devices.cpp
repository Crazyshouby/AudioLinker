// Output device list: enumeration filtered of virtual endpoints, user
// ordering, recognition of a device back under a new endpoint id, and the
// hot-plug notification sink.

#include "App.h"

namespace al {

class DeviceNotificationClient : public IMMNotificationClient {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override { return ref_.fetch_sub(1) - 1; }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { notify(); return S_OK; }
    // Only the console render role matters: it is what a running group
    // hijacks. Our own SetDefaultEndpoint calls come through here too; the
    // handler filters them out by timestamp (see WM_DEFAULT_CHANGED).
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole && g_mainWnd) {
            PostMessageW(g_mainWnd, WM_DEFAULT_CHANGED, 0, 0);
        }
        return S_OK;
    }
    // Ignored: fires constantly (volume, jack info...), never list-relevant.
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void notify() {
        if (g_mainWnd) PostMessageW(g_mainWnd, WM_DEVICES_CHANGED, 0, 0);
    }
    std::atomic<ULONG> ref_{1};
};

DeviceNotificationClient g_deviceNotifications;

IMMNotificationClient& DeviceNotificationSink() { return g_deviceNotifications; }

// The VB-Cable endpoint is the group's public face in Windows; it must never
// appear as a group member (instant feedback loop) and the leftover
// Voicemeeter endpoints are just noise, so all of them are hidden.
bool IsVirtualDevice(const std::wstring& name) {
    return name.find(L"VB-Audio") != std::wstring::npos ||
           name.find(L"CABLE") != std::wstring::npos ||
           name.find(L"Voicemeeter") != std::wstring::npos ||
           name == L"(nom inconnu)";
}

bool IsCableDevice(const std::wstring& name) {
    if (name.find(L"16ch") != std::wstring::npos) return false;
    return name.find(L"VB-Audio Virtual Cable") != std::wstring::npos ||
           name.find(L"CABLE Input") != std::wstring::npos ||
           name.find(L"CABLE Output") != std::wstring::npos;
}


// --- Device list ---

// User-arranged order first (drag & drop), then enumeration order for
// devices that were never moved (stable_sort keeps their relative order).
void SortOutputDevicesByOrder() {
    std::stable_sort(g_outputDevices.begin(), g_outputDevices.end(),
                     [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
                         return g_settings[a.id].order < g_settings[b.id].order;
                     });
}

// Endpoint ids are not stable for life: re-pairing a Bluetooth speaker or
// moving a USB DAC to another port hands it a brand-new id, and all its
// settings (latency, EQ, alias, color, setup membership) would silently
// start over. When an unknown id shows up, look through the config for a
// device whose last-seen name matches and whose own id is no longer
// enumerated: same device, new id -- carry everything over.
std::wstring FindOrphanByName(const std::wstring& name, const std::set<std::wstring>& presentIds) {
    if (name.empty()) return L"";
    for (const auto& [id, s] : g_settings) {
        if (!presentIds.count(id) && s.name == name) return id;
    }
    return L"";
}

void MigrateDeviceId(const std::wstring& oldId, const std::wstring& newId) {
    g_settings[newId] = g_settings[oldId];
    g_settings.erase(oldId);
    for (auto& setup : g_setups) {
        for (auto& [id, s] : setup.devices) {
            if (id == oldId) id = newId;
        }
    }
    LOG_INFO(L"Périphérique reconnu sous un nouvel identifiant, réglages transférés : " + newId);
}

// --- Automation: "when this speaker connects / disconnects" rules ---

std::wstring g_pendingAutoSetup;
std::set<std::wstring> g_lastPresentIds;
bool g_presentIdsKnown = false; // the first enumeration is not a "connection"

void RunAutomation(const std::set<std::wstring>& before, const std::set<std::wstring>& after) {
    for (const auto& r : g_cfg.automation) {
        bool was = before.count(r.deviceId) != 0;
        bool now = after.count(r.deviceId) != 0;
        if (!was && now && !r.onConnect.empty()) {
            LOG_INFO(L"Automatisation : enceinte connectée, setup « " + r.onConnect + L" » dans " +
                     std::to_wstring(kAutomationDelayMs) + L" ms.");
            g_pendingAutoSetup = r.onConnect;
            if (g_mainWnd) SetTimer(g_mainWnd, IDT_AUTOMATION, kAutomationDelayMs, nullptr);
        }
        if (was && !now && r.onDisconnect == L"stop" && (g_engine.isRunning() || g_engine.isStarting())) {
            LOG_INFO(L"Automatisation : enceinte déconnectée, arrêt du groupe.");
            if (g_mainWnd) OnStop(g_mainWnd);
        }
    }
}

void PopulateOutputList() {
    g_outputDevices.clear();
    auto devices = g_deviceManager->enumerateRenderDevices();
    std::set<std::wstring> presentIds;
    for (const auto& d : devices) presentIds.insert(d.id);
    if (g_presentIdsKnown && presentIds != g_lastPresentIds) RunAutomation(g_lastPresentIds, presentIds);
    g_lastPresentIds = presentIds;
    g_presentIdsKnown = true;
    const long long now = static_cast<long long>(std::time(nullptr));
    bool dirty = false;
    for (auto& d : devices) {
        if (IsVirtualDevice(d.name)) continue;
        if (g_settings.find(d.id) == g_settings.end()) {
            std::wstring orphan = FindOrphanByName(d.name, presentIds);
            if (!orphan.empty()) MigrateDeviceId(orphan, d.id);
            else g_settings[d.id] = OutputSettings{};
            dirty = true;
        }
        OutputSettings& s = g_settings[d.id];
        if (s.name != d.name) { s.name = d.name; dirty = true; }
        s.lastSeen = now;
        g_outputDevices.push_back(d);
    }
    if (dirty) SaveConfig();
    SortOutputDevicesByOrder();
}

} // namespace al
