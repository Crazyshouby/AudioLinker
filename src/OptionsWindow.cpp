// The Options window: a separate modal top-level window hosting its own
// WebView2 controller with options.html.

#include "App.h"

namespace al {

void ApplyOptionsWindowTheme() {
    if (!g_optionsWnd) return;
    HBRUSH newBrush = CreateSolidBrush(g_windowBg);
    SetClassLongPtrW(g_optionsWnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(newBrush));
    if (g_optionsHBrushWindowBg) DeleteObject(g_optionsHBrushWindowBg);
    g_optionsHBrushWindowBg = newBrush;

    BOOL darkMode = EffectiveLightTheme() ? FALSE : TRUE;
    DwmSetWindowAttribute(g_optionsWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    InvalidateRect(g_optionsWnd, nullptr, TRUE);
}

// Setup switcher (Ctrl+Alt+S): a standalone borderless always-on-top popup,
// drawn natively -- it works with the main window minimized or in the tray,
// which stays untouched. While it is up, a low-level mouse hook captures the
// wheel system-wide (the cursor can be anywhere on screen) and swallows it
// so the app under the cursor doesn't scroll.

// --- Options window (real, separate, modal top-level window -- not an
// in-page overlay, so it has its own title bar and can be moved/resized
// independently of the main window) ---

void CloseOptionsWindow() {
    // Safety net regardless of whether options.html's own JS got a chance to
    // post its own cancel first (e.g. Alt+F4, or the title bar's X, while a
    // hotkey capture is in progress).
    StopHotkeyCapture();
    if (g_optionsController) {
        g_optionsController->Close();
        g_optionsController.Reset();
    }
    g_optionsWebview.Reset();
    if (g_mainWnd) {
        EnableWindow(g_mainWnd, TRUE);
        SetForegroundWindow(g_mainWnd);
    }
    if (g_optionsWnd) DestroyWindow(g_optionsWnd);
}

LRESULT CALLBACK OptionsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            if (g_optionsController) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                g_optionsController->put_Bounds(rc);
            }
            return 0;
        case WM_SETFOCUS:
            if (g_optionsController) {
                g_optionsController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
            return 0;
        case WM_DPICHANGED: {
            auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ACTIVATE:
            // Alt-Tab away mid-capture: the hook would keep eating every
            // keystroke for whatever app is now in front.
            if (LOWORD(wParam) == WA_INACTIVE && g_hotkeyCaptureId) {
                StopHotkeyCapture();
                SendState();
            }
            return 0;
        case WM_CLOSE:
            CloseOptionsWindow();
            return 0;
        case WM_DESTROY:
            g_optionsWnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void InitOptionsWebView(HWND hwnd) {
    g_webviewEnvironment->CreateCoreWebView2Controller(hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [hwnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if (FAILED(result) || !controller) {
                    MessageBoxW(hwnd, L"Impossible d'initialiser la fenêtre Options.",
                                L"AudioLinker", MB_ICONERROR);
                    return S_OK;
                }
                g_optionsController = controller;
                controller->put_IsVisible(TRUE);
                if (FAILED(controller->get_CoreWebView2(g_optionsWebview.GetAddressOf())) ||
                    !g_optionsWebview) {
                    return S_OK;
                }

                ComPtr<ICoreWebView2Settings> settings;
                if (SUCCEEDED(g_optionsWebview->get_Settings(settings.GetAddressOf())) && settings) {
                    settings->put_AreDefaultContextMenusEnabled(FALSE);
                    settings->put_IsStatusBarEnabled(FALSE);
                    settings->put_IsZoomControlEnabled(FALSE);
                }

                g_optionsWebview->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [hwnd](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR msg = nullptr;
                            if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                                // Same dispatcher as the main window: every command
                                // options.html actually sends (autostart,
                                // launchwindows, capturehotkey, cancelhotkeycapture,
                                // cushion, cushioncommit, ready) behaves the same
                                // regardless of which window's hwnd is passed in.
                                HandleWebMessage(hwnd, msg);
                                CoTaskMemFree(msg);
                            }
                            return S_OK;
                        }).Get(), &g_optionsMessageToken);

                RECT rc;
                GetClientRect(hwnd, &rc);
                controller->put_Bounds(rc);

                std::wstring html = LoadHtmlResource(IDR_OPTIONS_HTML);
                if (!html.empty()) g_optionsWebview->NavigateToString(html.c_str());
                return S_OK;
            }).Get());
}

void OpenOptionsWindow(HWND owner) {
    if (g_optionsWnd) {
        SetForegroundWindow(g_optionsWnd);
        return;
    }
    // Opening Options requires the main page to already be interactive
    // (that's where the button lives), which requires its own environment to
    // already exist -- this should never actually be null here.
    if (!g_webviewEnvironment) return;

    const wchar_t* className = L"AudioLinkerOptionsWindow";
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = OptionsWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // Stock system brush as a placeholder -- never needs freeing and is
        // always valid, unlike sharing g_hBrushWindowBg's handle here (which
        // ApplyWindowTheme can later DeleteObject() out from under it since
        // this is a separate window class). ApplyOptionsWindowTheme() below
        // replaces it with a properly-tracked brush before anything paints.
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.hIcon = g_hAppIcon;
        RegisterClassW(&wc);
        classRegistered = true;
    }

    // Sized for the full settings list (hotkeys, the two latency sliders,
    // low-latency toggle, optimizer, log, theme, import/export) -- if a
    // future row pushes past this again, options.html scrolls (#app has
    // overflow-y:auto) instead of clipping.
    constexpr int kOptionsW = 520, kOptionsH = 600;
    UINT dpi = GetDpiForWindow(owner);
    if (dpi == 0) dpi = 96;
    RECT r = { 0, 0, MulDiv(kOptionsW, static_cast<int>(dpi), 96),
                     MulDiv(kOptionsH, static_cast<int>(dpi), 96) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);

    RECT ownerRect;
    GetWindowRect(owner, &ownerRect);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - (r.right - r.left)) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - (r.bottom - r.top)) / 2;

    // No WS_MINIMIZEBOX/WS_MAXIMIZEBOX: a small settings dialog, not a
    // resizable app window. Owned by the main window (not WS_CHILD, just the
    // owner param) so it stays above it, closes/minimizes with it, and
    // doesn't get its own taskbar button.
    g_optionsWnd = CreateWindowExW(0, className, L"Options — AudioLinker",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, r.right - r.left, r.bottom - r.top,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_optionsWnd) return;

    ApplyOptionsWindowTheme();

    EnableWindow(owner, FALSE); // modal: main window stays visible but unresponsive until this closes
    ShowWindow(g_optionsWnd, SW_SHOW);
    UpdateWindow(g_optionsWnd);
    InitOptionsWebView(g_optionsWnd);
}

} // namespace al
