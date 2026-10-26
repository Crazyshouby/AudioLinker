// The bridge to the WebView2 pages: outgoing JSON messages (state, status,
// sources, calibration, toasts) and the dispatcher of incoming commands.

#include "App.h"

namespace al {

// --- JSON bridge to the web UI ---
// Outgoing messages are built as nlohmann::json and serialized once --
// escaping, number formatting (locale-independent) and nesting are no
// longer hand-rolled. Incoming commands stay the compact "cmd|arg|arg"
// strings (see HandleWebMessage): endpoint ids never contain '|' and any
// free-text argument is always last, so no escaping is ever needed there.

std::string U8(const std::wstring& w) { return ToUtf8(w); }

void PostJsonText(const std::wstring& text) {
    // Never poke the main WebView while its window is hidden: it may be
    // suspended (see SuspendMainWebView) and a message would wake the whole
    // page just to update something nobody can see. RestoreFromTray re-sends
    // the full state on the way back.
    if (g_webview && g_mainWnd && IsWindowVisible(g_mainWnd)) {
        g_webview->PostWebMessageAsJson(text.c_str());
    }
    if (g_optionsWebview) g_optionsWebview->PostWebMessageAsJson(text.c_str());
}

void PostJson(const json& j) { PostJsonText(FromUtf8(j.dump())); }

const char* DeviceKindJson(AudioDeviceKind kind) {
    switch (kind) {
        case AudioDeviceKind::Headphones: return "headphones";
        case AudioDeviceKind::Hdmi:        return "hdmi";
        case AudioDeviceKind::Digital:     return "digital";
        case AudioDeviceKind::Bluetooth:   return "bluetooth";
        default:                           return "speaker";
    }
}

void SendState() {
    COLORREF accent = GetWindowsAccentColor();
    char accentHex[8];
    snprintf(accentHex, sizeof(accentHex), "#%02x%02x%02x", GetRValue(accent), GetGValue(accent), GetBValue(accent));

    json j;
    j["type"] = "state";
    j["running"] = g_engine.isRunning();
    j["starting"] = g_engine.isStarting();
    j["version"] = AL_VERSION_STR;
    j["accent"] = accentHex;
    j["autostart"] = g_autostart;
    j["launchAtStartup"] = IsLaunchAtStartupEnabled();
    j["masterVolume"] = g_masterVolumePercent;
    j["cushionMs"] = g_baseCushionMs;
    j["cushionMsMin"] = kMinBaseCushionMs;
    j["cushionMsMax"] = kMaxBaseCushionMs;
    j["deviceFillMs"] = g_deviceFillMs;
    j["deviceFillMsMin"] = kMinDeviceFillMs;
    j["deviceFillMsMax"] = kMaxDeviceFillMs;
    j["lowLatency"] = g_lowLatency;
    j["setupDigitHotkeys"] = g_setupDigitHotkeys;
    j["optimizing"] = g_optimizing;
    j["theme"] = g_themeOverride;
    j["toggleHotkey"] = U8(FormatHotkey(g_toggleHotkeyMods, g_toggleHotkeyVk));
    j["switcherHotkey"] = U8(FormatHotkey(g_switcherHotkeyMods, g_switcherHotkeyVk));
    j["capturingHotkey"] = g_hotkeyCaptureId == HOTKEY_TOGGLE_GROUP ? "toggle"
                         : g_hotkeyCaptureId == HOTKEY_SETUP_SWITCHER ? "switcher" : "";

    j["devices"] = json::array();
    for (const auto& d : g_outputDevices) {
        const auto& s = g_settings[d.id];
        json jd;
        jd["id"] = U8(d.id);
        jd["name"] = U8(d.name);
        jd["kind"] = DeviceKindJson(d.kind);
        jd["alias"] = U8(s.alias);
        jd["color"] = U8(s.colorHex);
        jd["checked"] = s.checked;
        jd["channel"] = s.channelMode;
        jd["volume"] = s.volumePercent;
        jd["latency"] = s.latencyMs;
        jd["eq"] = s.eqDb;
        jd["muted"] = s.muted;
        jd["sourcePid"] = s.sourcePid;
        j["devices"].push_back(std::move(jd));
    }

    j["setups"] = json::array();
    for (const auto& s : g_setups) {
        size_t checkedCount = 0;
        for (const auto& [id, os] : s.devices) {
            if (os.checked) ++checkedCount;
        }
        j["setups"].push_back({ { "name", U8(s.name) }, { "count", checkedCount } });
    }
    j["activeSetup"] = U8(g_activeSetup);
    j["editingSetup"] = U8(g_editingSetup);
    j["playingSetup"] = U8(g_playingSetup);
    j["setupMic"] = U8(g_setupMicId);

    // Automation rules keyed by device id (see AutomationRule).
    j["automation"] = json::object();
    for (const auto& r : g_cfg.automation) {
        j["automation"][U8(r.deviceId)] = { { "onConnect", U8(r.onConnect) },
                                            { "onDisconnect", U8(r.onDisconnect) } };
    }

    // Physical capture devices, for the INPUTS tab and the mic-calibration
    // modal's input picker (virtual endpoints filtered out: a cable "mic"
    // can't hear a speaker, and shouldn't become anyone's default mic).
    // Their Windows level/mute is only read while the INPUTS tab is showing
    // (g_micMetering): one COM activation per mic on every state resend is
    // not worth it for a tab nobody is looking at.
    j["inputs"] = json::array();
    for (const auto& d : g_deviceManager->enumerateCaptureDevices()) {
        if (IsVirtualDevice(d.name)) continue;
        int micVol = 100;
        bool micMuted = false;
        if (g_micMetering) GetCaptureVolume(d.id, micVol, micMuted);
        j["inputs"].push_back({ { "id", U8(d.id) }, { "name", U8(d.name) }, { "default", d.isDefault },
                                { "volume", micVol }, { "muted", micMuted } });
    }
    PostJson(j);
}

// Apps currently playing audio, for the speaker sheet's Source dropdown.
// Deliberately not part of SendState: enumerating the audio sessions of
// every device plus a process snapshot is too heavy to pay on every
// checkbox click, so the page asks for it when it needs it ("sources").
void SendSources() {
    json j;
    j["type"] = "sources";
    j["sources"] = json::array();
    for (const auto& s : EnumerateSourceApps()) {
        j["sources"].push_back({ { "pid", s.pid }, { "name", U8(s.name) } });
    }
    PostJson(j);
}

// Meters are sent rounded to 3 decimals: enough for a VU, a third of the
// bytes of a full double.
double Milli(double v) { return std::round(v * 1000.0) / 1000.0; }

void SendStatus() {
    auto status = g_engine.getStatus();
    // takePeaks() is a destructive read (meters reset to 0), so it is called
    // exactly once per timer tick, here -- every other reader (optimizer,
    // alignment) only uses the non-destructive getStatus().
    auto peaks = g_engine.takePeaks();

    // A captured app that died no longer takes the engine down (its outputs
    // just play silence), so the group's stop path won't unroute it anytime
    // soon -- put it back on its normal endpoint now, or relaunching the app
    // would land its audio in the virtual cable with nobody capturing that
    // old PID anymore. Only once the process is really gone: a transient
    // capture loss (source retried while the app lives) must keep the route.
    for (const auto& st : status) {
        if (!st.sourceLost || st.sourcePid == 0) continue;
        auto it = std::find(g_routedPids.begin(), g_routedPids.end(),
                            static_cast<DWORD>(st.sourcePid));
        if (it == g_routedPids.end()) continue;
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, *it);
        if (h) {
            bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
            CloseHandle(h);
            if (alive) continue;
        }
        SetAppRenderEndpoint(*it, L"");
        g_routedPids.erase(it);
        SaveRoutedAppsToIni();
    }

    json j;
    j["type"] = "status";
    j["running"] = g_engine.isRunning();
    j["starting"] = g_engine.isStarting();
    j["outputs"] = json::array();
    for (const auto& st : status) {
        const OutputPeaks* pk = nullptr;
        for (const auto& p : peaks) {
            if (p.deviceId == st.deviceId) { pk = &p; break; }
        }
        json jo;
        jo["id"] = U8(st.deviceId);
        jo["active"] = st.active;
        jo["sourceLost"] = st.sourceLost;
        jo["peak"] = Milli(pk ? pk->peak : 0.0);
        jo["underruns"] = st.underruns;
        jo["drift"] = st.driftCorrections;
        jo["lagMs"] = st.deviceLagMs;
        jo["bands"] = json::array();
        for (int b = 0; b < kSpectrumBandCount; ++b) {
            jo["bands"].push_back(Milli(pk ? pk->spectrumBands[b] : 0.0));
        }
        j["outputs"].push_back(std::move(jo));
    }
    // Per-mic peaks for the INPUTS tab's VU meters -- empty unless metering
    // is on (tab visible).
    j["micPeaks"] = json::array();
    for (const auto& [id, peak] : g_engine.getMicPeaks()) {
        j["micPeaks"].push_back({ { "id", U8(id) }, { "peak", Milli(peak) } });
    }
    PostJson(j);
}

// Progress of the mic calibration pass, polled by IDT_CAL_TIMER -- the
// modal's per-output rows render straight from this.
void SendCalStatus() {
    auto snap = g_calibrator.snapshot();
    json j;
    j["type"] = "cal";
    j["running"] = snap.running;
    j["cancelled"] = snap.cancelled;
    j["error"] = U8(snap.error);
    j["outputs"] = json::array();
    for (const auto& o : snap.outcomes) {
        const char* st = o.state == CalState::Pending   ? "pending"
                       : o.state == CalState::Measuring ? "measuring"
                       : o.state == CalState::Done      ? "done"
                                                        : "failed";
        j["outputs"].push_back({ { "id", U8(o.deviceId) }, { "state", st }, { "delay", o.delayMs } });
    }
    PostJson(j);
}

// Toasts posted while the main window is hidden never reach the (suspended)
// page: keep them for the next RestoreFromTray, and surface errors through
// a tray balloon right away so a failed autostart at logon isn't just lost.
void RememberHiddenToast(const json& j, const std::wstring& balloonText) {
    if (!g_mainWnd || IsWindowVisible(g_mainWnd)) return;
    if (g_pendingJson.size() >= 10) g_pendingJson.erase(g_pendingJson.begin());
    g_pendingJson.push_back(FromUtf8(j.dump()));
    if (!balloonText.empty()) ShowTrayBalloon(balloonText, NIIF_WARNING);
}

void SendError(const std::wstring& message) {
    json j = { { "type", "error" }, { "message", U8(message) } };
    PostJson(j);
    RememberHiddenToast(j, message);
}

void SendInfo(const std::wstring& message) {
    json j = { { "type", "info" }, { "message", U8(message) } };
    PostJson(j);
    RememberHiddenToast(j, L"");
}

// An error toast carrying one action button, which posts `command` back.
void SendErrorWithAction(const std::wstring& message, const std::wstring& command,
                         const std::wstring& label) {
    json j = { { "type", "error" }, { "message", U8(message) },
               { "action", U8(command) }, { "actionLabel", U8(label) } };
    PostJson(j);
    RememberHiddenToast(j, message);
}


// --- Messages coming from the web UI ---

void HandleWebMessage(HWND hwnd, const std::wstring& msg) {
    size_t p1 = msg.find(L'|');
    std::wstring cmd = msg.substr(0, p1);
    // rest = everything after the first '|' (may itself contain '|' only for
    // groupname, which is why the free-text argument is always last).
    std::wstring rest = (p1 == std::wstring::npos) ? L"" : msg.substr(p1 + 1);

    if (cmd == L"ready") {
        SendState();
        if (!g_startupWarning.empty()) {
            SendError(g_startupWarning);
            g_startupWarning.clear();
        }
        if (g_autostartPending) {
            g_autostartPending = false;
            if (!g_engine.isRunning() && !g_engine.isStarting()) OnCreateGroup(hwnd);
        }
        // --tray launch: the page was loaded into a window that has never
        // been shown and may never be this session -- don't keep a full
        // Chromium render tree warm for it. (No-op when the window is
        // visible, e.g. this ready came from the Options page.)
        if (g_mainWnd && !IsWindowVisible(g_mainWnd)) SuspendMainWebView();
        return;
    }
    if (cmd == L"toggle") { ToggleGroup(hwnd); return; }
    if (cmd == L"calstart") {
        // calstart|micId -- mic-based automatic latency alignment.
        BeginMicCalibration(hwnd, rest);
        return;
    }
    if (cmd == L"automation") {
        // automation|deviceId|onConnect|onDisconnect -- "when this speaker
        // connects, start setup X / when it disconnects, stop". Both empty
        // = no rule for this device.
        auto parts = SplitPipes(rest);
        if (parts.size() < 3 || parts[0].empty()) return;
        auto& rules = g_cfg.automation;
        rules.erase(std::remove_if(rules.begin(), rules.end(),
                                   [&](const AutomationRule& r) { return r.deviceId == parts[0]; }),
                    rules.end());
        if (!parts[1].empty() || !parts[2].empty()) {
            rules.push_back({ parts[0], parts[1], parts[2] });
        }
        SaveConfig();
        SendState();
        return;
    }
    if (cmd == L"calcancel") {
        g_calibrator.cancel(); // the pass winds down; IDT_CAL_TIMER finalizes
        return;
    }
    if (cmd == L"refresh") {
        // Safe even while playing: only the GUI-side list is rebuilt, the
        // engine keeps its own copies of everything.
        PopulateOutputList();
        SendState();
        SendSources();
        return;
    }
    if (cmd == L"sources") { SendSources(); return; }
    if (cmd == L"reclaim") {
        // The user answered the "audio left the group" toast: put the cable
        // back as the default output. Only meaningful while a group holds it.
        if (g_previousDefaultId.empty()) return;
        for (const auto& d : g_deviceManager->enumerateRenderDevices()) {
            if (!IsCableDevice(d.name)) continue;
            if (SetDefaultEndpointAllRoles(d.id)) SendInfo(L"Le groupe a repris la sortie par défaut.");
            else SendError(L"Impossible de redéfinir la sortie par défaut.");
            return;
        }
        return;
    }
    if (cmd == L"openoptions") { OpenOptionsWindow(g_mainWnd); return; }
    if (cmd == L"exportconfig") { ExportConfig(hwnd); return; }
    if (cmd == L"importconfig") { ImportConfig(hwnd); return; }
    if (cmd == L"autostart") { g_autostart = (rest == L"1"); SaveConfig(); return; }
    if (cmd == L"launchwindows") { SetLaunchAtStartup(rest == L"1"); return; }
    if (cmd == L"mastervolume") {
        g_masterVolumePercent = ClampVolume(_wtoi(rest.c_str()));
        if (g_engine.isRunning()) {
            for (const auto& d : g_outputDevices) {
                g_engine.setVolumePercent(d.id, EffectiveVolume(g_settings[d.id]));
            }
        }
        return;
    }
    if (cmd == L"mastervolumecommit") { SaveConfig(); return; }
    if (cmd == L"cushion") {
        g_baseCushionMs = ClampCushionMs(_wtoi(rest.c_str()));
        g_engine.setBaseCushionMs(g_baseCushionMs);
        return;
    }
    if (cmd == L"cushioncommit") {
        // Snap every output to the new cushion target within ~1s instead of
        // drifting there at the slow inaudible-correction rate.
        if (g_engine.isRunning()) g_engine.resyncAllOutputs();
        SaveConfig();
        return;
    }
    if (cmd == L"devicefill") {
        g_deviceFillMs = ClampDeviceFillMs(_wtoi(rest.c_str()));
        g_engine.setDeviceFillMs(g_deviceFillMs);
        return;
    }
    if (cmd == L"devicefillcommit") {
        // The fill change shifts audio between the device buffer and the
        // ring; the resync re-targets the ring immediately instead of
        // letting the ±0.15% correction chew on it for minutes.
        if (g_engine.isRunning()) g_engine.resyncAllOutputs();
        SaveConfig();
        return;
    }
    if (cmd == L"optimize") {
        if (rest == L"start") StartOptimizer(hwnd);
        else if (rest == L"cancel") StopOptimizer(hwnd, L"Optimisation annulée.");
        return;
    }
    if (cmd == L"openlog") {
        // Opens the diagnostic log in the user's default text viewer.
        wchar_t buf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) > 0) {
            std::wstring path = std::wstring(buf) + L"\\AudioLinker\\audiolinker.log";
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    if (cmd == L"lowlatency") {
        g_lowLatency = (rest == L"1");
        g_engine.setLowLatency(g_lowLatency);
        SaveConfig();
        // Only takes effect on the next group start (the period is fixed at
        // stream init) -- restart a running group so the change is live now.
        if (g_engine.isRunning()) {
            OnStop(hwnd);
            OnCreateGroup(hwnd);
        }
        SendState();
        return;
    }
    if (cmd == L"theme") {
        g_themeOverride = ClampThemeOverride(_wtoi(rest.c_str()));
        ApplyWindowTheme(g_mainWnd);
        ApplyOptionsWindowTheme();
        SaveConfig();
        SendState();
        return;
    }
    if (cmd == L"resetoptions") {
        // Scoped to exactly what the Options window itself edits -- not
        // setups, per-speaker EQ/aliases, or anything else, which would be a
        // much more destructive action than "reset these settings".
        g_autostart = false;
        SetLaunchAtStartup(false);
        g_baseCushionMs = kDefaultBaseCushionMs;
        g_engine.setBaseCushionMs(g_baseCushionMs);
        g_deviceFillMs = kDefaultDeviceFillMs;
        g_engine.setDeviceFillMs(g_deviceFillMs);
        g_lowLatency = false;
        g_engine.setLowLatency(false);
        if (g_engine.isRunning()) g_engine.resyncAllOutputs();
        g_setupDigitHotkeys = true;
        UpdateSetupDigitHotkeys(g_mainWnd);
        g_themeOverride = 0;
        ApplyWindowTheme(g_mainWnd);
        ApplyOptionsWindowTheme();
        RebindHotkey(g_mainWnd, HOTKEY_TOGGLE_GROUP, g_toggleHotkeyMods, g_toggleHotkeyVk,
                     MOD_CONTROL | MOD_ALT, 'G');
        RebindHotkey(g_mainWnd, HOTKEY_SETUP_SWITCHER, g_switcherHotkeyMods, g_switcherHotkeyVk,
                     MOD_CONTROL | MOD_ALT, 'S');
        SaveConfig();
        SendState();
        SendInfo(L"Paramètres réinitialisés.");
        return;
    }
    if (cmd == L"capturehotkey") {
        // capturehotkey|toggle  or  capturehotkey|switcher -- arms the
        // native low-level keyboard hook (see HotkeyCaptureHookProc) that
        // waits for the next combo pressed anywhere on the system.
        if (rest == L"toggle") StartHotkeyCapture(HOTKEY_TOGGLE_GROUP);
        else if (rest == L"switcher") StartHotkeyCapture(HOTKEY_SETUP_SWITCHER);
        else return;
        SendState();
        return;
    }
    if (cmd == L"cancelhotkeycapture") {
        StopHotkeyCapture();
        SendState();
        return;
    }
    if (cmd == L"setupdigits") {
        g_setupDigitHotkeys = (rest == L"1");
        UpdateSetupDigitHotkeys(g_mainWnd);
        SaveConfig();
        SendState();
        return;
    }
    if (cmd == L"devsource") {
        // devsource|deviceId|pid -- per-speaker source picked from the EQ
        // panel's Source dropdown. Cosmetic-level state (like alias): it
        // does not invalidate the active setup.
        if (g_engine.isStarting()) {
            SendError(L"Patientez : le groupe est en cours de démarrage.");
            SendState();
            return;
        }
        size_t p = rest.find(L'|');
        std::wstring deviceId = rest.substr(0, p);
        DWORD pid = (p == std::wstring::npos)
                        ? 0 : static_cast<DWORD>(_wtoi(rest.substr(p + 1).c_str()));
        auto it = g_settings.find(deviceId);
        if (it == g_settings.end() || it->second.sourcePid == pid) return;
        bool wasRunning = g_engine.isRunning();
        if (wasRunning) OnStop(hwnd);
        it->second.sourcePid = pid;
        SendState();
        if (wasRunning) OnCreateGroup(hwnd); // resume with the new routing
        return;
    }
    if (cmd == L"alias") {
        // Cosmetic: does not invalidate the active setup, hence handled
        // before the generic per-device block below.
        size_t p = rest.find(L'|');
        std::wstring deviceId = rest.substr(0, p);
        std::wstring alias = (p == std::wstring::npos) ? L"" : rest.substr(p + 1);
        auto it = g_settings.find(deviceId);
        if (it != g_settings.end()) {
            if (alias.size() > 60) alias = alias.substr(0, 60);
            it->second.alias = alias;
            SaveConfig();
            SendState(); // name shows in several places (list, settings title)
        }
        return;
    }
    if (cmd == L"devcolor") {
        // devcolor|deviceId|hex (hex empty = reset to the default kind-based
        // color). Cosmetic, like alias: doesn't invalidate the active setup.
        size_t p = rest.find(L'|');
        std::wstring deviceId = rest.substr(0, p);
        std::wstring color = (p == std::wstring::npos) ? L"" : rest.substr(p + 1);
        auto it = g_settings.find(deviceId);
        if (it != g_settings.end()) {
            it->second.colorHex = color;
            SaveConfig();
            SendState();
        }
        return;
    }
    if (cmd == L"setupmic") {
        // setupmic|captureId -- picks the mic tied to the current config
        // (INPUTS tab). Applied to Windows immediately; re-picking the
        // selected mic clears the choice (Windows default left as-is from
        // then on). Setup-level state, so the active preset diverges.
        if (rest == g_setupMicId) {
            g_setupMicId.clear();
        } else {
            g_setupMicId = rest;
            if (!SetDefaultEndpointAllRoles(rest)) {
                SendError(L"Impossible de définir ce micro comme périphérique par défaut.");
            }
        }
        g_activeSetup.clear();
        SaveConfig();
        SendState();
        return;
    }
    if (cmd == L"micvolume") {
        // micvolume|captureId|percent -- live Windows input level while the
        // slider drags. No SaveConfig/SendState here: a state resend would
        // rebuild the list mid-drag, and the value lives in Windows anyway.
        size_t p = rest.find(L'|');
        if (p == std::wstring::npos) return;
        SetCaptureVolume(rest.substr(0, p), _wtoi(rest.substr(p + 1).c_str()));
        return;
    }
    if (cmd == L"micvolcommit") {
        // End of the slider gesture: the mic level is setup-level state
        // (snapshotted on save), so the active preset diverges now.
        g_activeSetup.clear();
        SaveConfig();
        SendState();
        return;
    }
    if (cmd == L"micmute") {
        // micmute|captureId|0/1 -- Windows-level mic mute, system-wide.
        size_t p = rest.find(L'|');
        if (p == std::wstring::npos) return;
        SetCaptureMute(rest.substr(0, p), rest.substr(p + 1) == L"1");
        g_activeSetup.clear();
        SaveConfig();
        SendState();
        return;
    }
    if (cmd == L"micmeter") {
        // micmeter|0/1 -- VU metering for the INPUTS tab, on only while the
        // tab is actually visible: it holds open capture streams, which
        // lights the Windows "microphone in use" indicator.
        if (rest == L"1") {
            StartMicMetering();
            SendState(); // the inputs' Windows level/mute are only read while metering
        } else {
            StopMicMetering();
        }
        return;
    }
    if (cmd == L"identify") {
        // identify|deviceId -- plays a couple of short beeps directly to
        // that one device, independent of the group, so the user can tell
        // which list entry is which physical speaker.
        g_engine.identifyOutput(rest);
        return;
    }
    if (cmd == L"spectrum") {
        // spectrum|deviceId (empty = none) -- which output, if any, has its
        // EQ panel on screen and therefore needs the 10-band spectrum
        // computed (see AudioEngine::setSpectrumTarget).
        g_engine.setSpectrumTarget(rest);
        return;
    }
    if (cmd == L"eq") {
        // eq|deviceId|band|value -- 3 arguments, so handled here rather than
        // in the generic cmd|deviceId|value block below.
        size_t p = rest.find(L'|');
        std::wstring deviceId = rest.substr(0, p);
        std::wstring restB = (p == std::wstring::npos) ? L"" : rest.substr(p + 1);
        size_t p2 = restB.find(L'|');
        int band = (p2 == std::wstring::npos) ? -1 : _wtoi(restB.substr(0, p2).c_str());
        int v = (p2 == std::wstring::npos) ? 0 : _wtoi(restB.substr(p2 + 1).c_str());
        auto it = g_settings.find(deviceId);
        if (it == g_settings.end() || band < 0 || band >= kEqBandCount) return;
        g_activeSetup.clear();
        it->second.eqDb[band] = ClampDb(v);
        if (g_engine.isRunning()) g_engine.setEqBand(deviceId, band, it->second.eqDb[band]);
        return;
    }
    if (cmd == L"setup_save") { SaveSetupFromCurrent(rest); return; }
    if (cmd == L"setup_update") {
        if (!g_editingSetup.empty()) SaveSetupFromCurrent(g_editingSetup);
        return;
    }
    if (cmd == L"setup_play") {
        // Toggle: if this setup is already the one live and playing, stop;
        // otherwise load it and make sure the group actually starts (unlike
        // the hotkey/tray/switcher paths, which only switch config).
        if (g_playingSetup == rest && g_engine.isRunning()) {
            OnStop(hwnd);
        } else {
            ApplySetup(hwnd, rest, /*ensureRunning=*/true);
        }
        return;
    }
    if (cmd == L"setup_delete") { DeleteSetup(rest); return; }
    if (cmd == L"setup_select") {
        // setup_select|name -- clicking a setup row only loads it into the
        // live device list for viewing/editing; it never starts or
        // interrupts playback (unlike the play button or ApplySetup()).
        SelectSetup(rest);
        return;
    }
    if (cmd == L"reorder") {
        // rest = deviceId|deviceId|... in the new visual order. Purely
        // cosmetic (playback is unaffected), so no state resend and no
        // active-setup invalidation.
        auto ids = SplitPipes(rest);
        for (size_t i = 0; i < ids.size(); ++i) {
            auto it = g_settings.find(ids[i]);
            if (it != g_settings.end()) it->second.order = static_cast<int>(i);
        }
        SortOutputDevicesByOrder();
        SaveConfig();
        return;
    }

    // Remaining commands: cmd|deviceId or cmd|deviceId|value. Device ids
    // (MMDevice endpoint ids) never contain '|'.
    if (cmd != L"check" && cmd != L"mute" && cmd != L"channel" && cmd != L"volume" &&
        cmd != L"latency" && cmd != L"commit") {
        return; // unknown command: must not touch anything
    }
    size_t p2 = rest.find(L'|');
    std::wstring deviceId = rest.substr(0, p2);
    std::wstring value = (p2 == std::wstring::npos) ? L"" : rest.substr(p2 + 1);
    auto it = g_settings.find(deviceId);
    if (it == g_settings.end()) return;
    OutputSettings& s = it->second;
    int v = value.empty() ? 0 : _wtoi(value.c_str());

    // Any per-device tweak diverges from the applied preset; drop the
    // highlight (the web UI clears its own copy locally at the same time).
    g_activeSetup.clear();

    if (cmd == L"check") {
        s.checked = (v != 0);
        SaveConfig();
    } else if (cmd == L"mute") {
        s.muted = (v != 0);
        if (g_engine.isRunning()) g_engine.setMuted(deviceId, s.muted);
        SaveConfig();
    } else if (cmd == L"channel") {
        s.channelMode = ClampChannelMode(v);
        if (g_engine.isRunning()) g_engine.setChannelMode(deviceId, s.channelMode);
        SaveConfig();
    } else if (cmd == L"volume") {
        s.volumePercent = ClampVolume(v);
        if (g_engine.isRunning()) g_engine.setVolumePercent(deviceId, EffectiveVolume(s));
    } else if (cmd == L"latency") {
        s.latencyMs = ClampLatencyMs(v);
        if (g_engine.isRunning()) g_engine.setLatencyMs(deviceId, s.latencyMs);
    } else if (cmd == L"commit") {
        // End of a slider drag: the live engine values are already applied,
        // this persists them. The resync snaps the ring to a possibly-new
        // latency target within ~1s (no-op if the target didn't move).
        if (g_engine.isRunning()) g_engine.resyncOutput(deviceId);
        SaveConfig();
    }
}

} // namespace al
