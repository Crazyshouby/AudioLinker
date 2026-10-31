// The floating windows of the KDX-style UI.
//
// Like CDX, AudioLinker is not one big window but a small launcher (the main
// window, a column of buttons) plus one top-level window per function:
// speaker list, setups, level meters, journal, options. Every window is
// borderless (WS_POPUP) and draws its own title bar in HTML: a close box on
// the left, a roll-up box on the right, a resize grip bottom right. The page
// asks the native side to start a move/resize (dragwin / resizewin), to roll
// the window up (shade) or to close it.
//
// Each window hosts its own WebView2 controller (all sharing one
// environment) showing the same embedded page, ui.html, told which window it
// is through documentElement.dataset.panel (options.html for Options). The
// native side stays the single owner of the state and broadcasts every JSON
// message to all windows that are open; windows are created when opened and
// destroyed when closed (a closed window costs nothing). Hiding the launcher
// to the tray hides everything and remembers what was open.

#include "App.h"

namespace al {

namespace {

struct PanelDef {
    const char* key;
    const wchar_t* title;
    int resId;
    int defW, defH; // logical px (96 dpi)
    int minW, minH;
    bool resizable;
    bool defaultOpen;
};

const PanelDef kDefs[kPanelCount] = {
    { "launcher", L"AudioLinker — Groupe d'enceintes", IDR_UI_HTML, kLauncherW, kLauncherH, kLauncherW, kLauncherH, false, true },
    { "outputs", L"Enceintes — AudioLinker", IDR_UI_HTML, 660, 560, 480, 320, true, true },
    { "setups", L"Setups — AudioLinker", IDR_UI_HTML, 360, 340, 260, 200, true, true },
    { "levels", L"Niveaux — AudioLinker", IDR_UI_HTML, 360, 250, 260, 140, true, false },
    { "journal", L"Journal — AudioLinker", IDR_UI_HTML, 660, 240, 320, 140, true, false },
    { "options", L"Options — AudioLinker", IDR_OPTIONS_HTML, 540, 640, 460, 320, true, false },
};

// Height of a rolled-up window: title bar + the frame's bottom edge (css px).
constexpr int kShadeCssPx = 26;

struct Panel {
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> ctl;
    ComPtr<ICoreWebView2> wv;
    EventRegistrationToken tok = {};
    bool creating = false;  // controller creation in flight
    bool ready = false;     // the page posted 'ready' and listens to messages
    bool wantOpen = false;  // should be open whenever the launcher is shown
    bool shaded = false;
    int fullH = 0;          // height to go back to when un-shaded
    std::vector<std::wstring> pending; // messages queued until 'ready'
};

Panel g_panels[kPanelCount];
bool g_trayHidden = false;

// A window being moved by its title bar (see the page's dragstart/dragmove/
// dragend): where the cursor and the window were when the press happened.
struct DragState {
    bool active = false;
    PanelId id = PanelId::Launcher;
    POINT cursor0 = {};
    RECT rect0 = {};
} g_drag;
HBRUSH g_panelBrush = nullptr;
constexpr const wchar_t* kPanelClass = L"AudioLinkerPanel";

Panel& P(PanelId id) { return g_panels[static_cast<int>(id)]; }
const PanelDef& Def(PanelId id) { return kDefs[static_cast<int>(id)]; }

UINT DpiFor(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    if (dpi == 0) dpi = GetDpiForSystem();
    return dpi ? dpi : 96;
}

// Rolls a window up to its title bar, or back. A window with a sizing frame
// cannot be made shorter than a system minimum, so the frame style is taken
// off while it is rolled up (it cannot be resized then anyway).
void ApplyShade(PanelId id, bool want) {
    Panel& p = P(id);
    if (!p.hwnd) return;
    RECT r;
    GetWindowRect(p.hwnd, &r);
    LONG_PTR style = GetWindowLongPtrW(p.hwnd, GWL_STYLE);
    if (want) {
        p.fullH = r.bottom - r.top;
        p.shaded = true;
        SetWindowLongPtrW(p.hwnd, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_THICKFRAME));
        SetWindowPos(p.hwnd, nullptr, 0, 0, r.right - r.left,
                     MulDiv(kShadeCssPx, static_cast<int>(DpiFor(p.hwnd)), 96),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    } else {
        p.shaded = false;
        SetWindowLongPtrW(p.hwnd, GWL_STYLE, style | WS_THICKFRAME);
        int h = p.fullH > 0 ? p.fullH : MulDiv(Def(id).defH, static_cast<int>(DpiFor(p.hwnd)), 96);
        SetWindowPos(p.hwnd, nullptr, 0, 0, r.right - r.left, h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

RECT WorkAreaNear(const RECT& r) {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcWork;
}

// Keeps a rect fully inside the work area of the monitor it is mostly on.
RECT ClampToWorkArea(RECT r) {
    RECT wa = WorkAreaNear(r);
    int w = std::min<int>(r.right - r.left, wa.right - wa.left);
    int h = std::min<int>(r.bottom - r.top, wa.bottom - wa.top);
    int x = std::max<int>(wa.left, std::min<int>(r.left, wa.right - w));
    int y = std::max<int>(wa.top, std::min<int>(r.top, wa.bottom - h));
    return { x, y, x + w, y + h };
}

// Where a window opens: where it was last time if that still touches a
// monitor, otherwise a default layout built around the launcher
// (outputs to its right, setups and levels further right, journal below).
RECT InitialRect(PanelId id) {
    const PanelDef& d = Def(id);
    UINT dpi = DpiFor(g_mainWnd);
    auto S = [&](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };

    auto it = g_cfg.panels.find(d.key);
    if (it != g_cfg.panels.end() && it->second.valid()) {
        const PanelPlacement& pp = it->second;
        RECT r = { pp.x, pp.y, pp.x + pp.w, pp.y + pp.h };
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return r;
    }

    RECT lr = { 0, 0, S(Def(PanelId::Launcher).defW), S(Def(PanelId::Launcher).defH) };
    if (g_mainWnd) GetWindowRect(g_mainWnd, &lr);
    const int gap = S(14);
    RECT out = { lr.right + gap, lr.top, 0, 0 };
    RECT outputs = { out.left, out.top, out.left + S(Def(PanelId::Outputs).defW), out.top + S(Def(PanelId::Outputs).defH) };
    RECT r;
    switch (id) {
        case PanelId::Outputs: r = outputs; break;
        case PanelId::Setups:
            r = { outputs.right + gap, outputs.top, outputs.right + gap + S(d.defW), outputs.top + S(d.defH) };
            break;
        case PanelId::Levels:
            r = { outputs.right + gap, outputs.top + S(Def(PanelId::Setups).defH) + gap,
                  outputs.right + gap + S(d.defW), outputs.top + S(Def(PanelId::Setups).defH) + gap + S(d.defH) };
            break;
        case PanelId::Journal:
            r = { outputs.left, outputs.bottom + gap, outputs.left + S(d.defW), outputs.bottom + gap + S(d.defH) };
            break;
        case PanelId::Options:
        default: {
            RECT wa = WorkAreaNear(lr);
            int w = S(d.defW), h = S(d.defH);
            int x = wa.left + ((wa.right - wa.left) - w) / 2, y = wa.top + ((wa.bottom - wa.top) - h) / 2;
            r = { x, y, x + w, y + h };
            break;
        }
    }
    return ClampToWorkArea(r);
}

void DestroyPanelWindow(PanelId id);

// The launcher is the app: without its page there is nothing to show. Any
// other window that cannot be built just closes and says so.
void PanelFailed(PanelId id);

void WebViewFatalError(HWND hwnd) {
    MessageBoxW(hwnd,
        L"Impossible d'initialiser l'interface web (WebView2).\n\n"
        L"Le runtime « Microsoft Edge WebView2 » est requis (préinstallé sur "
        L"Windows 11, téléchargeable depuis le site de Microsoft sinon).",
        L"AudioLinker", MB_ICONERROR);
    if (g_mainWnd) DestroyWindow(g_mainWnd);
}

void ResizeController(Panel& p) {
    if (!p.ctl || !p.hwnd) return;
    RECT rc;
    GetClientRect(p.hwnd, &rc);
    p.ctl->put_Bounds(rc);
}

// Whether the window is the active one comes from WM_ACTIVATE itself (asking
// GetForegroundWindow() while the message is handled can still name the old one).
void SendActive(PanelId id, bool active) {
    if (!P(id).hwnd) return;
    PostToPanel(id, std::wstring(L"{\"type\":\"active\",\"value\":") + (active ? L"true" : L"false") + L"}");
}

// The primary mouse button, as the system maps it (GetAsyncKeyState reads the
// physical buttons: with swapped buttons the primary one is the right one).
bool PrimaryButtonDown() {
    return (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
}

// What a page can ask for: the chrome commands and the window commands. The
// rest goes to the common dispatcher (WebBridge.cpp).
void OnPageMessage(PanelId id, const std::wstring& msg) {
    size_t p1 = msg.find(L'|');
    std::wstring cmd = msg.substr(0, p1);
    std::wstring rest = (p1 == std::wstring::npos) ? L"" : msg.substr(p1 + 1);
    if (HandlePanelMessage(id, cmd, rest)) return;
    HandleWebMessage(g_mainWnd, msg);
}

void CreatePanelWebView(PanelId id) {
    Panel& p = P(id);
    if (!g_webviewEnvironment || !p.hwnd || p.ctl || p.creating) return;
    p.creating = true;
    HWND hwnd = p.hwnd;
    g_webviewEnvironment->CreateCoreWebView2Controller(hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [id, hwnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                Panel& p = P(id);
                p.creating = false;
                // The window may have been closed while the controller was
                // being created.
                if (p.hwnd != hwnd) {
                    if (controller) controller->Close();
                    return S_OK;
                }
                if (FAILED(result) || !controller) {
                    PanelFailed(id);
                    return S_OK;
                }
                p.ctl = controller;
                // Kept in sync with the host window (a controller made for a
                // window never shown -- --tray launch -- can otherwise stay
                // blank the first time the window appears).
                controller->put_IsVisible(IsWindowVisible(hwnd));
                if (FAILED(controller->get_CoreWebView2(p.wv.GetAddressOf())) || !p.wv) {
                    PanelFailed(id);
                    return S_OK;
                }
                HardenWebView(p.wv.Get());

                // Same colour as the window frame behind it: no white flash
                // while the page paints or the window is resized.
                ComPtr<ICoreWebView2Controller2> controller2;
                if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(controller2.GetAddressOf())))) {
                    COREWEBVIEW2_COLOR bg = { 255, 0x22, 0x22, 0x22 };
                    controller2->put_DefaultBackgroundColor(bg);
                }

                p.wv->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [id](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR msg = nullptr;
                            if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                                OnPageMessage(id, msg);
                                CoTaskMemFree(msg);
                            }
                            return S_OK;
                        }).Get(), &p.tok);
                ResizeController(p);

                // The page learns which window it is (window.__panel) before any
                // script of its own runs; the navigation starts once that is
                // registered.
                std::wstring script = std::wstring(L"window.__panel='") + FromUtf8(Def(id).key) + L"';";
                p.wv->AddScriptToExecuteOnDocumentCreated(script.c_str(),
                    Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                        [id](HRESULT, LPCWSTR) -> HRESULT {
                            Panel& p = P(id);
                            if (!p.wv) return S_OK;
                            std::wstring html = LoadHtmlResource(Def(id).resId);
                            if (html.empty()) {
                                PanelFailed(id);
                                return S_OK;
                            }
                            p.wv->NavigateToString(html.c_str());
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void DestroyPanelWebView(Panel& p) {
    p.ready = false;
    p.pending.clear();
    if (p.wv) {
        p.wv->remove_WebMessageReceived(p.tok);
        p.wv.Reset();
    }
    if (p.ctl) {
        p.ctl->Close();
        p.ctl.Reset();
    }
}

void DestroyPanelWindow(PanelId id) {
    Panel& p = P(id);
    DestroyPanelWebView(p);
    p.creating = false;
    if (id == PanelId::Launcher) return; // its window is the app's main window
    if (p.hwnd) {
        HWND h = p.hwnd;
        p.hwnd = nullptr;
        DestroyWindow(h);
    }
}

void PanelFailed(PanelId id) {
    if (id == PanelId::Launcher) {
        WebViewFatalError(P(id).hwnd);
        return;
    }
    LOG_ERROR(std::wstring(L"La fenêtre « ") + FromUtf8(Def(id).key) + L" » n'a pas pu être créée.");
    P(id).wantOpen = false;
    DestroyPanelWindow(id);
    SendError(L"Une fenêtre n'a pas pu s'ouvrir (WebView2 a échoué). Voir le journal.");
}

LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    LONG_PTR tag = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (tag > 0) {
        PanelId id = static_cast<PanelId>(tag - 1);
        bool handled = false;
        LRESULT r = PanelCommonProc(id, hwnd, msg, wParam, lParam, handled);
        if (handled) return r;
        if (msg == WM_CLOSE) {
            ClosePanel(id);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RegisterPanelClass() {
    static bool done = false;
    if (done) return;
    done = true;
    g_panelBrush = CreateSolidBrush(RGB(0x22, 0x22, 0x22));
    WNDCLASSW wc = {};
    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kPanelClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_panelBrush;
    wc.hIcon = g_hAppIcon;
    RegisterClassW(&wc);
}

void CreatePanelWindow(PanelId id) {
    Panel& p = P(id);
    if (p.hwnd) return;
    RegisterPanelClass();
    RECT r = InitialRect(id);
    // Not owned by the launcher: an owned window always stays above its
    // owner, and every window must come to the front when it is clicked. The
    // launcher closes them itself when it goes to the tray. Tool window: no
    // taskbar button of its own.
    p.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kPanelClass, Def(id).title,
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
        r.left, r.top, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, GetModuleHandleW(nullptr),
        reinterpret_cast<LPVOID>(static_cast<intptr_t>(static_cast<int>(id) + 1)));
    if (!p.hwnd) return;
    ApplyFlatFrame(p.hwnd);
    auto it = g_cfg.panels.find(Def(id).key);
    if (it != g_cfg.panels.end() && it->second.shaded && Def(id).resizable) ApplyShade(id, true);
}

} // namespace

// --- Public API ---

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

void ApplyFlatFrame(HWND hwnd) {
    DWORD corner = 1; // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DWORD none = 0xFFFFFFFE; // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &none, sizeof(none));
}

const char* PanelKey(PanelId id) { return Def(id).key; }

bool PanelFromKey(const std::wstring& key, PanelId& out) {
    for (int i = 0; i < kPanelCount; ++i) {
        if (FromUtf8(kDefs[i].key) == key) {
            out = static_cast<PanelId>(i);
            return true;
        }
    }
    return false;
}

bool IsPanelOpen(PanelId id) {
    const Panel& p = P(id);
    return p.hwnd && IsWindowVisible(p.hwnd);
}

bool IsPanelShaded(PanelId id) { return P(id).shaded; }

bool IsLauncherHidden() { return g_trayHidden; }

bool AnyPanelVisible() {
    for (int i = 0; i < kPanelCount; ++i) {
        if (g_panels[i].hwnd && IsWindowVisible(g_panels[i].hwnd)) return true;
    }
    return false;
}

void AttachLauncherWindow(HWND hwnd, bool startHidden) {
    P(PanelId::Launcher).hwnd = hwnd;
    P(PanelId::Launcher).wantOpen = true;
    g_trayHidden = startHidden;
    // What was open last time (a first run opens the default windows).
    for (int i = 1; i < kPanelCount; ++i) {
        auto it = g_cfg.panels.find(kDefs[i].key);
        g_panels[i].wantOpen = (static_cast<PanelId>(i) == PanelId::Options) ? false
            : (it != g_cfg.panels.end() ? it->second.open : kDefs[i].defaultOpen);
    }
}

void InitPanelWebViewEnvironment(HWND launcher) {
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
            [launcher](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    WebViewFatalError(launcher);
                    return S_OK;
                }
                g_webviewEnvironment = env; // shared by every window
                CreatePanelWebView(PanelId::Launcher);
                if (!g_trayHidden) PanelsRestoreFromTray();
                return S_OK;
            }).Get());
    if (FAILED(hr)) WebViewFatalError(launcher);
}

void OpenPanel(PanelId id) {
    Panel& p = P(id);
    p.wantOpen = true;
    if (!g_webviewEnvironment) return; // opened once the environment exists
    if (id == PanelId::Launcher) return;
    if (g_trayHidden) return; // shown when the launcher comes back
    if (!p.hwnd) CreatePanelWindow(id);
    if (!p.hwnd) return;
    if (!p.ctl && !p.creating) CreatePanelWebView(id);
    ShowWindow(p.hwnd, SW_SHOW);
    ResizeController(p);
    if (p.ctl) p.ctl->put_IsVisible(TRUE);
    SetForegroundWindow(p.hwnd);
    SendState(); // every window learns which ones are open
}

void ClosePanel(PanelId id) {
    if (id == PanelId::Launcher) {
        HideLauncherToTray();
        return;
    }
    Panel& p = P(id);
    if (id == PanelId::Options) StopHotkeyCapture();
    if (id == PanelId::Outputs) {
        // Its page can no longer tell the engine to stop: the mic meters keep
        // microphone streams open (Windows shows "microphone in use") and the
        // spectrum is computed for a speaker nobody sees.
        StopMicMetering();
        g_engine.setSpectrumTarget(L"");
    }
    p.wantOpen = false;
    SavePanelPlacements(); // the rect is read while the window still exists
    DestroyPanelWindow(id);
    if (g_mainWnd && IsWindowVisible(g_mainWnd)) SetForegroundWindow(g_mainWnd);
    SendState();
}

void TogglePanel(PanelId id) {
    if (IsPanelOpen(id)) ClosePanel(id);
    else OpenPanel(id);
}

void PostToPanel(PanelId id, const std::wstring& jsonText) {
    Panel& p = P(id);
    if (!p.wv || !p.hwnd || !IsWindowVisible(p.hwnd)) return;
    if (!p.ready) return; // the page asks for the state itself once it is up
    p.wv->PostWebMessageAsJson(jsonText.c_str());
}

void PostToPanels(const std::wstring& jsonText) {
    for (int i = 0; i < kPanelCount; ++i) PostToPanel(static_cast<PanelId>(i), jsonText);
}

// Transient messages (toasts) go to one window only, not to all of them: the
// one being used, else the biggest one that is open.
bool PostToastTarget(const std::wstring& jsonText) {
    auto usable = [](PanelId id) {
        const Panel& p = P(id);
        return p.wv && p.ready && p.hwnd && IsWindowVisible(p.hwnd) && !p.shaded;
    };
    // Same order as isToaster() in ui.html: the pages that raise toasts on
    // their own must pick the same window, or a message shows twice.
    const PanelId order[] = { PanelId::Outputs, PanelId::Setups, PanelId::Journal, PanelId::Levels,
                              PanelId::Options, PanelId::Launcher };
    for (PanelId id : order) {
        if (usable(id)) {
            PostToPanel(id, jsonText);
            return true;
        }
    }
    return false;
}

void SavePanelPlacements() {
    for (int i = 1; i < kPanelCount; ++i) {
        Panel& p = g_panels[i];
        PanelPlacement& pp = g_cfg.panels[kDefs[i].key];
        if (p.hwnd) {
            WINDOWPLACEMENT wp = { sizeof(wp) };
            if (GetWindowPlacement(p.hwnd, &wp)) {
                const RECT& r = wp.rcNormalPosition;
                if (r.right > r.left && r.bottom > r.top) {
                    pp.x = r.left;
                    pp.y = r.top;
                    pp.w = r.right - r.left;
                    pp.h = (p.shaded && p.fullH > 0) ? p.fullH : r.bottom - r.top;
                }
            }
        }
        pp.open = p.wantOpen && static_cast<PanelId>(i) != PanelId::Options;
        pp.shaded = p.shaded;
    }
    SaveConfig();
}

void PanelsHideToTray() {
    SavePanelPlacements();
    g_trayHidden = true;
    for (int i = 1; i < kPanelCount; ++i) {
        if (g_panels[i].hwnd) DestroyPanelWindow(static_cast<PanelId>(i)); // wantOpen kept
    }
}

void PanelsRestoreFromTray() {
    g_trayHidden = false;
    for (int i = 1; i < kPanelCount; ++i) {
        if (g_panels[i].wantOpen) OpenPanel(static_cast<PanelId>(i));
    }
}

void DestroyAllPanels() {
    for (int i = kPanelCount - 1; i >= 0; --i) DestroyPanelWindow(static_cast<PanelId>(i));
}

// The launcher's window is created by the app itself (RunApplication); this
// is the same suspend/resume the old single window used while hidden: a
// merely invisible WebView2 keeps its whole render tree warm.
void SuspendLauncherPage() {
    // The page posts micmeter|0 and spectrum| from its visibilitychange
    // handler, but suspension can win that race -- stop metering here too so
    // the Windows "microphone in use" indicator never stays lit with the
    // window hidden, and nobody computes a spectrum nobody can see.
    StopMicMetering();
    g_engine.setSpectrumTarget(L"");
    Panel& p = P(PanelId::Launcher);
    if (!p.ctl || !p.wv) return;
    p.ctl->put_IsVisible(FALSE); // TrySuspend requires hidden
    ComPtr<ICoreWebView2_19> wv19;
    if (SUCCEEDED(p.wv.As(&wv19)) && wv19) {
        wv19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
    }
    ComPtr<ICoreWebView2_3> wv3;
    if (SUCCEEDED(p.wv.As(&wv3)) && wv3) {
        wv3->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>(
            [](HRESULT, BOOL) -> HRESULT { return S_OK; }).Get());
    }
}

void ResumeLauncherPage() {
    Panel& p = P(PanelId::Launcher);
    if (!p.ctl || !p.wv) return;
    ComPtr<ICoreWebView2_3> wv3;
    BOOL suspended = FALSE;
    if (SUCCEEDED(p.wv.As(&wv3)) && wv3 &&
        SUCCEEDED(wv3->get_IsSuspended(&suspended)) && suspended) {
        wv3->Resume();
    }
    ComPtr<ICoreWebView2_19> wv19;
    if (SUCCEEDED(p.wv.As(&wv19)) && wv19) {
        wv19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
    }
    p.ctl->put_IsVisible(TRUE);
}

void HideLauncherToTray() {
    if (!g_mainWnd) return;
    SaveWindowPlacement(g_mainWnd);
    PanelsHideToTray();
    ShowWindow(g_mainWnd, SW_HIDE);
    SuspendLauncherPage();
}

void ShowLauncherFromTray() {
    HWND hwnd = g_mainWnd;
    if (!hwnd) return;
    g_trayHidden = false;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    ResumeLauncherPage();
    PanelsRestoreFromTray();
}

bool HandlePanelMessage(PanelId from, const std::wstring& cmd, const std::wstring& rest) {
    Panel& p = P(from);
    if (cmd == L"ready") {
        p.ready = true;
        SendActive(from, p.hwnd && GetForegroundWindow() == p.hwnd);
        if (p.shaded) PostToPanel(from, L"{\"type\":\"shade\",\"value\":true}");
        if (from == PanelId::Launcher) {
            // First-run work (autostart...) and the state itself live in the common handler.
            for (const auto& m : p.pending) {
                if (p.wv) p.wv->PostWebMessageAsJson(m.c_str());
            }
            p.pending.clear();
            FlushPendingToasts();
            return false;
        }
        SendStateTo(from);
        if (from == PanelId::Journal) SendJournal(from);
        // Commands queued while the window was opening (switch to the inputs
        // tab, open the calibration...) need the state they act on.
        for (const auto& m : p.pending) {
            if (p.wv) p.wv->PostWebMessageAsJson(m.c_str());
        }
        p.pending.clear();
        FlushPendingToasts();
        return true;
    }
    if (cmd == L"dragstart") {
        g_drag.active = false;
        // Only while the button is still down (the message crosses a process
        // boundary: a quick click can be over by the time it arrives).
        if (!p.hwnd || !PrimaryButtonDown()) return true;
        g_drag.active = true;
        g_drag.id = from;
        GetCursorPos(&g_drag.cursor0);
        GetWindowRect(p.hwnd, &g_drag.rect0);
        return true;
    }
    if (cmd == L"dragmove") {
        if (!g_drag.active || g_drag.id != from || !p.hwnd) return true;
        if (!PrimaryButtonDown()) { // release missed: stop following the cursor
            g_drag.active = false;
            return true;
        }
        POINT pt;
        GetCursorPos(&pt);
        SetWindowPos(p.hwnd, nullptr, g_drag.rect0.left + (pt.x - g_drag.cursor0.x),
                     g_drag.rect0.top + (pt.y - g_drag.cursor0.y), 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }
    if (cmd == L"dragend") {
        if (g_drag.active && g_drag.id == from) {
            g_drag.active = false;
            if (from == PanelId::Launcher) SaveWindowPlacement(p.hwnd);
            else SavePanelPlacements();
        }
        return true;
    }
    if (cmd == L"resizewin") {
        if (!p.hwnd || !Def(from).resizable || p.shaded || !PrimaryButtonDown()) return true;
        ReleaseCapture();
        POINT pt;
        GetCursorPos(&pt);
        PostMessageW(p.hwnd, WM_NCLBUTTONDOWN, HTBOTTOMRIGHT, MAKELPARAM(pt.x, pt.y));
        return true;
    }
    if (cmd == L"shade") {
        ShadePanel(from, rest == L"1" ? 1 : rest == L"0" ? 0 : -1);
        return true;
    }
    if (cmd == L"quit") {
        PostMessageW(g_mainWnd, WM_COMMAND, ID_TRAY_QUIT, 0); // same path as the tray's Quitter
        return true;
    }
    if (cmd == L"journalclear") {
        JournalClear();
        return true;
    }
    if (cmd == L"closepanel") {
        ClosePanel(from);
        return true;
    }
    if (cmd == L"panel") {
        // panel|<key>|open / close / toggle [|<queued page command>]
        auto parts = SplitPipes(rest);
        PanelId target;
        if (parts.size() < 2 || !PanelFromKey(parts[0], target)) return true;
        if (parts[1] == L"open") OpenPanel(target);
        else if (parts[1] == L"close") ClosePanel(target);
        else TogglePanel(target);
        // A page command for the target (e.g. open the inputs tab), delivered
        // as soon as that page is ready.
        if (parts.size() >= 3 && !parts[2].empty() && IsPanelOpen(target)) {
            Panel& t = P(target);
            if (t.ready && t.wv) t.wv->PostWebMessageAsJson(parts[2].c_str());
            else t.pending.push_back(parts[2]);
        }
        return true;
    }
    return false;
}

void ShadePanel(PanelId id, int mode) {
    Panel& p = P(id);
    if (!p.hwnd || !Def(id).resizable) return;
    bool want = mode < 0 ? !p.shaded : mode == 1;
    if (want == p.shaded) return;
    ApplyShade(id, want);
    PostToPanel(id, std::wstring(L"{\"type\":\"shade\",\"value\":") + (want ? L"true" : L"false") + L"}");
    SavePanelPlacements();
    SendState(); // the other windows need to know (where a toast can appear)
}

// Window messages common to the launcher and the other windows (the
// launcher's own procedure, in Gui.cpp, forwards them here first).
LRESULT PanelCommonProc(PanelId id, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool& handled) {
    handled = true;
    Panel& p = P(id);
    switch (msg) {
        case WM_NCCALCSIZE:
            return 0; // no non-client area at all: the page draws the frame
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) break;
            ResizeController(p);
            if (p.ctl) p.ctl->put_IsVisible(IsWindowVisible(hwnd));
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            UINT dpi = DpiFor(hwnd);
            const PanelDef& d = Def(id);
            mmi->ptMinTrackSize.x = MulDiv(d.minW, static_cast<int>(dpi), 96);
            mmi->ptMinTrackSize.y = MulDiv(p.shaded ? kShadeCssPx : d.minH, static_cast<int>(dpi), 96);
            if (!d.resizable) {
                mmi->ptMaxTrackSize.x = mmi->ptMinTrackSize.x;
                mmi->ptMaxTrackSize.y = mmi->ptMinTrackSize.y;
            } else if (p.shaded) {
                mmi->ptMaxTrackSize.y = mmi->ptMinTrackSize.y; // a rolled-up window only resizes sideways
            }
            return 0;
        }
        case WM_DPICHANGED: {
            // Moved to a monitor with another scale: adopt the size/position
            // Windows computed. WebView2 re-rasterizes on its own.
            auto* sr = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, sr->left, sr->top, sr->right - sr->left, sr->bottom - sr->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SETFOCUS:
            // The host window has nothing focusable: hand focus to the page.
            if (p.ctl) p.ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            return 0;
        case WM_ACTIVATE:
            SendActive(id, LOWORD(wParam) != WA_INACTIVE);
            // Alt-Tab away mid-capture: the hook would keep eating every
            // keystroke for whatever app is now in front.
            if (id == PanelId::Options && LOWORD(wParam) == WA_INACTIVE && g_hotkeyCaptureId) {
                StopHotkeyCapture();
                SendState();
            }
            handled = false; // the default handling still has to run
            return 0;
        case WM_EXITSIZEMOVE:
            if (id == PanelId::Launcher) SaveWindowPlacement(hwnd);
            else SavePanelPlacements();
            return 0;
        default:
            break;
    }
    handled = false;
    return 0;
}

} // namespace al
