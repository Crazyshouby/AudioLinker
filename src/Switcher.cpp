// Setup switcher popup (Ctrl+Alt+S): a standalone topmost GDI window with
// a system-wide mouse-wheel hook while it is up.

#include "App.h"

namespace al {

// --- Setup switcher popup (Ctrl+Alt+S) ---

// ensureRunning: force the group to (re)start even if it wasn't already
// running -- used by the setup row's dedicated play button. Other callers
// (hotkeys, tray menu, switcher popup) leave it false: a quick preset switch
// shouldn't start audio that wasn't already playing.

void CloseSwitcherHook() {
    if (g_switcherWheelHook) {
        UnhookWindowsHookEx(g_switcherWheelHook);
        g_switcherWheelHook = nullptr;
    }
}

namespace switcher {

// Mirrors the web UI (common.css): --panel, --frame/--red-frame, --text,
// --muted, --faint. Single dark theme.
struct Palette { COLORREF bg, border, text, muted, faint; };

Palette CurrentPalette() {
    return { RGB(0x33, 0x33, 0x33), RGB(0x88, 0x00, 0x00), RGB(0xFF, 0xFF, 0xFF),
             RGB(0xDD, 0xDD, 0xDD), RGB(0xA7, 0xAF, 0xB2) };
}

int g_index = -1;  // selected row
int g_scroll = 0;  // first visible row

struct Metrics {
    UINT dpi;
    int w, h, padX, padTop, padBottom, titleH, rowH, rowGap, hintH, visRows;
};

Metrics MetricsFor(HWND wnd) {
    Metrics m{};
    m.dpi = GetDpiForWindow(wnd);
    if (m.dpi == 0) m.dpi = 96;
    auto s = [&](int v) { return MulDiv(v, static_cast<int>(m.dpi), 96); };
    m.w = s(440);
    m.padX = s(16);
    m.padTop = s(12);
    m.padBottom = s(8);
    m.titleH = s(32);
    m.rowH = s(46);
    m.rowGap = s(4);
    m.hintH = s(30);
    m.visRows = std::min(8, std::max(1, static_cast<int>(g_setups.size())));
    m.h = m.padTop + m.titleH + m.visRows * (m.rowH + m.rowGap) + m.hintH + m.padBottom;
    return m;
}

int RowAt(HWND wnd, int x, int y) {
    Metrics m = MetricsFor(wnd);
    if (x < m.padX || x > m.w - m.padX) return -1;
    int top = m.padTop + m.titleH;
    if (y < top) return -1;
    int idx = g_scroll + (y - top) / (m.rowH + m.rowGap);
    if (idx >= static_cast<int>(g_setups.size()) || idx >= g_scroll + m.visRows) return -1;
    return idx;
}

void Move(int dir) {
    if (!g_switcherWnd || g_setups.empty()) return;
    int n = static_cast<int>(g_setups.size());
    g_index = ((g_index < 0 ? 0 : g_index) + dir % n + n) % n;
    Metrics m = MetricsFor(g_switcherWnd);
    if (g_index < g_scroll) g_scroll = g_index;
    if (g_index >= g_scroll + m.visRows) g_scroll = g_index - m.visRows + 1;
    InvalidateRect(g_switcherWnd, nullptr, FALSE);
}

// Hide first, apply after: the popup must vanish instantly even though
// applying may restart the whole engine.
void Close(bool apply) {
    static bool closing = false;
    if (closing || !g_switcherWnd || !IsWindowVisible(g_switcherWnd)) return;
    closing = true;
    std::wstring name;
    if (apply && g_index >= 0 && g_index < static_cast<int>(g_setups.size())) {
        name = g_setups[g_index].name;
    }
    ShowWindow(g_switcherWnd, SW_HIDE);
    CloseSwitcherHook();
    closing = false;
    if (!name.empty()) ApplySetup(g_mainWnd, name);
}

void Paint(HWND wnd, HDC target) {
    RECT rc;
    GetClientRect(wnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    Metrics m = MetricsFor(wnd);
    Palette pal = CurrentPalette();
    auto s = [&](int v) { return MulDiv(v, static_cast<int>(m.dpi), 96); };

    HDC hdc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, w, h);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(hdc, bmp));

    HBRUSH bg = CreateSolidBrush(pal.bg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    HBRUSH frame = CreateSolidBrush(pal.border);
    FrameRect(hdc, &rc, frame);
    DeleteObject(frame);

    HFONT fontTitle = CreateFontW(-s(11), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT fontRow = CreateFontW(-s(15), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT fontSmall = CreateFontW(-s(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SetBkMode(hdc, TRANSPARENT);

    auto drawText = [&](const std::wstring& text, RECT r, COLORREF color, HFONT font, UINT fmt) {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
        SetTextColor(hdc, color);
        DrawTextW(hdc, text.c_str(), -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, old);
    };

    RECT titleRc = { m.padX, m.padTop, w - m.padX, m.padTop + m.titleH };
    drawText(L"CHANGER DE SETUP", titleRc, pal.muted, fontTitle, DT_LEFT | DT_VCENTER);

    if (g_setups.empty()) {
        RECT er = { m.padX, m.padTop + m.titleH, w - m.padX, h - m.hintH - m.padBottom };
        drawText(L"Aucun setup enregistré.", er, pal.faint, fontRow, DT_CENTER | DT_VCENTER);
    }

    int last = std::min(static_cast<int>(g_setups.size()), g_scroll + m.visRows);
    for (int i = g_scroll; i < last; ++i) {
        int top = m.padTop + m.titleH + (i - g_scroll) * (m.rowH + m.rowGap);
        RECT row = { m.padX, top, w - m.padX, top + m.rowH };
        bool cur = (i == g_index);
        if (cur) {
            // KDX selected row: navy, square corners.
            HBRUSH b = CreateSolidBrush(RGB(0x00, 0x11, 0x77));
            FillRect(hdc, &row, b);
            DeleteObject(b);
        }
        size_t checkedCount = 0;
        for (const auto& [id, os] : g_setups[i].devices) {
            if (os.checked) ++checkedCount;
        }
        std::wstring cnt = std::to_wstring(checkedCount) +
                           (checkedCount > 1 ? L" enceintes" : L" enceinte");
        RECT nameRc = { row.left + s(14), row.top, row.right - s(110), row.bottom };
        RECT cntRc = { row.right - s(106), row.top, row.right - s(14), row.bottom };
        drawText(g_setups[i].name, nameRc, cur ? RGB(0xFF, 0x00, 0xFF) : pal.text, fontRow,
                 DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        drawText(cnt, cntRc, cur ? RGB(0xFF, 0x00, 0xFF) : pal.faint, fontSmall, DT_RIGHT | DT_VCENTER);
    }

    RECT hintRc = { m.padX, h - m.hintH - m.padBottom, w - m.padX, h - m.padBottom };
    drawText(L"Molette : choisir   ·   Entrée : appliquer   ·   Échap : annuler",
             hintRc, pal.faint, fontSmall, DT_CENTER | DT_VCENTER);

    DeleteObject(fontTitle);
    DeleteObject(fontRow);
    DeleteObject(fontSmall);

    BitBlt(target, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

LRESULT CALLBACK Proc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(wnd, &ps);
            Paint(wnd, hdc);
            EndPaint(wnd, &ps);
            return 0;
        }
        case WM_MOUSEWHEEL:
            // Fallback only: while the low-level hook is installed it
            // swallows wheel input before it ever reaches this window.
            Move(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
            return 0;
        case WM_MOUSEMOVE: {
            int r = RowAt(wnd, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            if (r >= 0 && r != g_index) {
                g_index = r;
                InvalidateRect(wnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            int r = RowAt(wnd, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            if (r >= 0) {
                g_index = r;
                Close(true);
            }
            return 0;
        }
        case WM_KEYDOWN:
            switch (wParam) {
                case VK_RETURN: Close(true); return 0;
                case VK_ESCAPE: Close(false); return 0;
                case VK_DOWN: Move(1); return 0;
                case VK_UP: Move(-1); return 0;
            }
            return 0;
        case WM_ACTIVATE:
            // Clicking elsewhere / Alt-Tab cancels; an invisible popup must
            // never keep the system-wide wheel hook alive.
            if (LOWORD(wParam) == WA_INACTIVE) Close(false);
            return 0;
    }
    return DefWindowProcW(wnd, msg, wParam, lParam);
}

LRESULT CALLBACK WheelHookProc(int code, WPARAM wParam, LPARAM lParam) {
    // Low-level hooks run on the installing thread (this GUI thread, during
    // message dispatch), so touching the popup from here is safe.
    if (code == HC_ACTION && wParam == WM_MOUSEWHEEL) {
        auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        short delta = static_cast<short>(HIWORD(info->mouseData));
        Move(delta > 0 ? -1 : 1);
        return 1; // swallowed: nothing else may scroll while the switcher is up
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void Open() {
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    if (!g_switcherWnd) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = Proc;
            wc.hInstance = hinst;
            wc.lpszClassName = L"AudioLinkerSwitcher";
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            registered = true;
        }
        // WS_EX_TOOLWINDOW: no taskbar button, it's a transient OSD.
        g_switcherWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"AudioLinkerSwitcher",
                                        L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hinst, nullptr);
        if (!g_switcherWnd) return;
        // Win11 rounds the corners for us; older systems just get a rectangle.
        DWORD corner = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(g_switcherWnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                              &corner, sizeof(corner));
    }

    g_index = -1;
    for (size_t i = 0; i < g_setups.size(); ++i) {
        if (g_setups[i].name == g_activeSetup) g_index = static_cast<int>(i);
    }
    if (g_index < 0 && !g_setups.empty()) g_index = 0;
    g_scroll = 0;
    Metrics m = MetricsFor(g_switcherWnd);
    if (g_index >= m.visRows) g_scroll = g_index - m.visRows + 1;

    // Centered (slightly high) on the monitor the cursor is on.
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - m.w) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - m.h) * 2 / 5;

    SetWindowPos(g_switcherWnd, HWND_TOPMOST, x, y, m.w, m.h, SWP_SHOWWINDOW);
    SetForegroundWindow(g_switcherWnd); // hotkey handling grants us foreground rights
    InvalidateRect(g_switcherWnd, nullptr, FALSE);

    if (!g_switcherWheelHook) {
        g_switcherWheelHook = SetWindowsHookExW(WH_MOUSE_LL, WheelHookProc, hinst, 0);
    }
}

} // namespace switcher

} // namespace al
