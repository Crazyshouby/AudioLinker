// Everything that reaches into Windows' audio policy on the app's behalf:
// default endpoint (IPolicyConfig), endpoint rename, capture levels, mic
// metering, per-app endpoint routing (AudioPolicyConfig), source apps,
// crash repair, plus the HKCU Run entry.

#include "App.h"

namespace al {

// --- Launch at Windows startup (HKCU Run key) ---

constexpr const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool IsLaunchAtStartupEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    bool found = RegQueryValueExW(key, L"AudioLinker", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

void SetLaunchAtStartup(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        // --tray: start hidden in the notification area instead of opening
        // the window on every logon.
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --tray";
        RegSetValueExW(key, L"AudioLinker", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"AudioLinker");
    }
    RegCloseKey(key);
}

// --- Endpoint management (rename + default device via IPolicyConfig) ---

// Note: the MMDevices registry keys are writable only by the audio service,
// even elevated — renaming must go through IPolicyConfig like the sound
// control panel does.
bool RenameEndpoint(const std::wstring& deviceId, const std::wstring& newName) {
    ComPtr<IPolicyConfig> pc;
    if (FAILED(CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL,
                                 __uuidof(IPolicyConfig),
                                 reinterpret_cast<void**>(pc.GetAddressOf())))) {
        return false;
    }
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_LPWSTR;
    pv.pwszVal = const_cast<LPWSTR>(newName.c_str());
    // pv wraps a string we don't own: no PropVariantClear.
    return SUCCEEDED(pc->SetPropertyValue(deviceId.c_str(), 0, PKEY_Device_DeviceDesc, &pv));
}

// Works for render and capture endpoints alike: SetDefaultEndpoint keys on
// the endpoint id, which carries its own flow.
bool SetDefaultEndpointAllRoles(const std::wstring& deviceId) {
    ComPtr<IPolicyConfig> pc;
    if (FAILED(CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL,
                                 __uuidof(IPolicyConfig),
                                 reinterpret_cast<void**>(pc.GetAddressOf())))) {
        return false;
    }
    g_lastDefaultChangeTick = GetTickCount64();
    bool ok = SUCCEEDED(pc->SetDefaultEndpoint(deviceId.c_str(), eConsole));
    pc->SetDefaultEndpoint(deviceId.c_str(), eMultimedia);
    // Without this, apps that pick their device via the communications role
    // (Teams, Discord, Skype...) stay on the old device when the group
    // starts, unlike the Sound panel which sets all three roles.
    pc->SetDefaultEndpoint(deviceId.c_str(), eCommunications);
    return ok;
}

std::wstring GetDefaultRenderId() {
    for (auto& d : g_deviceManager->enumerateRenderDevices()) {
        if (d.isDefault) return d.id;
    }
    return L"";
}

// The default output we displaced when the group started is written to the
// config the moment it changes: a crash mid-group would otherwise leave the
// virtual cable as the default with no record of what to put back (see
// RepairAfterCrash).
void PersistPreviousDefault() {
    FlushConfig();
}

// Puts Windows' default output back on preferredId -- or, if that device
// is gone (unplugged since the group started) or refuses, on the first
// physical output still present. Returns false only when nothing could be
// restored at all, which leaves the cable as the default: a silent PC.
bool RestoreDefaultRender(const std::wstring& preferredId) {
    auto devices = g_deviceManager->enumerateRenderDevices();
    bool present = false;
    for (const auto& d : devices) {
        if (d.id == preferredId) { present = true; break; }
    }
    if (present && SetDefaultEndpointAllRoles(preferredId)) return true;
    for (const auto& d : devices) {
        if (IsVirtualDevice(d.name)) continue;
        if (SetDefaultEndpointAllRoles(d.id)) {
            LOG_WARN(L"Sortie par défaut précédente introuvable — repli sur « " + d.name + L" ».");
            return true;
        }
    }
    LOG_ERROR(L"Impossible de restaurer une sortie par défaut : aucune sortie physique disponible.");
    return false;
}

// --- Windows input level of capture endpoints (INPUTS tab) ---

ComPtr<IAudioEndpointVolume> OpenEndpointVolume(const std::wstring& deviceId) {
    auto device = g_deviceManager->getDeviceById(deviceId);
    if (!device) return {};
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(volume.GetAddressOf())))) {
        return {};
    }
    return volume;
}

// The same input level/mute the Sound settings page shows -- system-wide,
// every app capturing this mic sees the change.
bool GetCaptureVolume(const std::wstring& deviceId, int& volPercent, bool& muted) {
    auto volume = OpenEndpointVolume(deviceId);
    if (!volume) return false;
    float scalar = 1.0f;
    BOOL m = FALSE;
    if (FAILED(volume->GetMasterVolumeLevelScalar(&scalar))) return false;
    volume->GetMute(&m);
    volPercent = static_cast<int>(scalar * 100.0f + 0.5f);
    muted = m != FALSE;
    return true;
}

bool SetCaptureVolume(const std::wstring& deviceId, int volPercent) {
    auto volume = OpenEndpointVolume(deviceId);
    if (!volume) return false;
    return SUCCEEDED(volume->SetMasterVolumeLevelScalar(
        ClampVolume(volPercent) / 100.0f, nullptr));
}

bool SetCaptureMute(const std::wstring& deviceId, bool muted) {
    auto volume = OpenEndpointVolume(deviceId);
    if (!volume) return false;
    return SUCCEEDED(volume->SetMute(muted ? TRUE : FALSE, nullptr));
}

// (Re)starts engine metering over the current capture-device set. Shared by
// the micmeter web command and the hot-plug refresh (a mic plugged in while
// the INPUTS tab is open must start metering too).
void StartMicMetering() {
    std::vector<std::wstring> ids;
    for (const auto& d : g_deviceManager->enumerateCaptureDevices()) {
        if (!IsVirtualDevice(d.name)) ids.push_back(d.id);
    }
    g_micMetering = true;
    g_engine.startMicMetering(ids);
    // The meter timers normally only run with the group; metering needs
    // them too (SendPeaks carries the mic peaks).
    UpdateMeterTimers();
}

void StopMicMetering() {
    if (!g_micMetering) return;
    g_micMetering = false;
    g_engine.stopMicMetering();
    UpdateMeterTimers();
}

// --- Audio source apps (process-loopback candidates) ---

struct ProcSnapshotEntry {
    DWORD parentPid;
    std::wstring exe;
};

std::map<DWORD, ProcSnapshotEntry> SnapshotProcesses() {
    std::map<DWORD, ProcSnapshotEntry> procs;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return procs;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            procs[pe.th32ProcessID] = { pe.th32ParentProcessID, pe.szExeFile };
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return procs;
}

// Multi-process apps (Chrome, Spotify, Discord...) attribute their audio
// sessions to child processes -- often a *sibling* of the process that
// actually renders the audio. Capturing that child's tree yields silence.
// Climb to the top-most ancestor running the same executable: its tree
// contains the whole app, audio service included.
DWORD RootProcessSameExe(DWORD pid, const std::map<DWORD, ProcSnapshotEntry>& procs) {
    DWORD current = pid;
    for (int depth = 0; depth < 32; ++depth) {
        auto it = procs.find(current);
        if (it == procs.end()) break;
        auto parent = procs.find(it->second.parentPid);
        if (parent == procs.end() || it->second.parentPid == current) break;
        if (_wcsicmp(parent->second.exe.c_str(), it->second.exe.c_str()) != 0) break;
        current = it->second.parentPid;
    }
    return current;
}

// --- Per-app endpoint routing ---
// The API behind Settings > "App volume and device preferences". Undocumented
// but stable (EarTrumpet, SoundSwitch...): Windows.Media.Internal.
// AudioPolicyConfig. Used to send a captured app's own output into the
// virtual cable for the session's duration -- muting its mixer session is
// NOT an option, the process-loopback tap sits after session volume, so a
// mute silences the capture too (hence the "son pendant 0,25s" symptom).

// IInspectable base is load-bearing: this is a WinRT interface, its vtable
// carries GetIids/GetRuntimeClassName/GetTrustLevel before everything else.
// Deriving from plain IUnknown shifts every call three slots early -- which
// crashes in SetPersistedDefaultAudioEndpoint.
struct IAudioPolicyConfigFactory : public IInspectable {
    // 19 vtable slots we don't use, in their exact order.
    virtual HRESULT STDMETHODCALLTYPE p01() = 0; virtual HRESULT STDMETHODCALLTYPE p02() = 0;
    virtual HRESULT STDMETHODCALLTYPE p03() = 0; virtual HRESULT STDMETHODCALLTYPE p04() = 0;
    virtual HRESULT STDMETHODCALLTYPE p05() = 0; virtual HRESULT STDMETHODCALLTYPE p06() = 0;
    virtual HRESULT STDMETHODCALLTYPE p07() = 0; virtual HRESULT STDMETHODCALLTYPE p08() = 0;
    virtual HRESULT STDMETHODCALLTYPE p09() = 0; virtual HRESULT STDMETHODCALLTYPE p10() = 0;
    virtual HRESULT STDMETHODCALLTYPE p11() = 0; virtual HRESULT STDMETHODCALLTYPE p12() = 0;
    virtual HRESULT STDMETHODCALLTYPE p13() = 0; virtual HRESULT STDMETHODCALLTYPE p14() = 0;
    virtual HRESULT STDMETHODCALLTYPE p15() = 0; virtual HRESULT STDMETHODCALLTYPE p16() = 0;
    virtual HRESULT STDMETHODCALLTYPE p17() = 0; virtual HRESULT STDMETHODCALLTYPE p18() = 0;
    virtual HRESULT STDMETHODCALLTYPE p19() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(
        UINT processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(
        UINT processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

ComPtr<IAudioPolicyConfigFactory> GetAudioPolicyConfigFactory() {
    static const wchar_t kClass[] = L"Windows.Media.Internal.AudioPolicyConfig";
    HSTRING_HEADER header;
    HSTRING cls = nullptr;
    if (FAILED(WindowsCreateStringReference(kClass, ARRAYSIZE(kClass) - 1, &header, &cls))) {
        return nullptr;
    }
    // Interface IID changed in 21H2; same vtable layout on both.
    static const GUID kIid21H2 = { 0x2a59116d, 0x6c4f, 0x45e0,
                                   { 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58 } };
    static const GUID kIidLegacy = { 0xab3d4648, 0xe242, 0x459f,
                                     { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 } };
    ComPtr<IAudioPolicyConfigFactory> factory;
    if (FAILED(RoGetActivationFactory(cls, kIid21H2,
                                      reinterpret_cast<void**>(factory.GetAddressOf())))) {
        RoGetActivationFactory(cls, kIidLegacy,
                               reinterpret_cast<void**>(factory.GetAddressOf()));
    }
    return factory;
}

// Empty mmDeviceId = reset the app back to "Default".
bool SetAppRenderEndpoint(DWORD pid, const std::wstring& mmDeviceId) {
    auto factory = GetAudioPolicyConfigFactory();
    if (!factory) return false;
    HSTRING device = nullptr;
    std::wstring path;
    if (!mmDeviceId.empty()) {
        // SWD device-interface path form, DEVINTERFACE_AUDIO_RENDER suffix.
        path = L"\\\\?\\SWD#MMDEVAPI#" + mmDeviceId + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
        if (FAILED(WindowsCreateString(path.c_str(), static_cast<UINT32>(path.size()), &device))) {
            return false;
        }
    }
    HRESULT hr1 = factory->SetPersistedDefaultAudioEndpoint(pid, eRender, eConsole, device);
    HRESULT hr2 = factory->SetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia, device);
    if (device) WindowsDeleteString(device);
    return SUCCEEDED(hr1) && SUCCEEDED(hr2);
}

// Apps we routed into the cable for the current group session; every stop
// path must undo this or the app stays silent after AudioLinker exits.
std::vector<DWORD> g_routedPids;


void UnrouteAllApps() {
    for (DWORD pid : g_routedPids) SetAppRenderEndpoint(pid, L"");
    g_routedPids.clear();
    if (!g_cfg.routedApps.empty()) {
        g_cfg.routedApps.clear();
        FlushConfig();
    }
}

// Undoes everything a group start did to Windows: captured apps back on
// their own output, the displaced default output restored (with a fallback
// if it vanished). Every stop path -- clean stop, failed start, logoff,
// exit -- ends here, so none of them can forget a step.
void RestoreDefaultAndUnroute() {
    UnrouteAllApps();
    if (g_previousDefaultId.empty()) return;
    RestoreDefaultRender(g_previousDefaultId);
    g_previousDefaultId.clear();
    PersistPreviousDefault();
}

// Apps currently holding a render session on any output device -- the
// candidates for direct per-application capture. Keyed by their root
// process, so Chrome's dozen per-tab sessions collapse into one entry.
std::vector<SourceApp> EnumerateSourceApps() {
    std::vector<SourceApp> result;
    DWORD selfPid = GetCurrentProcessId();
    auto procs = SnapshotProcesses();
    for (const auto& d : g_deviceManager->enumerateRenderDevices()) {
        auto device = g_deviceManager->getDeviceById(d.id);
        if (!device) continue;
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(manager.GetAddressOf())))) continue;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(sessions.GetAddressOf()))) continue;
        int count = 0;
        sessions->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> ctrl;
            if (FAILED(sessions->GetSession(i, ctrl.GetAddressOf()))) continue;
            AudioSessionState state = AudioSessionStateInactive;
            ctrl->GetState(&state);
            if (state == AudioSessionStateExpired) continue;
            ComPtr<IAudioSessionControl2> ctrl2;
            if (FAILED(ctrl.As(&ctrl2))) continue;
            if (ctrl2->IsSystemSoundsSession() == S_OK) continue;
            DWORD sessionPid = 0;
            if (FAILED(ctrl2->GetProcessId(&sessionPid)) || sessionPid == 0 || sessionPid == selfPid) continue;
            DWORD pid = RootProcessSameExe(sessionPid, procs);
            bool known = false;
            for (const auto& s : result) {
                if (s.pid == pid) known = true;
            }
            if (known) continue;

            std::wstring name;
            auto pit = procs.find(pid);
            if (pit != procs.end()) name = pit->second.exe;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, L".exe") == 0) {
                name.resize(name.size() - 4);
            }
            if (name.empty()) name = L"PID " + std::to_wstring(pid);
            result.push_back({ pid, name });
        }
    }
    std::sort(result.begin(), result.end(),
              [](const SourceApp& a, const SourceApp& b) { return a.name < b.name; });
    return result;
}

// Remembers which executables are currently routed into the cable, so a
// crash (no clean stop path) can be repaired at the next launch.
void SaveRoutedAppsToIni() {
    auto procs = SnapshotProcesses();
    g_cfg.routedApps.clear();
    for (DWORD pid : g_routedPids) {
        auto it = procs.find(pid);
        if (it != procs.end()) g_cfg.routedApps.push_back(it->second.exe);
    }
    FlushConfig(); // crash repair relies on this being on disk right away
}

// Startup repair after a previous run died mid-group (crash, taskkill):
// - the default output it had displaced is put back if the cable is still
//   the default (see PersistPreviousDefault);
// - every still-running instance of the executables it had routed into the
//   cable goes back on the default device. (An app not running keeps its
//   stale route until it can be resolved to a PID again -- unavoidable with
//   this API.)
void RepairAfterCrash() {
    bool dirty = false;
    if (!g_cfg.previousDefault.empty()) {
        std::wstring current = GetDefaultRenderId();
        bool onCable = false;
        for (const auto& d : g_deviceManager->enumerateRenderDevices()) {
            if (d.id == current && IsCableDevice(d.name)) onCable = true;
        }
        if (onCable) {
            LOG_WARN(L"Arrêt brutal détecté lors de la session précédente : restauration de la sortie par défaut.");
            RestoreDefaultRender(g_cfg.previousDefault);
        }
        g_cfg.previousDefault.clear();
        dirty = true;
    }

    if (!g_cfg.routedApps.empty()) {
        auto procs = SnapshotProcesses();
        std::vector<DWORD> roots;
        for (const auto& exe : g_cfg.routedApps) {
            for (const auto& [pid, info] : procs) {
                if (_wcsicmp(info.exe.c_str(), exe.c_str()) != 0) continue;
                DWORD root = RootProcessSameExe(pid, procs);
                if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
                    roots.push_back(root);
                }
            }
        }
        for (DWORD root : roots) SetAppRenderEndpoint(root, L"");
        g_cfg.routedApps.clear();
        dirty = true;
    }
    if (dirty) FlushConfig();
}

} // namespace al
