// Group lifecycle (start/stop), setups (save/apply/select/delete), config
// save scheduling and export/import.

#include "App.h"

namespace al {

// --- Config persistence (JSON in %APPDATA%\AudioLinker, see Config.h) ---

// Writes g_cfg now. Reserved for the few changes that must survive a crash
// right away (the displaced default output, routed apps) and for exit.
void FlushConfig() {
    if (g_mainWnd) KillTimer(g_mainWnd, IDT_SAVE);
    g_store.save(g_cfg);
}

// Schedules a write shortly: every setting change calls this, and a burst
// of them (slider drag, setup apply) still costs one file write.
void SaveConfig() {
    if (g_mainWnd) SetTimer(g_mainWnd, IDT_SAVE, kSaveDelayMs, nullptr);
    else FlushConfig();
}

// The volume actually sent to the engine: the speaker's own slider scaled by
// the group's master volume. g_settings keeps s.volumePercent as the
// unscaled base value, so the per-speaker balance survives moving the master
// slider (and is what gets persisted/shown on that speaker's own slider).
void UpdateMeterTimers() {
    if (!g_mainWnd) return;
    if (g_statusTimerForGroup || g_micMetering) {
        SetTimer(g_mainWnd, IDT_STATUS_TIMER, kStatusIntervalMs, nullptr);
        SetTimer(g_mainWnd, IDT_VU_TIMER, kVuIntervalMs, nullptr);
    } else {
        KillTimer(g_mainWnd, IDT_STATUS_TIMER);
        KillTimer(g_mainWnd, IDT_VU_TIMER);
    }
}

bool EffectiveMuted(const OutputSettings& s) { return s.muted || g_groupMuted; }

int EffectiveVolume(const OutputSettings& s) {
    return ClampVolume((s.volumePercent * g_masterVolumePercent + 50) / 100);
}


// --- Deferred actions after an asynchronous stop ---

AfterStop g_afterStop = AfterStop::None;
std::wstring g_afterStopArg;

void QueueAfterStop(HWND hwnd, AfterStop action, const std::wstring& arg) {
    g_afterStop = action;
    g_afterStopArg = arg;
    SetTimer(hwnd, IDT_STOP_POLL, 50, nullptr);
}

void RunAfterStop(HWND hwnd) {
    AfterStop action = g_afterStop;
    std::wstring arg = g_afterStopArg;
    g_afterStop = AfterStop::None;
    g_afterStopArg.clear();
    switch (action) {
        case AfterStop::Start: OnCreateGroup(hwnd); break;
        case AfterStop::Calibrate: BeginMicCalibration(hwnd, arg); break;
        case AfterStop::None: break;
    }
}

// --- Group start/stop ---

void OnCreateGroup(HWND hwnd) {
    // The sweep must stay the only thing playing while a mic calibration
    // pass runs -- and it will restart the group itself if it stopped one.
    if (g_calibrating) {
        SendError(L"Patientez : calage micro en cours.");
        return;
    }
    // The previous group is still winding down on its own thread (see
    // AudioEngine::stopAsync): start as soon as it is gone. Lets every
    // "stop then start" caller stay a plain OnStop(); OnCreateGroup();.
    if (g_engine.isStopping()) {
        QueueAfterStop(hwnd, AfterStop::Start);
        return;
    }
    // The Windows-visible endpoint name follows the active setup: setups ARE
    // the groups -- a separate group name was one concept too many.
    std::wstring groupName = g_activeSetup.empty() ? L"Groupe d'enceintes" : g_activeSetup;

    std::vector<OutputLinkConfig> outputs;
    std::vector<DWORD> appPids; // distinct app sources across the group
    bool needSystemSource = false;
    bool anySourceReset = false;
    for (const auto& d : g_outputDevices) {
        auto& s = g_settings[d.id];
        if (!s.checked) continue;
        if (s.sourcePid != 0) {
            // The chosen app may have exited since it was picked; fall back
            // to the system source rather than failing the whole group.
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, s.sourcePid);
            if (proc) {
                CloseHandle(proc);
                if (std::find(appPids.begin(), appPids.end(), s.sourcePid) == appPids.end()) {
                    appPids.push_back(s.sourcePid);
                }
            } else {
                s.sourcePid = 0;
                anySourceReset = true;
            }
        }
        if (s.sourcePid == 0) needSystemSource = true;
        OutputLinkConfig cfg;
        cfg.deviceId = d.id;
        cfg.latencyMs = s.latencyMs;
        cfg.channelMode = s.channelMode;
        cfg.volumePercent = EffectiveVolume(s);
        cfg.eqDb = s.eqDb;
        cfg.muted = EffectiveMuted(s);
        cfg.sourcePid = s.sourcePid;
        outputs.push_back(cfg);
    }
    if (outputs.empty()) {
        SendError(L"Cochez d'abord les enceintes à inclure dans le groupe.");
        return;
    }
    if (anySourceReset) {
        SendError(L"Une application source fermée a été remplacée par « Tout le système ».");
        SendState();
    }

    // The cable is always needed: sink for the system-wide capture and/or
    // silent sink the captured apps are routed into (see SetAppRenderEndpoint
    // -- their audio must land somewhere inaudible, and muting is not an
    // option since the loopback tap sits after session volume).
    const AudioDeviceInfo* cableRender = nullptr;
    auto renderDevices = g_deviceManager->enumerateRenderDevices();
    for (auto& d : renderDevices) {
        if (IsCableDevice(d.name)) { cableRender = &d; break; }
    }
    if (!cableRender) {
        SendError(L"Le câble audio virtuel (VB-CABLE) est introuvable. "
                  L"Installez-le, ou redémarrez le PC si vous venez de l'installer.");
        return;
    }

    std::wstring sourceId;
    bool loopback = true;
    std::wstring masterVolumeId;
    if (needSystemSource) {
        for (auto& d : g_deviceManager->enumerateCaptureDevices()) {
            if (IsCableDevice(d.name)) {
                sourceId = d.id;
                loopback = false;
                break;
            }
        }
        if (sourceId.empty()) sourceId = cableRender->id;
        masterVolumeId = cableRender->id;

        std::wstring currentBase = cableRender->name.substr(0, cableRender->name.find(L" ("));
        if (currentBase != groupName) {
            RenameEndpoint(cableRender->id, groupName);
        }

        g_previousDefaultId = GetDefaultRenderId();
        if (g_previousDefaultId == cableRender->id) {
            g_previousDefaultId = outputs[0].deviceId;
        }
        PersistPreviousDefault();
        SetDefaultEndpointAllRoles(cableRender->id);
    } else {
        // Pure app-sourced group: the real default device stays in place.
        loopback = false;
        g_previousDefaultId.clear();
    }

    // Send each captured app's own output into the cable: inaudible there,
    // while the process-loopback capture still sees its full-volume stream.
    for (DWORD pid : appPids) {
        if (SetAppRenderEndpoint(pid, cableRender->id)) {
            g_routedPids.push_back(pid);
        }
    }
    SaveRoutedAppsToIni();

    LOG_INFO(L"Démarrage du groupe « " + groupName + L" » : " + std::to_wstring(outputs.size()) +
             L" sortie(s), source " + (needSystemSource ? L"système" : L"application") +
             L", marge " + std::to_wstring(g_baseCushionMs) + L"ms, tampon " +
             std::to_wstring(g_deviceFillMs) + L"ms, basse latence " + (g_lowLatency ? L"ON" : L"OFF") + L".");
    std::wstring error;
    if (!g_engine.start(sourceId, loopback, masterVolumeId, outputs, error)) {
        // Only the synchronous pre-checks (already running, nothing selected)
        // fail here; device negotiation itself now runs in the background.
        RestoreDefaultAndUnroute();
        SendError(error);
        return;
    }

    // The status timer does double duty: while isStarting(), it polls for
    // that to finish (see WM_TIMER); once running, it reports counters, and
    // the VU timer the level meters.
    g_playingSetup = g_activeSetup;
    g_awaitingGroupStart = true;
    g_statusTimerForGroup = true;
    UpdateMeterTimers();
    SendBanner(L"");
    SendState();
}

void OnStop(HWND hwnd) {
    g_statusTimerForGroup = false;
    UpdateMeterTimers();
    SendBanner(L"");
    if (g_optimizing) { KillTimer(hwnd, IDT_OPT_TIMER); g_optimizing = false; }
    g_awaitingGroupStart = false;
    g_playingSetup.clear();
    // Threads are torn down in the background; Windows is put back in
    // order right away (neither step needs the other).
    g_engine.stopAsync();
    RestoreDefaultAndUnroute();
    SendState();
}

void ToggleGroup(HWND hwnd) {
    if (g_engine.isRunning()) OnStop(hwnd);
    else if (!g_engine.isStarting() && !g_engine.isStopping()) OnCreateGroup(hwnd);
}

// Mic-based latency alignment (see Calibrator.h). The group must be silent
// during the sweeps: a playing group is stopped here and restarted by
// IDT_CAL_TIMER once the pass ends.
void BeginMicCalibration(HWND hwnd, const std::wstring& micId) {
    if (g_engine.isStarting()) {
        SendError(L"Patientez : le groupe est en cours de démarrage.");
        return;
    }
    if (g_calibrating || micId.empty()) return;
    std::vector<std::wstring> ids;
    for (const auto& d : g_outputDevices) {
        if (g_settings[d.id].checked) ids.push_back(d.id);
    }
    if (ids.size() < 2) {
        SendError(L"Cochez au moins deux enceintes à caler entre elles.");
        return;
    }
    if (g_engine.isRunning()) {
        g_calRestartAfter = true; // stays set across the queued re-entry below
        OnStop(hwnd);
    }
    if (g_engine.isStopping()) {
        QueueAfterStop(hwnd, AfterStop::Calibrate, micId);
        return;
    }
    if (!g_calibrator.start(micId, ids, g_deviceFillMs, g_lowLatency)) return;
    g_calibrating = true;
    SetTimer(hwnd, IDT_CAL_TIMER, 250, nullptr);
    SendCalStatus();
}

// The (id, source) set a running group is built from: two configs with the
// same set differ only in per-output knobs the engine changes live.
std::set<std::pair<std::wstring, unsigned long>> ActiveOutputSet() {
    std::set<std::pair<std::wstring, unsigned long>> set;
    for (const auto& d : g_outputDevices) {
        const auto& s = g_settings[d.id];
        if (s.checked) set.emplace(d.id, s.sourcePid);
    }
    return set;
}

// Pushes the live config of every active output into the running engine
// -- what a setup switch between two presets built on the same speakers
// amounts to, with no audible gap instead of a full stop/start.
void PushLiveSettingsToEngine(const std::wstring& groupName) {
    for (const auto& d : g_outputDevices) {
        const auto& s = g_settings[d.id];
        if (!s.checked) continue;
        g_engine.setLatencyMs(d.id, s.latencyMs);
        g_engine.setChannelMode(d.id, s.channelMode);
        g_engine.setVolumePercent(d.id, EffectiveVolume(s));
        for (int b = 0; b < kEqBandCount; ++b) g_engine.setEqBand(d.id, b, s.eqDb[b]);
        g_engine.setMuted(d.id, EffectiveMuted(s));
        g_engine.resyncOutput(d.id);
    }
    // The Windows-visible endpoint name follows the setup (see OnCreateGroup).
    for (const auto& d : g_deviceManager->enumerateRenderDevices()) {
        if (!IsCableDevice(d.name)) continue;
        std::wstring currentBase = d.name.substr(0, d.name.find(L" ("));
        if (currentBase != groupName) RenameEndpoint(d.id, groupName);
        break;
    }
}

// --- Setup actions ---

void SaveSetupFromCurrent(std::wstring name) {
    // Trim whitespace and strip '|' (the INI serialization delimiter).
    name.erase(std::remove(name.begin(), name.end(), L'|'), name.end());
    size_t b = name.find_first_not_of(L" \t");
    size_t e = name.find_last_not_of(L" \t");
    name = (b == std::wstring::npos) ? L"" : name.substr(b, e - b + 1);
    if (name.empty()) {
        SendError(L"Donnez un nom au setup avant de l'enregistrer.");
        return;
    }
    if (name.size() > 60) name = name.substr(0, 60);

    Setup setup;
    setup.name = name;
    setup.micId = g_setupMicId;
    if (!setup.micId.empty()) {
        // Snapshot the mic's current Windows level/mute so applying the
        // setup restores them too. Fails harmlessly (micVolume stays -1,
        // level untouched on apply) if the mic is unplugged right now.
        GetCaptureVolume(setup.micId, setup.micVolume, setup.micMute);
    }
    bool anyChecked = false;
    for (const auto& d : g_outputDevices) {
        const auto& s = g_settings[d.id];
        if (s.checked) anyChecked = true;
        setup.devices.emplace_back(d.id, s);
    }
    if (!anyChecked) {
        SendError(L"Cochez d'abord les enceintes à inclure dans ce setup.");
        return;
    }

    bool replaced = false;
    for (auto& existing : g_setups) {
        if (existing.name == name) {
            existing = setup;
            replaced = true;
        }
    }
    if (!replaced) g_setups.push_back(std::move(setup));
    g_activeSetup = name;
    g_editingSetup = name;
    SaveConfig();
    UpdateSetupDigitHotkeys(g_mainWnd);
    SendState();
}

// Copies a setup's devices/settings into the live config (g_settings) and
// marks it as the active/editing setup. Shared by ApplySetup() (which also
// touches playback) and SelectSetup() (which deliberately doesn't). Returns
// false if no such setup exists.
bool MergeSetupIntoLiveSettings(const std::wstring& name) {
    const Setup* setup = nullptr;
    for (const auto& s : g_setups) {
        if (s.name == name) { setup = &s; break; }
    }
    if (!setup) return false;

    // The setup defines the selection wholesale: anything it doesn't mention
    // (e.g. a speaker plugged in after the setup was saved) gets unchecked.
    for (const auto& d : g_outputDevices) g_settings[d.id].checked = false;
    for (const auto& [id, s] : setup->devices) {
        OutputSettings merged = s;
        // alias, color, list order and per-speaker source are device-level
        // state (and sources are runtime-only): they must survive applying a
        // preset saved at another time.
        auto existing = g_settings.find(id);
        if (existing != g_settings.end()) {
            merged.alias = existing->second.alias;
            merged.order = existing->second.order;
            merged.sourcePid = existing->second.sourcePid;
            merged.colorHex = existing->second.colorHex;
        }
        g_settings[id] = merged;
    }

    // The setup's mic becomes the live selection here (so the INPUTS tab
    // reflects it), but Windows' default capture device only changes in
    // ApplySetup() -- selecting a setup for editing must not switch mics,
    // mirroring how it doesn't touch playback.
    g_setupMicId = setup->micId;

    g_activeSetup = name;
    g_editingSetup = name;
    return true;
}

void ApplySetup(HWND hwnd, const std::wstring& name, bool ensureRunning) {
    if (g_engine.isStarting()) {
        SendError(L"Patientez : le groupe est en cours de démarrage.");
        return;
    }
    bool wasRunning = g_engine.isRunning();
    auto before = wasRunning ? ActiveOutputSet() : std::set<std::pair<std::wstring, unsigned long>>{};
    if (!MergeSetupIntoLiveSettings(name)) return; // unknown setup: nothing touched
    if (wasRunning && ActiveOutputSet() == before) {
        // Same speakers, same sources: switch live, no restart, no gap.
        PushLiveSettingsToEngine(name);
        g_playingSetup = name;
        wasRunning = false; // nothing left to restart below
        ensureRunning = false;
    } else if (wasRunning) {
        OnStop(hwnd);
    }
    if (!g_setupMicId.empty()) {
        SetDefaultEndpointAllRoles(g_setupMicId);
        // Restore the level/mute snapshotted when the setup was saved
        // (micVolume -1 = pre-snapshot setup, leave the level alone).
        for (const auto& s : g_setups) {
            if (s.name != name) continue;
            if (s.micVolume >= 0) {
                SetCaptureVolume(s.micId, s.micVolume);
                SetCaptureMute(s.micId, s.micMute);
            }
            break;
        }
    }
    SaveConfig();
    SendState();
    if (wasRunning || ensureRunning) OnCreateGroup(hwnd); // (re)start playback on the new configuration
}

// Loads a setup into the live device list (so its speakers/settings can be
// viewed and edited -- e.g. via the checkboxes and per-device sliders, then
// saved back with the "update setup" button) without touching playback at
// all. Lets the user select and modify a setup that isn't currently
// running; starting it is then a deliberate, separate action (the row's
// play button, or the main "Démarrer le groupe" button).
void SelectSetup(const std::wstring& name) {
    if (!MergeSetupIntoLiveSettings(name)) return;
    SaveConfig();
    SendState();
}

void DeleteSetup(const std::wstring& name) {
    g_setups.erase(std::remove_if(g_setups.begin(), g_setups.end(),
                                  [&](const Setup& s) { return s.name == name; }),
                   g_setups.end());
    if (g_activeSetup == name) g_activeSetup.clear();
    if (g_editingSetup == name) g_editingSetup.clear();
    if (g_playingSetup == name) g_playingSetup.clear();
    SaveConfig();
    UpdateSetupDigitHotkeys(g_mainWnd);
    SendState();
}

// --- Options: export/import the whole config file ---

void ExportConfig(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"AudioLinker.json";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Configuration AudioLinker (*.json)\0*.json\0Tous les fichiers (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return; // cancelled

    if (g_store.exportTo(path, g_cfg)) {
        SendInfo(L"Configuration exportée.");
    } else {
        SendError(L"Échec de l'export (erreur " + std::to_wstring(GetLastError()) + L").");
    }
}

void ImportConfig(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Configuration AudioLinker (*.json;*.ini)\0*.json;*.ini\0Tous les fichiers (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return; // cancelled

    // Hot-reloading setups/hotkeys/devices/EQ into a possibly-running group
    // has too many edge cases to do safely live -- relaunching from scratch
    // against the newly-imported file is what actually guarantees every
    // setting ends up consistent with it.
    if (MessageBoxW(hwnd,
            L"Importer ce fichier remplacera l'intégralité de la configuration actuelle "
            L"(setups, enceintes, raccourcis, réglages) et redémarrera AudioLinker.\n\n"
            L"Continuer ?",
            L"AudioLinker", MB_ICONWARNING | MB_YESNO) != IDYES) {
        return;
    }

    // Parsed first (a .json export, or a legacy .ini) so a broken file is
    // refused here instead of wiping the current config.
    AppConfig imported;
    if (!g_store.importFrom(path, imported)) {
        SendError(L"Ce fichier n'est pas une configuration AudioLinker valide.");
        return;
    }
    // A running group's displaced default / routed apps belong to THIS
    // session, which is about to shut down cleanly and restore them.
    imported.previousDefault.clear();
    imported.routedApps.clear();
    if (!g_store.exportTo(g_store.path(), imported)) {
        SendError(L"Échec de l'import (erreur " + std::to_wstring(GetLastError()) + L").");
        return;
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    // --restart: the new instance waits for this one's single-instance mutex
    // instead of seeing it still held and exiting immediately.
    std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" --restart";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exePath, cmdLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    // Same clean shutdown path as the tray's Quitter (stops the engine,
    // restores the previous default device, unroutes apps) -- EXCEPT the
    // final SaveConfig(), which would clobber the just-imported file with
    // the old in-memory settings (that was a real bug: the import silently
    // lost everything but the [setup_N] sections).
    g_skipSaveOnExit = true;
    DestroyWindow(g_mainWnd);
}

} // namespace al
