// The launcher window (the app's main window) procedure, the pieces both page
// hosts share (HTML resources, WebView2 hardening), window placement and
// RunApplication. The other windows live in Panels.cpp.

#include "App.h"
#include "Gui.h"

namespace al {

// The UI pages (ui.html, options.html) are embedded as RCDATA resources
// (UTF-8) so the exe stays self-contained -- no loose .html files to ship or
// tamper with.
std::wstring LoadHtmlResource(int resId) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!res) return L"";
    HGLOBAL data = LoadResource(mod, res);
    if (!data) return L"";
    const char* bytes = static_cast<const char*>(LockResource(data));
    DWORD size = SizeofResource(mod, res);
    if (!bytes || size == 0) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(size), nullptr, 0);
    std::wstring html(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(size), html.data(), wlen);
    return html;
}

// The pages are fully embedded (NavigateToString) and contain no external
// link, so any other navigation or popup can only be unintended -- block it
// rather than let a stray link turn the app window into a browser.
void HardenWebView(ICoreWebView2* webview) {
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview->get_Settings(settings.GetAddressOf())) && settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
#ifdef NDEBUG
        settings->put_AreDevToolsEnabled(FALSE);
#endif
        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3))) {
            // F5 would reload the page and drop its state; Ctrl+P, Ctrl+F... are noise.
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }
    }
    EventRegistrationToken token;
    webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    const std::wstring u = uri;
                    if (u.rfind(L"data:", 0) != 0 && u.rfind(L"about:", 0) != 0) {
                        args->put_Cancel(TRUE);
                    }
                    CoTaskMemFree(uri);
                }
                return S_OK;
            }).Get(), &token);
    webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(), &token);
}

// --- Window placement (persisted as [window] x/y/w/h, the normal rect) ---

// Captures the window's normal rect into the config (written with the next
// save -- the callers that hide or destroy the window flush right after).
void SaveWindowPlacement(HWND hwnd) {
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (!GetWindowPlacement(hwnd, &wp)) return;
    const RECT& r = wp.rcNormalPosition;
    if (r.right <= r.left || r.bottom <= r.top) return;
    g_cfg.window = { r.left, r.top, r.right - r.left, r.bottom - r.top };
    SaveConfig();
}

// False if nothing usable was saved, or the saved rect no longer touches any
// monitor (screen unplugged since) -- the caller then uses the defaults.
bool LoadWindowPlacement(RECT& out) {
    const WindowPlacement& w = g_cfg.window;
    if (!w.valid()) return false;
    out = { w.x, w.y, w.x + w.w, w.y + w.h };
    return MonitorFromRect(&out, MONITOR_DEFAULTTONULL) != nullptr;
}


// --- Window ---

// Set by RunApplication before the window exists (--tray launch).
bool g_startHidden = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Registered (runtime) message id, so it can't live in the switch below:
    // a second instance asking this one to come to the foreground.
    if (g_showAppMessage != 0 && msg == g_showAppMessage) {
        RestoreFromTray(hwnd);
        return 0;
    }
    // Frame-less window: sizing, focus, activation, DPI... are shared with
    // the other windows (Panels.cpp).
    {
        bool handled = false;
        LRESULT r = PanelCommonProc(PanelId::Launcher, hwnd, msg, wParam, lParam, handled);
        if (handled) return r;
    }
    switch (msg) {
        case WM_CREATE: {
            g_mainWnd = hwnd;
            AttachLauncherWindow(hwnd, g_startHidden);
            JournalInstall(); // the diagnostic log also feeds the Journal window
            // g_cfg was loaded in RunApplication (before the window existed).
            g_autostartPending = g_autostart;
            g_engine.setBaseCushionMs(g_baseCushionMs);
            g_engine.setDeviceFillMs(g_deviceFillMs);
            g_engine.setLowLatency(g_lowLatency);
            RepairAfterCrash(); // default output + app routing left over by a crash

            PopulateOutputList();
            AddTrayIcon(hwnd);
            g_deviceManager->registerNotifications(&DeviceNotificationSink());
            // A combo can be owned by another app (or a previous zombie
            // instance): the hotkey would then be silently dead. Surface it
            // once the UI is up ('ready') -- there is no web view yet here.
            if (!RegisterHotKey(hwnd, HOTKEY_TOGGLE_GROUP, g_toggleHotkeyMods | MOD_NOREPEAT,
                                g_toggleHotkeyVk)) {
                g_startupWarning = L"Le raccourci " + FormatHotkey(g_toggleHotkeyMods, g_toggleHotkeyVk) +
                                   L" (démarrer/arrêter) est déjà utilisé par une autre application.";
            }
            if (!RegisterHotKey(hwnd, HOTKEY_SETUP_SWITCHER, g_switcherHotkeyMods | MOD_NOREPEAT,
                                g_switcherHotkeyVk)) {
                if (!g_startupWarning.empty()) g_startupWarning += L"\n";
                g_startupWarning += L"Le raccourci " + FormatHotkey(g_switcherHotkeyMods, g_switcherHotkeyVk) +
                                    L" (changer de setup) est déjà utilisé par une autre application.";
            }
            UpdateSetupDigitHotkeys(hwnd);
            InitPanelWebViewEnvironment(hwnd);
            return 0;
        }
        case WM_SIZE:
            // Minimized from the taskbar button: same as closing, the app
            // lives in the tray.
            if (wParam == SIZE_MINIMIZED) HideLauncherToTray();
            return 0;
        case WM_JOURNAL:
            JournalFromMessage(lParam);
            return 0;
        case WM_TRAYICON: {
            if (LOWORD(lParam) == WM_LBUTTONUP) RestoreFromTray(hwnd);
            else if (LOWORD(lParam) == WM_RBUTTONUP) ShowTrayMenu(hwnd);
            return 0;
        }
        case WM_HOTKEY:
            if (wParam == HOTKEY_TOGGLE_GROUP) {
                ToggleGroup(hwnd);
            } else if (wParam == HOTKEY_SETUP_SWITCHER) {
                switcher::Open(); // standalone topmost popup; main window stays as it is
            } else if (wParam >= HOTKEY_SETUP_FIRST &&
                       wParam < HOTKEY_SETUP_FIRST + kSetupHotkeyCount) {
                size_t idx = wParam - HOTKEY_SETUP_FIRST;
                if (idx < g_setups.size()) ApplySetup(hwnd, g_setups[idx].name);
            }
            return 0;
        case WM_DEVICES_CHANGED:
            // Hot-plug burst from the COM notification thread: (re)arm the
            // debounce, the actual refresh happens once things settle.
            SetTimer(hwnd, IDT_DEVICE_REFRESH, 600, nullptr);
            return 0;
        case WM_DEFAULT_CHANGED: {
            // Somebody moved Windows' default output away from the cable
            // (Sound settings, another switcher, a newly plugged device
            // Windows promoted) while a group depends on it: the PC's audio
            // just left the group, silently. Our own SetDefaultEndpoint
            // calls land here too, hence the timestamp filter.
            if (g_previousDefaultId.empty()) return 0; // no group holding the default
            if (GetTickCount64() - g_lastDefaultChangeTick < 2000) return 0;
            std::wstring current = GetDefaultRenderId();
            for (const auto& d : g_deviceManager->enumerateRenderDevices()) {
                if (d.id == current && IsCableDevice(d.name)) return 0; // still ours
            }
            LOG_WARN(L"La sortie par défaut a été changée hors d'AudioLinker pendant le groupe.");
            SendBanner(L"Le son a quitté le groupe : Windows a changé de sortie par défaut.",
                       L"reclaim", L"Reprendre");
            SendErrorWithAction(L"Le son a quitté le groupe : Windows a changé de sortie par défaut.",
                                L"reclaim", L"Reprendre");
            return 0;
        }
        case WM_POWERBROADCAST:
            // Back from sleep: every stream is stale (the retry loops would
            // recover on their own, but a clean restart lands faster and
            // re-establishes the cushions exactly). Delayed so the audio
            // stack has finished waking up first.
            if (wParam == PBT_APMRESUMEAUTOMATIC && (g_engine.isRunning() || g_engine.isStarting())) {
                LOG_INFO(L"Reprise de veille : redémarrage du groupe dans 2 s.");
                SetTimer(hwnd, IDT_RESUME, 2000, nullptr);
            }
            return TRUE;
        case WM_CLOSE:
            HideLauncherToTray();
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id >= ID_TRAY_SETUP_BASE && id < ID_TRAY_SETUP_BASE + kMaxTraySetups) {
                size_t idx = static_cast<size_t>(id - ID_TRAY_SETUP_BASE);
                if (idx < g_setups.size()) ApplySetup(hwnd, g_setups[idx].name);
                return 0;
            }
            switch (id) {
                case ID_TRAY_TOGGLE: ToggleGroup(hwnd); return 0;
                case ID_TRAY_SHOW: RestoreFromTray(hwnd); return 0;
                case ID_TRAY_QUIT: DestroyWindow(hwnd); return 0;
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == IDT_DEVICE_REFRESH) {
                KillTimer(hwnd, IDT_DEVICE_REFRESH);
                PopulateOutputList();
                if (g_micMetering) StartMicMetering(); // fresh mic set
                SendState();
                SendSources();
                return 0;
            }
            if (wParam == IDT_RESUME) {
                KillTimer(hwnd, IDT_RESUME);
                if (g_engine.isRunning()) {
                    OnStop(hwnd);
                    OnCreateGroup(hwnd);
                }
                return 0;
            }
            if (wParam == IDT_SAVE) {
                FlushConfig(); // kills the timer itself
                return 0;
            }
            if (wParam == IDT_VU_TIMER) {
                if (!g_engine.isStarting()) SendPeaks();
                return 0;
            }
            if (wParam == IDT_STOP_POLL) {
                if (g_engine.isStopping()) return 0; // still winding down
                KillTimer(hwnd, IDT_STOP_POLL);
                RunAfterStop(hwnd);
                return 0;
            }
            if (wParam == IDT_AUTOMATION) {
                KillTimer(hwnd, IDT_AUTOMATION);
                std::wstring name = g_pendingAutoSetup;
                g_pendingAutoSetup.clear();
                if (!name.empty()) ApplySetup(hwnd, name, /*ensureRunning=*/true);
                return 0;
            }
            if (wParam == IDT_OPT_TIMER) {
                if (g_optimizing) OptimizerTick(hwnd);
                return 0;
            }
            if (wParam == IDT_HK_CAPTURE) {
                if (g_hotkeyCaptureId) {
                    StopHotkeyCapture();
                    SendInfo(L"Capture du raccourci annulée : aucune combinaison en 10 s.");
                    SendState();
                }
                return 0;
            }
            if (wParam == IDT_CAL_TIMER) {
                SendCalStatus(); // includes the final (running=false) snapshot
                if (g_calibrating && !g_calibrator.isRunning()) {
                    KillTimer(hwnd, IDT_CAL_TIMER);
                    g_calibrating = false;
                    bool restart = g_calRestartAfter;
                    g_calRestartAfter = false;
                    auto snap = g_calibrator.snapshot();
                    if (!snap.error.empty()) {
                        SendError(snap.error);
                    } else if (!snap.cancelled && g_cfg.captureMode == 1) {
                        // "No cable" mode: the real default output plays natively,
                        // the group outputs reach the ear after the whole pipeline
                        // (capture + cushion + device fill) plus their own delay.
                        // latency_i = defaultDelay - pipeline - delay_i; a negative
                        // value means the native output leads by more than the
                        // pipeline can absorb -- the physical limit of this mode,
                        // reported as the measured deficit (go / no-go data).
                        std::wstring defaultId = GetDefaultRenderId();
                        int defaultDelay = -1, okCount = 0;
                        for (const auto& o : snap.outcomes) {
                            if (o.state == CalState::Done && o.deviceId == defaultId) defaultDelay = o.delayMs;
                        }
                        if (defaultDelay < 0) {
                            SendError(L"Calage sans câble : la sortie par défaut (référence) n'a pas pu être mesurée.");
                        } else {
                            const int pipelineMs = g_baseCushionMs + g_deviceFillMs + 20;
                            int worstDeficit = 0;
                            for (const auto& o : snap.outcomes) {
                                if (o.state != CalState::Done || o.deviceId == defaultId) continue;
                                ++okCount;
                                int wanted = defaultDelay - pipelineMs - o.delayMs;
                                if (wanted < 0) worstDeficit = std::max(worstDeficit, -wanted);
                                g_settings[o.deviceId].latencyMs = ClampLatencyMs(std::max(0, wanted));
                            }
                            g_activeSetup.clear();
                            SaveConfig();
                            SendState();
                            if (okCount == 0) {
                                SendError(L"Calage sans câble : aucune enceinte du groupe n'a pu être mesurée.");
                            } else if (worstDeficit > 0) {
                                SendError(L"Calage sans câble : la sortie par défaut a " + std::to_wstring(worstDeficit) +
                                          L" ms d'avance que le groupe ne peut pas rattraper (latences mises à 0). "
                                          L"Baissez la marge et le tampon, ou prenez une sortie par défaut plus lente.");
                            } else {
                                SendInfo(L"Calage sans câble appliqué : groupe aligné sur la sortie par défaut (" +
                                         std::to_wstring(defaultDelay) + L" ms mesurés, pipeline ≈ " +
                                         std::to_wstring(pipelineMs) + L" ms).");
                            }
                        }
                    } else if (!snap.cancelled) {
                        // Align every measured output on the slowest one --
                        // same principle as the deviceLagMs-based autolatency,
                        // but from real acoustic arrival times.
                        int maxDelay = -1, okCount = 0;
                        for (const auto& o : snap.outcomes) {
                            if (o.state != CalState::Done) continue;
                            ++okCount;
                            maxDelay = std::max(maxDelay, o.delayMs);
                        }
                        if (okCount >= 2) {
                            for (const auto& o : snap.outcomes) {
                                if (o.state != CalState::Done) continue;
                                g_settings[o.deviceId].latencyMs = ClampLatencyMs(maxDelay - o.delayMs);
                            }
                            g_activeSetup.clear(); // latencies diverged from any preset
                            SaveConfig();
                            SendState();
                            std::wstring summary = L"Calage micro appliqué : latences alignées sur "
                                               L"l'enceinte la plus lente (" +
                                               std::to_wstring(maxDelay) + L" ms).";
                            if (okCount < static_cast<int>(snap.outcomes.size())) {
                                summary += L" Certaines enceintes n'ont pas pu être mesurées.";
                            }
                            SendInfo(summary);
                        } else {
                            SendError(L"Calage micro : le signal n'a pas été détecté. "
                                      L"Rapprochez le micro des enceintes et vérifiez leur volume.");
                        }
                    }
                    if (restart) OnCreateGroup(hwnd);
                }
                return 0;
            }
            if (wParam == IDT_STATUS_TIMER) {
                if (g_engine.isStarting()) {
                    return 0; // still negotiating devices in the background
                }
                if (g_awaitingGroupStart) {
                    g_awaitingGroupStart = false;
                    std::wstring error;
                    if (g_engine.takeStartError(error)) {
                        LOG_ERROR(L"Échec du démarrage du groupe : " + error);
                        g_statusTimerForGroup = false;
                        UpdateMeterTimers();
                        g_playingSetup.clear();
                        RestoreDefaultAndUnroute();
                        SendState();
                        SendError(error);
                        return 0;
                    }
                    SaveConfig();
                    SendState();
                    return 0;
                }
                SendStatus();
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam) {
                // System is shutting down or user is logging off.
                // Stop the engine and restore default audio device.
                if (g_engine.isRunning()) {
                    g_engine.stop();
                }
                RestoreDefaultAndUnroute();
                FlushConfig();
            }
            return 0;
        case WM_DESTROY:
            // Skipped after a config import: the imported file must reach
            // the relaunched instance untouched (see ImportConfig).
            KillTimer(hwnd, IDT_SAVE);
            if (!g_skipSaveOnExit) SaveWindowPlacement(hwnd);
            g_calibrator.cancel(); // its destructor joins the worker
            StopHotkeyCapture();
            CloseSwitcherHook();
            if (g_switcherWnd) {
                DestroyWindow(g_switcherWnd);
                g_switcherWnd = nullptr;
            }
            if (!g_skipSaveOnExit) SavePanelPlacements();
            DestroyAllPanels();
            g_deviceManager->unregisterNotifications(&DeviceNotificationSink());
            UnregisterHotKey(hwnd, HOTKEY_TOGGLE_GROUP);
            UnregisterHotKey(hwnd, HOTKEY_SETUP_SWITCHER);
            for (int i = 0; i < kSetupHotkeyCount; ++i) {
                UnregisterHotKey(hwnd, HOTKEY_SETUP_FIRST + i);
            }
            Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
            g_engine.stop();
            RestoreDefaultAndUnroute();
            if (!g_skipSaveOnExit) FlushConfig();
            DeleteObject(g_hBrushWindowBg);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace al

using namespace al;

bool QuitRunningInstance(DWORD timeoutMs) {
    HWND wnd = FindWindowW(L"AudioLinkerMainWindow", nullptr);
    if (!wnd) return true; // nothing running
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    HANDLE proc = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    PostMessageW(wnd, WM_COMMAND, al::ID_TRAY_QUIT, 0);
    bool gone = true;
    if (proc) {
        gone = WaitForSingleObject(proc, timeoutMs) == WAIT_OBJECT_0;
        CloseHandle(proc);
    }
    return gone;
}

int RunApplication(HINSTANCE hInstance, int nCmdShow, bool startHidden, bool waitForPrevious) {
    LogInit();
    // Single instance: a second launch would mean two tray icons, hotkey
    // registrations silently failing, and two processes writing the same
    // ini. The mutex is held (owned) for this process's whole lifetime.
    g_showAppMessage = RegisterWindowMessageW(L"AudioLinker.ShowWindow");
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\AudioLinker.SingleInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (waitForPrevious) {
            // Config-import relaunch: the previous instance is shutting down
            // and will release the mutex when it exits -- wait for that
            // (WAIT_ABANDONED is the normal outcome: it dies still owning it).
            DWORD w = WaitForSingleObject(instanceMutex, 10000);
            if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
                CloseHandle(instanceMutex);
                return 0;
            }
        } else {
            // Defer to the running instance: bring its window up instead.
            HWND existing = FindWindowW(L"AudioLinkerMainWindow", nullptr);
            if (existing && g_showAppMessage != 0) {
                PostMessageW(existing, g_showAppMessage, 0, 0);
            }
            CloseHandle(instanceMutex);
            return 0;
        }
    }

    g_deviceManager = std::make_unique<DeviceManager>();

    // The whole config is loaded before any window exists: the class
    // background brush below already needs the theme, and the window its
    // saved placement. (First run after 1.1: the INI is migrated here.)
    g_store.init();
    g_store.load(g_cfg);

    g_startHidden = startHidden;
    g_windowBg = WindowBgForTheme(EffectiveLightTheme());
    g_hBrushWindowBg = CreateSolidBrush(g_windowBg);
    g_hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));

    const wchar_t* className = L"AudioLinkerMainWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_hBrushWindowBg;
    wc.hIcon = g_hAppIcon;
    RegisterClassW(&wc);

    // Where the launcher goes: its last saved position if it still lands on a
    // monitor, otherwise the top-left corner of the work area of the monitor
    // under the cursor. Its size is fixed (scaled by that monitor's DPI).
    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi = 0, dpiY = 0;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpiY)) || dpi == 0) dpi = GetDpiForSystem();
    if (dpi == 0) dpi = 96;
    int w = MulDiv(kLauncherW, static_cast<int>(dpi), 96);
    int h = MulDiv(kLauncherH, static_cast<int>(dpi), 96);
    int x, y;
    RECT saved;
    if (LoadWindowPlacement(saved)) {
        x = saved.left;
        y = saved.top;
    } else {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        x = mi.rcWork.left + MulDiv(40, static_cast<int>(dpi), 96);
        y = mi.rcWork.top + MulDiv(40, static_cast<int>(dpi), 96);
    }
    // Borderless (the page draws the frame), with a taskbar button: the
    // launcher is how the app is found again.
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, className, L"AudioLinker — Groupe d'enceintes",
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
        x, y, w, h,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        g_deviceManager.reset();
        if (instanceMutex) CloseHandle(instanceMutex);
        return 0;
    }

    ApplyFlatFrame(hwnd);

    // --tray: stay hidden; the tray icon (added in WM_CREATE) is the only
    // presence until the user opens the window. WebView2 initializes fine
    // against a hidden host, so the UI is ready the moment it's shown.
    if (!startHidden) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    (void)nCmdShow; // the launcher always opens at its saved place, whatever the shortcut asks

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_deviceManager.reset();
    // Released as late as possible: an import-relaunch (--restart) waits on
    // this mutex and must not proceed while this process still owns the ini.
    if (instanceMutex) CloseHandle(instanceMutex);
    return static_cast<int>(msg.wParam);
}
