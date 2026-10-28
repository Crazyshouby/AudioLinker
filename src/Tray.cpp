// Notification-area icon, its menu and balloons, and restoring the main
// window from it.

#include "App.h"

#include <cmath>
#include <cstdint>

namespace al {

// --- Tray icon ---

void AddTrayIcon(HWND hwnd) {
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = hwnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon = g_hAppIcon ? g_hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayIcon.szTip, L"AudioLinker — Groupe d'enceintes");
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
}

// Balloon/toast notification from the tray icon -- the only channel left
// when the window is hidden. iconFlag: NIIF_INFO / NIIF_WARNING / NIIF_ERROR.
void ShowTrayBalloon(const std::wstring& text, DWORD iconFlag) {
    if (!g_trayIcon.hWnd) return;
    NOTIFYICONDATAW nid = g_trayIcon;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = iconFlag;
    wcscpy_s(nid.szInfoTitle, L"AudioLinker");
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// The "group active" variant of the app icon: the base icon with a small
// green dot (white ring) in the bottom-right corner, built once into a
// 32-bit DIB so both alphas survive. Returns null if anything fails -- the
// caller then just keeps the plain icon.
HICON MakeActiveIcon(HICON base) {
    const int sz = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sz;
    bi.bV5Height = -sz; // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!color || !mem || !bits) {
        if (color) DeleteObject(color);
        if (mem) DeleteDC(mem);
        return nullptr;
    }
    HGDIOBJ old = SelectObject(mem, color);
    DrawIconEx(mem, 0, 0, base, sz, sz, 0, nullptr, DI_NORMAL);
    SelectObject(mem, old);
    DeleteDC(mem);

    // Dot: radius 6.5 centred at (24.5, 24.5), 1.5px white ring. Pixels are
    // written directly (premultiplied BGRA) so the alpha is exact.
    auto* px = static_cast<uint32_t*>(bits);
    const double cx = 24.5, cy = 24.5, rOuter = 7.0, rInner = 5.5;
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            double d = std::sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy));
            if (d > rOuter) continue;
            uint32_t rgb = d <= rInner ? 0x003DDC84 /* --green */ : 0x00FFFFFF;
            px[y * sz + x] = 0xFF000000 | rgb;
        }
    }

    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}

void UpdateTrayState() {
    if (!g_trayIcon.hWnd) return;
    static HICON activeIcon = nullptr;
    static bool activeIconTried = false;
    static bool lastActive = false;
    static std::wstring lastTip;

    bool active = g_engine.isRunning();
    std::wstring tip = active
        ? L"AudioLinker — " + (g_playingSetup.empty() ? std::wstring(L"groupe actif") : g_playingSetup)
        : L"AudioLinker — groupe arrêté";
    if (tip.size() > 120) tip.resize(120);
    if (active == lastActive && tip == lastTip) return;
    if (active && !activeIconTried) {
        activeIconTried = true;
        if (g_hAppIcon) activeIcon = MakeActiveIcon(g_hAppIcon);
    }
    NOTIFYICONDATAW nid = g_trayIcon;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = (active && activeIcon) ? activeIcon : g_trayIcon.hIcon;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    lastActive = active;
    lastTip = tip;
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Afficher AudioLinker");
    std::wstring toggleLabel = (g_engine.isRunning() ? L"Arrêter le groupe\t" : L"Démarrer le groupe\t") +
                               FormatHotkey(g_toggleHotkeyMods, g_toggleHotkeyVk);
    AppendMenuW(menu, MF_STRING, ID_TRAY_TOGGLE, toggleLabel.c_str());
    if (!g_setups.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        // Section header: a disabled entry, purely a label.
        std::wstring setupsHeader = L"Setups\t" + FormatHotkey(g_switcherHotkeyMods, g_switcherHotkeyVk);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, setupsHeader.c_str());
        int count = std::min(static_cast<int>(g_setups.size()), kMaxTraySetups);
        for (int i = 0; i < count; ++i) {
            UINT flags = MF_STRING;
            if (g_setups[i].name == g_activeSetup) flags |= MF_CHECKED;
            AppendMenuW(menu, flags, ID_TRAY_SETUP_BASE + i, g_setups[i].name.c_str());
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quitter");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

void RestoreFromTray(HWND /*hwnd*/) {
    ShowLauncherFromTray(); // the launcher, then the windows that were open
    // Everything that changed while the windows were hidden (setup switched
    // by hotkey, device hot-plugs...) was not posted to any page -- see
    // PostJson -- so re-sync it now that someone can see it again, then
    // replay the toasts they missed.
    SendState();
    for (const auto& j : g_pendingJson) PostToastTarget(j);
    g_pendingJson.clear();
}

} // namespace al
