// Notification-area icon, its menu and balloons, and restoring the main
// window from it.

#include "App.h"

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


void RestoreFromTray(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    ResumeMainWebView();
    // Everything that changed while the window was hidden (setup switched by
    // hotkey, device hot-plugs...) was not posted to the page -- see
    // PostJson -- so re-sync it now that someone can see it again, then
    // replay the toasts it missed.
    SendState();
    if (g_webview) {
        for (const auto& j : g_pendingJson) g_webview->PostWebMessageAsJson(j.c_str());
    }
    g_pendingJson.clear();
}

} // namespace al
