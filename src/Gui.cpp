// Main window: theme application, WebView2 bootstrap and background
// footprint, window placement, the window procedure, and RunApplication.

#include "App.h"
#include "Gui.h"

namespace al {

void ApplyWindowTheme(HWND hwnd) {
    bool light = EffectiveLightTheme();
    g_windowBg = WindowBgForTheme(light);

    HBRUSH newBrush = CreateSolidBrush(g_windowBg);
    SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(newBrush));
    if (g_hBrushWindowBg) DeleteObject(g_hBrushWindowBg);
    g_hBrushWindowBg = newBrush;

    BOOL darkMode = light ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    if (g_webviewController) {
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(g_webviewController->QueryInterface(IID_PPV_ARGS(controller2.GetAddressOf())))) {
            COREWEBVIEW2_COLOR bg = { 255, GetRValue(g_windowBg), GetGValue(g_windowBg), GetBValue(g_windowBg) };
            controller2->put_DefaultBackgroundColor(bg);
        }
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

// Same idea as ApplyWindowTheme but for the Options window's own class brush
// (a separate window class, so it needs its own brush tracked separately --
// see g_optionsHBrushWindowBg). No-op if the window isn't currently open.
// Call ApplyWindowTheme(g_mainWnd) first: this reuses the g_windowBg it just
// recomputed instead of recomputing it a second time.

// --- WebView2 bootstrap ---

// Both UI pages (ui.html, options.html) are embedded as RCDATA resources
// (UTF-8) so the exe stays self-contained — no loose .html files to ship or
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

void WebViewFatalError(HWND hwnd) {
    MessageBoxW(hwnd,
        L"Impossible d'initialiser WebView2.\n\n"
        L"Le runtime « Microsoft Edge WebView2 » est requis (préinstallé sur "
        L"Windows 11, téléchargeable depuis le site de Microsoft sinon).",
        L"AudioLinker", MB_ICONERROR);
    DestroyWindow(hwnd);
}

void OnWebViewControllerReady(HWND hwnd, ICoreWebView2Controller* controller) {
    g_webviewController = controller;
    // Explicitly sync WebView2's own visibility flag to the host window's
    // current state instead of relying on it to follow the HWND on its own:
    // a controller created against a window that has never been shown yet
    // (--tray autostart at Windows login) can otherwise end up with a blank
    // surface the first time the window is actually shown. Kept in sync on
    // every later hide/show too (WM_CLOSE, SIZE_MINIMIZED, RestoreFromTray).
    controller->put_IsVisible(IsWindowVisible(hwnd));
    if (FAILED(controller->get_CoreWebView2(g_webview.GetAddressOf())) || !g_webview) {
        WebViewFatalError(hwnd);
        return;
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(g_webview->get_Settings(settings.GetAddressOf())) && settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
    }

    // Matches the page's current background, so resizing never flashes the
    // wrong color behind the (asynchronously repainted) web content.
    ApplyWindowTheme(hwnd);

    g_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [hwnd](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR msg = nullptr;
                if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                    HandleWebMessage(hwnd, msg);
                    CoTaskMemFree(msg);
                }
                return S_OK;
            }).Get(), &g_webMessageToken);

    RECT rc;
    GetClientRect(hwnd, &rc);
    controller->put_Bounds(rc);

    std::wstring html = LoadHtmlResource(IDR_UI_HTML);
    if (html.empty()) {
        WebViewFatalError(hwnd);
        return;
    }
    g_webview->NavigateToString(html.c_str());
}

void InitWebView(HWND hwnd) {
    // WebView2 refuses to write its profile next to Program Files; give it an
    // explicit per-user data folder.
    std::wstring userData;
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) > 0) {
        userData = std::wstring(buf) + L"\\AudioLinker\\WebView2";
    }

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr,
        userData.empty() ? nullptr : userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    WebViewFatalError(hwnd);
                    return S_OK;
                }
                g_webviewEnvironment = env; // reused by the Options window's controller
                env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT result2, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result2) || !controller) {
                                WebViewFatalError(hwnd);
                                return S_OK;
                            }
                            OnWebViewControllerReady(hwnd, controller);
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    if (FAILED(hr)) WebViewFatalError(hwnd);
}

// --- Background footprint ---
// The app lives in the tray with its window hidden most of the time, but a
// merely-invisible WebView2 keeps its full Chromium render tree warm
// (150-250 MB across subprocesses). Suspending the page and asking the
// browser process to trim (where the runtime supports both -- best-effort
// QueryInterface otherwise) drops that to a fraction, for the cost of a
// brief repaint at the next restore. The Options window is not involved:
// its controller is destroyed outright when it closes.

void SuspendMainWebView() {
    // The page posts micmeter|0 and spectrum| from its visibilitychange
    // handler, but suspension can win that race -- stop metering here too so
    // the Windows "microphone in use" indicator never stays lit with the
    // window hidden, and nobody computes a spectrum nobody can see.
    StopMicMetering();
    g_engine.setSpectrumTarget(L"");
    if (!g_webviewController || !g_webview) return;
    g_webviewController->put_IsVisible(FALSE); // TrySuspend requires hidden
    ComPtr<ICoreWebView2_19> wv19;
    if (SUCCEEDED(g_webview.As(&wv19)) && wv19) {
        wv19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
    }
    ComPtr<ICoreWebView2_3> wv3;
    if (SUCCEEDED(g_webview.As(&wv3)) && wv3) {
        wv3->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>(
            [](HRESULT, BOOL) -> HRESULT { return S_OK; }).Get());
    }
}

void ResumeMainWebView() {
    if (!g_webviewController || !g_webview) return;
    // put_IsVisible(TRUE) resumes a suspended page on its own; the explicit
    // Resume() just removes any doubt about ordering across runtime versions.
    ComPtr<ICoreWebView2_3> wv3;
    BOOL suspended = FALSE;
    if (SUCCEEDED(g_webview.As(&wv3)) && wv3 &&
        SUCCEEDED(wv3->get_IsSuspended(&suspended)) && suspended) {
        wv3->Resume();
    }
    ComPtr<ICoreWebView2_19> wv19;
    if (SUCCEEDED(g_webview.As(&wv19)) && wv19) {
        wv19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
    }
    g_webviewController->put_IsVisible(TRUE);
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Registered (runtime) message id, so it can't live in the switch below:
    // a second instance asking this one to come to the foreground.
    if (g_showAppMessage != 0 && msg == g_showAppMessage) {
        RestoreFromTray(hwnd);
        return 0;
    }
    switch (msg) {
        case WM_CREATE: {
            g_mainWnd = hwnd;
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
            InitWebView(hwnd);
            return 0;
        }
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                SaveWindowPlacement(hwnd);
                ShowWindow(hwnd, SW_HIDE);
                SuspendMainWebView();
            } else if (g_webviewController) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                g_webviewController->put_Bounds(rc);
                g_webviewController->put_IsVisible(TRUE);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            // Per-monitor DPI aware (see AudioLinker.manifest): logical
            // pixel constants must be scaled by the window's current DPI.
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            UINT dpi = GetDpiForWindow(hwnd);
            if (dpi == 0) dpi = 96;
            RECT r = { 0, 0, MulDiv(kMinClientW, static_cast<int>(dpi), 96),
                             MulDiv(kMinClientH, static_cast<int>(dpi), 96) };
            AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
                                     FALSE, 0, dpi);
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        case WM_DPICHANGED: {
            // Moved to a monitor with a different scale factor: adopt the
            // size/position Windows computed. WebView2 re-rasterizes on its
            // own; the switcher popup already reads its DPI per-open.
            auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SETFOCUS:
            // Hand keyboard focus straight to the web content: the host
            // window itself has nothing focusable.
            if (g_webviewController) {
                g_webviewController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
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
        case WM_SETTINGCHANGE:
            // Windows broadcasts this with lParam == "ImmersiveColorSet" when
            // the user toggles Settings > Personnalisation > Couleurs > Mode.
            // ui.html re-themes itself via prefers-color-scheme automatically;
            // this only re-syncs the native chrome (title bar, host brush).
            if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
                ApplyWindowTheme(hwnd);
            }
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            // Windows accent color changed: the next state message carries
            // the new value, the web UI recolors itself from it.
            SendState();
            return 0;
        case WM_CLOSE:
            SaveWindowPlacement(hwnd);
            ShowWindow(hwnd, SW_HIDE);
            SuspendMainWebView();
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
                            std::wstring msg = L"Calage micro appliqué : latences alignées sur "
                                               L"l'enceinte la plus lente (" +
                                               std::to_wstring(maxDelay) + L" ms).";
                            if (okCount < static_cast<int>(snap.outcomes.size())) {
                                msg += L" Certaines enceintes n'ont pas pu être mesurées.";
                            }
                            SendInfo(msg);
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
            if (g_optionsWnd) {
                if (g_optionsController) {
                    g_optionsController->Close();
                    g_optionsController.Reset();
                }
                g_optionsWebview.Reset();
                DestroyWindow(g_optionsWnd);
                g_optionsWnd = nullptr;
            }
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
            if (g_webview) {
                g_webview->remove_WebMessageReceived(g_webMessageToken);
                g_webview.Reset();
            }
            if (g_webviewController) {
                g_webviewController->Close();
                g_webviewController.Reset();
            }
            DeleteObject(g_hBrushWindowBg);
            if (g_optionsHBrushWindowBg) {
                DeleteObject(g_optionsHBrushWindowBg);
                g_optionsHBrushWindowBg = nullptr;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace al

using namespace al;

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

    // Where the window goes: its last saved placement if it still lands on a
    // monitor, otherwise the default size scaled by the DPI of the monitor
    // under the cursor (per-monitor DPI aware; WM_DPICHANGED takes over if
    // the window is later dragged to a differently-scaled screen).
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w, h;
    RECT saved;
    if (LoadWindowPlacement(saved)) {
        x = saved.left;
        y = saved.top;
        w = saved.right - saved.left;
        h = saved.bottom - saved.top;
    } else {
        POINT cursor;
        GetCursorPos(&cursor);
        UINT dpi = 0, dpiY = 0;
        if (FAILED(GetDpiForMonitor(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY),
                                    MDT_EFFECTIVE_DPI, &dpi, &dpiY)) || dpi == 0) {
            dpi = GetDpiForSystem();
        }
        if (dpi == 0) dpi = 96;
        RECT r = { 0, 0, MulDiv(kDefaultClientW, static_cast<int>(dpi), 96),
                         MulDiv(kDefaultClientH, static_cast<int>(dpi), 96) };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
                                 FALSE, 0, dpi);
        w = r.right - r.left;
        h = r.bottom - r.top;
    }
    HWND hwnd = CreateWindowExW(0, className, L"AudioLinker — Groupe d'enceintes",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME |
            WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        x, y, w, h,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        g_deviceManager.reset();
        if (instanceMutex) CloseHandle(instanceMutex);
        return 0;
    }

    BOOL darkMode = EffectiveLightTheme() ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    // --tray: stay hidden; the tray icon (added in WM_CREATE) is the only
    // presence until the user opens the window. WebView2 initializes fine
    // against a hidden host, so the UI is ready the moment it's shown.
    if (!startHidden) {
        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
    }

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
