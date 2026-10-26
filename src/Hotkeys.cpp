// Global hotkeys: formatting, (re)registration, and the native capture of
// a new combo through a low-level keyboard hook.

#include "App.h"

namespace al {

// --- Global hotkeys (customizable) ---

std::wstring FormatHotkey(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Maj+";
    if (mods & MOD_WIN) s += L"Win+";
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        s += static_cast<wchar_t>(vk);
    } else {
        wchar_t buf[8];
        swprintf(buf, 8, L"%02X", vk);
        s += buf;
    }
    return s;
}

// Ctrl+Maj+1..9 are only registered while they can do something (option on
// AND at least one setup) -- a global hotkey that steals a combo from every
// other app just to do nothing is the worst of both worlds. Re-run after
// any change to the setup list or the option.
void UpdateSetupDigitHotkeys(HWND hwnd) {
    for (int i = 0; i < kSetupHotkeyCount; ++i) UnregisterHotKey(hwnd, HOTKEY_SETUP_FIRST + i);
    if (!g_setupDigitHotkeys || g_setups.empty()) return;
    int count = std::min(static_cast<int>(g_setups.size()), kSetupHotkeyCount);
    for (int i = 0; i < count; ++i) {
        RegisterHotKey(hwnd, HOTKEY_SETUP_FIRST + i, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
                       static_cast<UINT>('1' + i));
    }
}

// Swaps a live global hotkey for a new combo, restoring the previous one if
// the new combo is already taken (by another app, or by this app's other
// hotkey) so the app never ends up with a silently dead binding.
bool RebindHotkey(HWND hwnd, int id, UINT& modsVar, UINT& vkVar, UINT newMods, UINT newVk) {
    UINT oldMods = modsVar, oldVk = vkVar;
    UnregisterHotKey(hwnd, id);
    if (!RegisterHotKey(hwnd, id, newMods | MOD_NOREPEAT, newVk)) {
        RegisterHotKey(hwnd, id, oldMods | MOD_NOREPEAT, oldVk);
        return false;
    }
    modsVar = newMods;
    vkVar = newVk;
    return true;
}


// --- Hotkey capture (native) ---
//
// Capturing the combo via a DOM keydown listener in ui.html was unreliable:
// WebView2/Chromium reserves a bunch of Ctrl(+Shift)+<letter> and F-key
// combos as its own browser accelerators (find, print, devtools...) and
// never delivers those to the page's JS at all, so pressing one while
// "listening" silently did nothing. A low-level keyboard hook -- the same
// technique already used for the switcher popup's wheel handling -- sees
// every keystroke system-wide before any window (including WebView2's
// internal accelerator table) gets a chance to swallow it.
// Modifier state tracked purely from what the hook itself observes (see
// HotkeyCaptureHookProc), NOT GetAsyncKeyState/GetKeyState/LLKHF_ALTDOWN --
// a debug log proved all three report "not pressed" for every modifier the
// whole time it was actually held and auto-repeating (reproducible on this
// machine, cause unclear -- possibly a UIPI/session quirk), even though the
// hook correctly sees every keydown's real vkCode. Since the hook sees each
// modifier's own down/up transitions directly, tracking them ourselves
// sidesteps that unreliable API entirely.
bool g_hkCtrlDown = false, g_hkAltDown = false, g_hkShiftDown = false, g_hkWinDown = false;

void StopHotkeyCapture() {
    if (g_hotkeyCaptureHook) {
        UnhookWindowsHookEx(g_hotkeyCaptureHook);
        g_hotkeyCaptureHook = nullptr;
    }
    if (g_mainWnd) KillTimer(g_mainWnd, IDT_HK_CAPTURE);
    g_hotkeyCaptureId = 0;
    g_hkCtrlDown = g_hkAltDown = g_hkShiftDown = g_hkWinDown = false;
}

LRESULT CALLBACK HotkeyCaptureHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wParam, lParam);
    bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!isDown && !isUp) return CallNextHookEx(nullptr, code, wParam, lParam);

    auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    DWORD vk = info->vkCode;

    bool* tracked = nullptr;
    if (vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL) tracked = &g_hkCtrlDown;
    else if (vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU) tracked = &g_hkAltDown;
    else if (vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT) tracked = &g_hkShiftDown;
    else if (vk == VK_LWIN || vk == VK_RWIN) tracked = &g_hkWinDown;
    if (tracked) {
        // A bare modifier press/release just updates the tracked state --
        // keep listening for the actual key that completes the combo.
        *tracked = isDown;
        return 1;
    }
    if (!isDown) return 1; // ignore key-ups of non-modifier keys entirely

    if (vk == VK_ESCAPE) {
        StopHotkeyCapture();
        SendState(); // re-renders the button back to its current combo
        return 1;
    }

    UINT mods = 0;
    if (g_hkCtrlDown) mods |= MOD_CONTROL;
    if (g_hkAltDown) mods |= MOD_ALT;
    if (g_hkShiftDown) mods |= MOD_SHIFT;
    if (g_hkWinDown) mods |= MOD_WIN;
    // Require at least one modifier so this can't bind a "hotkey" that's
    // just a normal letter (which would break typing everywhere else).
    // Swallow the keystroke regardless, so a bare key pressed while
    // capturing doesn't leak through to whatever has focus underneath, and
    // stay in capture mode so the user can just try again immediately.
    if (mods == 0) {
        SendError(L"Aucun modificateur détecté : maintiens Ctrl, Alt, Maj ou Win enfoncé "
                  L"PENDANT que tu appuies sur l'autre touche, sans le relâcher avant.");
        return 1;
    }

    int id = g_hotkeyCaptureId;
    StopHotkeyCapture();
    bool ok = false;
    if (id == HOTKEY_TOGGLE_GROUP) {
        ok = RebindHotkey(g_mainWnd, HOTKEY_TOGGLE_GROUP, g_toggleHotkeyMods, g_toggleHotkeyVk, mods, vk);
    } else if (id == HOTKEY_SETUP_SWITCHER) {
        ok = RebindHotkey(g_mainWnd, HOTKEY_SETUP_SWITCHER, g_switcherHotkeyMods, g_switcherHotkeyVk, mods, vk);
    }
    if (ok) SaveConfig();
    else SendError(L"Cette combinaison est déjà utilisée par une autre application.");
    SendState();
    return 1; // swallow: the combo shouldn't also act on whatever has focus
}

void StartHotkeyCapture(int id) {
    StopHotkeyCapture();
    g_hotkeyCaptureHook = SetWindowsHookExW(WH_KEYBOARD_LL, HotkeyCaptureHookProc,
                                            GetModuleHandleW(nullptr), 0);
    if (!g_hotkeyCaptureHook) {
        // Without the hook, no keystroke can ever complete the capture --
        // surface this instead of leaving the button stuck on "Appuyez sur
        // une touche..." with no explanation.
        SendError(L"Impossible d'activer la capture clavier (erreur " +
                  std::to_wstring(GetLastError()) + L").");
        return;
    }
    g_hotkeyCaptureId = id;
    // The hook eats every keystroke on the whole system until a combo
    // completes it -- never leave it armed indefinitely (see IDT_HK_CAPTURE).
    SetTimer(g_mainWnd, IDT_HK_CAPTURE, kHotkeyCaptureTimeoutMs, nullptr);
}

} // namespace al
