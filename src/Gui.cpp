#include "Gui.h"
#include "DeviceManager.h"
#include "AudioEngine.h"
#include "Calibrator.h"
#include "Log.h"
#include "PolicyConfig.h"
#include "resource.h"

#include <shellapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl.h>
#include <wrl/client.h>
#include <WebView2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

// Tray menu
constexpr int ID_TRAY_SHOW = 201;
constexpr int ID_TRAY_TOGGLE = 202;
constexpr int ID_TRAY_QUIT = 203;
// One menu entry per saved setup, ID_TRAY_SETUP_BASE + index into g_setups.
constexpr int ID_TRAY_SETUP_BASE = 300;
constexpr int kMaxTraySetups = 30;

constexpr UINT_PTR IDT_STATUS_TIMER = 1;
// Debounce for device hot-plug notifications: they arrive in bursts (one
// Bluetooth headset connecting fires several), one refresh at the end.
constexpr UINT_PTR IDT_DEVICE_REFRESH = 2;
// Polls the mic calibration worker (see Calibrator) for UI progress.
constexpr UINT_PTR IDT_CAL_TIMER = 3;
// Drives the automatic latency optimizer (see OptimizerTick).
constexpr UINT_PTR IDT_OPT_TIMER = 4;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_DEVICES_CHANGED = WM_APP + 2;
constexpr int HOTKEY_TOGGLE_GROUP = 1;   // Ctrl+Alt+G
constexpr int HOTKEY_SETUP_SWITCHER = 2; // Ctrl+Alt+S
// Ctrl+Shift+1..9 applies setup #1..9 directly. Ctrl+Alt+digit is off-limits:
// that's AltGr+digit on French layouts (~ # { [ | \ ^ @...).
constexpr int HOTKEY_SETUP_FIRST = 10;
constexpr int kSetupHotkeyCount = 9;

constexpr int kMinClientW = 880;
constexpr int kMinClientH = 540;
constexpr int kDefaultClientW = 1100;
constexpr int kDefaultClientH = 910;

// Window background behind the WebView2 control (visible briefly while it
// loads/resizes) -- recomputed by ApplyWindowTheme() to match the current
// Windows light/dark app theme; matches --bg / the light override in ui.html.
COLORREF g_windowBg = RGB(0x0E, 0x10, 0x13);

// The user's Windows accent color. DWM stores it as 0xAABBGGRR; falls back
// to the app's original purple if the value is missing (accent on surfaces
// disabled, very old builds).
COLORREF GetWindowsAccentColor() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return RGB(value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF);
    }
    return RGB(0x7C, 0x6F, 0xEF);
}

// Whether Windows apps currently use the light theme (Settings > Personnalisation
// > Couleurs). Missing key (pre-1809) defaults to light, matching Windows' own
// historical default before dark mode existed.
bool IsWindowsLightThemeEnabled() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value != 0;
    }
    return true;
}

// 0 = auto (follow Windows), 1 = forced light, 2 = forced dark -- see the
// Options window's theme selector. Persisted as ini key [group]/theme.
int g_themeOverride = 0;

// What every theme-driven decision (native chrome + the web content's
// data-theme attribute) should actually use instead of the raw OS reading.
bool EffectiveLightTheme() {
    if (g_themeOverride == 1) return true;
    if (g_themeOverride == 2) return false;
    return IsWindowsLightThemeEnabled();
}

COLORREF WindowBgForTheme(bool light) {
    return light ? RGB(0xF3, 0xF3, 0xF3) : RGB(0x0E, 0x10, 0x13);
}

struct OutputSettings {
    bool checked = false;
    int latencyMs = 0;
    int channelMode = 0; // 0 = stéréo, 1 = gauche, 2 = droite
    int volumePercent = 100;
    std::array<int, kEqBandCount> eqDb{}; // per-band gain, -12..+12 dB each
    bool muted = false;
    // Position in the user-arranged list (drag & drop). Devices never
    // reordered keep this default and sort after the arranged ones, in
    // enumeration order.
    int order = 1000000;
    // User-chosen display name; empty = show the system name. Cosmetic only,
    // deliberately not part of setups (renaming survives applying a preset).
    std::wstring alias;
    // User-chosen accent color for this speaker's row/icon (e.g. "e07a5f");
    // empty = use the default kind-based color. Same treatment as alias --
    // device-level, survives applying a setup saved at another time.
    std::wstring colorHex;
    // Per-speaker source: 0 = system-wide capture (virtual cable), otherwise
    // the PID of the app captured for this speaker. Runtime-only (PIDs don't
    // survive a reboot), hence neither persisted nor stored in setups.
    DWORD sourcePid = 0;
};

// A named snapshot of a full group configuration: every device known at save
// time with its checked state and settings. Applying one replaces the current
// selection wholesale (devices absent from the setup get unchecked).
struct Setup {
    std::wstring name;
    std::vector<std::pair<std::wstring, OutputSettings>> devices;
};

std::unique_ptr<DeviceManager> g_deviceManager;
AudioEngine g_engine;
// Mic-based automatic latency alignment (see Calibrator.h). g_calibrating is
// the GUI-side "a pass is in flight" flag driving IDT_CAL_TIMER;
// g_calRestartAfter remembers the group was playing when the pass started so
// it can be restarted with the new latencies once the pass completes.
Calibrator g_calibrator;
bool g_calibrating = false;
bool g_calRestartAfter = false;

// Automatic latency optimizer: steps the ring cushion down while playback
// stays clean (no new underruns across a measurement window), backs off when
// glitches appear, and stops at the lowest stable value -- so the user gets
// their machine's real latency floor without trial and error. State lives
// here; the logic is OptimizerTick(), driven by IDT_OPT_TIMER.
bool g_optimizing = false;
bool g_optSettle = false;       // skip this window: a change just landed, let it settle
bool g_optBackingOff = false;   // currently raising the cushion after a glitch
uint64_t g_optBaselineGlitches = 0;
int g_optTicks = 0;
constexpr int kOptStepMs = 20;
constexpr int kOptWindowMs = 3500;
constexpr int kOptMaxTicks = 60; // hard stop (~3.5min) against any pathological loop
std::vector<AudioDeviceInfo> g_outputDevices;
std::map<std::wstring, OutputSettings> g_settings;
std::vector<Setup> g_setups;
// Name of the last setup applied/saved, highlighted in the UI. Cleared as
// soon as any individual setting is touched: the current state no longer
// matches the preset, so keeping it highlighted would lie.
std::wstring g_activeSetup;
// Unlike g_activeSetup, this does NOT clear on divergence -- it's the setup a
// "save changes" button in the UI would overwrite, so it must survive the
// very edits that make it useful (checking a device, tweaking a slider...).
// Set alongside g_activeSetup by ApplySetup()/SaveSetupFromCurrent(); only a
// different setup being applied/saved, or this one being deleted, changes it.
std::wstring g_editingSetup;
// The setup whose config the engine is actually running, set when a group
// (successfully) starts and cleared on stop. Deliberately distinct from
// g_activeSetup: selecting a setup for editing (SelectSetup()) reassigns
// g_activeSetup without touching playback, so it alone can't tell the UI
// which row's play button should show "stop".
std::wstring g_playingSetup;
std::wstring g_previousDefaultId;
std::wstring g_configPath;
bool g_autostart = false;
// Live attenuation applied on top of every output's own volume (which stays
// unchanged in g_settings/the ini) -- lets one slider duck the whole group
// without losing the balance set between speakers.
int g_masterVolumePercent = 100;
// Mirrors AudioEngine's base ring-buffer cushion (kDefaultBaseCushionMs) so
// it can be edited from the UI and persisted -- see the "cushion"/
// "cushioncommit" web commands.
int g_baseCushionMs = kDefaultBaseCushionMs;
// Mirrors AudioEngine's device-buffer fill target (kDefaultDeviceFillMs) --
// see the "devicefill"/"devicefillcommit" web commands. Cushion + fill is
// the bulk of the group's total latency vs. native playback.
int g_deviceFillMs = kDefaultDeviceFillMs;
// Low-latency period toggle (IAudioClient3) -- see AudioEngine::setLowLatency
// and the "lowlatency" web command. Persisted as [group]/lowlatency.
bool g_lowLatency = false;
// Customizable global hotkeys (see FormatHotkey/RebindHotkey/LoadHotkeysFromIni).
UINT g_toggleHotkeyMods = MOD_CONTROL | MOD_ALT;
UINT g_toggleHotkeyVk = 'G';
UINT g_switcherHotkeyMods = MOD_CONTROL | MOD_ALT;
UINT g_switcherHotkeyVk = 'S';
// Native hotkey-capture state (see HotkeyCaptureHookProc): non-zero while
// waiting for the next combo, holding which binding it'll go to.
HHOOK g_hotkeyCaptureHook = nullptr;
int g_hotkeyCaptureId = 0; // HOTKEY_TOGGLE_GROUP / HOTKEY_SETUP_SWITCHER, or 0
// True from OnCreateGroup() until WM_TIMER observes g_engine.isStarting()
// having gone back to false, at which point it checks whether that attempt
// succeeded or failed and finalizes the UI accordingly (see WM_TIMER).
bool g_awaitingGroupStart = false;
// Autostart fires once, when the web UI first reports 'ready' (the UI has to
// exist before starting: errors and state changes are reported through it).
bool g_autostartPending = false;
// Set by ImportConfig just before relaunching: WM_DESTROY must NOT run its
// usual SaveConfig(), which would overwrite the freshly-imported file with
// the old in-memory settings before the new instance reads it.
bool g_skipSaveOnExit = false;
// Startup problems that happened before the web UI existed (e.g. a hotkey
// already taken by another app) -- delivered once the UI reports 'ready'.
std::wstring g_startupWarning;
// Cross-process "show yourself" message: a second instance posts this to the
// first one's main window before exiting (see the single-instance guard in
// RunApplication). RegisterWindowMessageW guarantees a system-wide unique id.
UINT g_showAppMessage = 0;

HBRUSH g_hBrushWindowBg = nullptr;
HBRUSH g_optionsHBrushWindowBg = nullptr; // the Options window class's own background brush
HICON g_hAppIcon = nullptr;
NOTIFYICONDATAW g_trayIcon = {};

ComPtr<ICoreWebView2Controller> g_webviewController;
ComPtr<ICoreWebView2> g_webview;
EventRegistrationToken g_webMessageToken = {};
// Shared with the Options window's controller (see OpenOptionsWindow) --
// environments are meant to be reused across controllers/windows instead of
// creating a new one per top-level window.
ComPtr<ICoreWebView2Environment> g_webviewEnvironment;

// Options: a real, separate, modal top-level window (see OpenOptionsWindow),
// not an in-page overlay -- movable/resizable independently of the main
// window, with its own WebView2 controller hosting options.html.
HWND g_optionsWnd = nullptr;
ComPtr<ICoreWebView2Controller> g_optionsController;
ComPtr<ICoreWebView2> g_optionsWebview;
EventRegistrationToken g_optionsMessageToken = {};

// Re-syncs everything driven by the native side's own rendering (the WebView2
// host background, the DWM title bar, and later the switcher popup's GDI
// palette) with the effective theme (Windows setting, or the Options
// window's override -- see EffectiveLightTheme). The web content itself
// (ui.html/options.html) re-themes on its own via a `data-theme` attribute
// driven by state.theme (see SendState), falling back to the OS-synced
// `prefers-color-scheme` media query when there's no override.
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
HHOOK g_switcherWheelHook = nullptr;
HWND g_switcherWnd = nullptr;
HWND g_mainWnd = nullptr; // ApplySetup needs the main window (status timer)

// Forwards MMDevice hot-plug events to the GUI thread. Callbacks arrive on a
// COM worker thread, so nothing is touched here beyond a PostMessage; the
// window procedure debounces and re-enumerates. Stack/static lifetime, like
// the engine's EndpointVolumeCallback.
class DeviceNotificationClient : public IMMNotificationClient {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ref_.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override { return ref_.fetch_sub(1) - 1; }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { notify(); return S_OK; }
    // Ignored: we change the default endpoint ourselves when starting or
    // stopping a group -- reacting to it would refresh in a loop.
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
    // Ignored: fires constantly (volume, jack info...), never list-relevant.
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void notify() {
        if (g_mainWnd) PostMessageW(g_mainWnd, WM_DEVICES_CHANGED, 0, 0);
    }
    std::atomic<ULONG> ref_{1};
};

DeviceNotificationClient g_deviceNotifications;

// The VB-Cable endpoint is the group's public face in Windows; it must never
// appear as a group member (instant feedback loop) and the leftover
// Voicemeeter endpoints are just noise, so all of them are hidden.
bool IsVirtualDevice(const std::wstring& name) {
    return name.find(L"VB-Audio") != std::wstring::npos ||
           name.find(L"CABLE") != std::wstring::npos ||
           name.find(L"Voicemeeter") != std::wstring::npos ||
           name == L"(nom inconnu)";
}

bool IsCableDevice(const std::wstring& name) {
    if (name.find(L"16ch") != std::wstring::npos) return false;
    return name.find(L"VB-Audio Virtual Cable") != std::wstring::npos ||
           name.find(L"CABLE Input") != std::wstring::npos ||
           name.find(L"CABLE Output") != std::wstring::npos;
}

// --- Config persistence (INI in %APPDATA%) ---

void InitConfigPath() {
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) > 0) {
        g_configPath = std::wstring(buf) + L"\\AudioLinker.ini";
    } else {
        g_configPath = L"AudioLinker.ini";
    }
}

void SaveConfig() {
    if (g_configPath.empty()) return;
    WritePrivateProfileStringW(L"group", L"autostart", g_autostart ? L"1" : L"0", g_configPath.c_str());
    WritePrivateProfileStringW(L"group", L"mastervolume", std::to_wstring(g_masterVolumePercent).c_str(),
                               g_configPath.c_str());
    WritePrivateProfileStringW(L"group", L"cushionms", std::to_wstring(g_baseCushionMs).c_str(),
                               g_configPath.c_str());
    WritePrivateProfileStringW(L"group", L"devicefillms", std::to_wstring(g_deviceFillMs).c_str(),
                               g_configPath.c_str());
    WritePrivateProfileStringW(L"group", L"lowlatency", g_lowLatency ? L"1" : L"0",
                               g_configPath.c_str());
    WritePrivateProfileStringW(L"group", L"theme", std::to_wstring(g_themeOverride).c_str(),
                               g_configPath.c_str());
    WritePrivateProfileStringW(L"hotkeys", L"togglemods", std::to_wstring(g_toggleHotkeyMods).c_str(),
                               g_configPath.c_str());
    WritePrivateProfileStringW(L"hotkeys", L"togglevk", std::to_wstring(g_toggleHotkeyVk).c_str(),
                               g_configPath.c_str());
    WritePrivateProfileStringW(L"hotkeys", L"switchermods", std::to_wstring(g_switcherHotkeyMods).c_str(),
                               g_configPath.c_str());
    WritePrivateProfileStringW(L"hotkeys", L"switchervk", std::to_wstring(g_switcherHotkeyVk).c_str(),
                               g_configPath.c_str());
    WritePrivateProfileStringW(L"setups", L"active", g_activeSetup.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"setups", L"editing", g_editingSetup.c_str(), g_configPath.c_str());

    for (const auto& [deviceId, s] : g_settings) {
        const wchar_t* section = deviceId.c_str();
        WritePrivateProfileStringW(section, L"checked", s.checked ? L"1" : L"0", g_configPath.c_str());
        WritePrivateProfileStringW(section, L"latency", std::to_wstring(s.latencyMs).c_str(), g_configPath.c_str());
        WritePrivateProfileStringW(section, L"channel", std::to_wstring(s.channelMode).c_str(), g_configPath.c_str());
        WritePrivateProfileStringW(section, L"volume", std::to_wstring(s.volumePercent).c_str(), g_configPath.c_str());
        for (int b = 0; b < kEqBandCount; ++b) {
            WritePrivateProfileStringW(section, (L"eq" + std::to_wstring(b)).c_str(),
                                       std::to_wstring(s.eqDb[b]).c_str(), g_configPath.c_str());
        }
        WritePrivateProfileStringW(section, L"muted", s.muted ? L"1" : L"0", g_configPath.c_str());
        WritePrivateProfileStringW(section, L"order", std::to_wstring(s.order).c_str(), g_configPath.c_str());
        WritePrivateProfileStringW(section, L"alias", s.alias.c_str(), g_configPath.c_str());
        WritePrivateProfileStringW(section, L"color", s.colorHex.c_str(), g_configPath.c_str());
    }
}

int ClampDb(int v) { return std::min(12, std::max(-12, v)); }
int ClampChannelMode(int v) { return std::min(2, std::max(0, v)); }
int ClampVolume(int v) { return std::min(100, std::max(0, v)); }
int ClampLatencyMs(int v) { return std::min(500, std::max(0, v)); }
int ClampCushionMs(int v) { return std::min(kMaxBaseCushionMs, std::max(kMinBaseCushionMs, v)); }
int ClampDeviceFillMs(int v) { return std::min(kMaxDeviceFillMs, std::max(kMinDeviceFillMs, v)); }
int ClampThemeOverride(int v) { return std::min(2, std::max(0, v)); }

// The volume actually sent to the engine: the speaker's own slider scaled by
// the group's master volume. g_settings keeps s.volumePercent as the
// unscaled base value, so the per-speaker balance survives moving the master
// slider (and is what gets persisted/shown on that speaker's own slider).
int EffectiveVolume(const OutputSettings& s) {
    return ClampVolume((s.volumePercent * g_masterVolumePercent + 50) / 100);
}

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

void LoadHotkeysFromIni() {
    if (g_configPath.empty()) return;
    g_toggleHotkeyMods = GetPrivateProfileIntW(L"hotkeys", L"togglemods", MOD_CONTROL | MOD_ALT,
                                               g_configPath.c_str());
    g_toggleHotkeyVk = GetPrivateProfileIntW(L"hotkeys", L"togglevk", 'G', g_configPath.c_str());
    g_switcherHotkeyMods = GetPrivateProfileIntW(L"hotkeys", L"switchermods", MOD_CONTROL | MOD_ALT,
                                                 g_configPath.c_str());
    g_switcherHotkeyVk = GetPrivateProfileIntW(L"hotkeys", L"switchervk", 'S', g_configPath.c_str());
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

OutputSettings LoadDeviceSettings(const std::wstring& deviceId) {
    OutputSettings s;
    if (g_configPath.empty()) return s;
    const wchar_t* section = deviceId.c_str();
    s.checked = GetPrivateProfileIntW(section, L"checked", 0, g_configPath.c_str()) != 0;
    s.latencyMs = GetPrivateProfileIntW(section, L"latency", 0, g_configPath.c_str());
    s.channelMode = GetPrivateProfileIntW(section, L"channel", 0, g_configPath.c_str());
    s.volumePercent = GetPrivateProfileIntW(section, L"volume", 100, g_configPath.c_str());
    // Bands default from the old bass/treble keys (band 0 = low shelf, the
    // last = high shelf) so existing configs migrate instead of resetting.
    int legacyBass = static_cast<int>(GetPrivateProfileIntW(section, L"bass", 0, g_configPath.c_str()));
    int legacyTreble = static_cast<int>(GetPrivateProfileIntW(section, L"treble", 0, g_configPath.c_str()));
    for (int b = 0; b < kEqBandCount; ++b) {
        int fallback = (b == 0) ? legacyBass : (b == kEqBandCount - 1) ? legacyTreble : 0;
        s.eqDb[b] = ClampDb(static_cast<int>(
            GetPrivateProfileIntW(section, (L"eq" + std::to_wstring(b)).c_str(), fallback, g_configPath.c_str())));
    }
    s.muted = GetPrivateProfileIntW(section, L"muted", 0, g_configPath.c_str()) != 0;
    s.order = static_cast<int>(GetPrivateProfileIntW(section, L"order", 1000000, g_configPath.c_str()));
    wchar_t aliasBuf[128] = {};
    GetPrivateProfileStringW(section, L"alias", L"", aliasBuf, 128, g_configPath.c_str());
    s.alias = aliasBuf;
    wchar_t colorBuf[16] = {};
    GetPrivateProfileStringW(section, L"color", L"", colorBuf, 16, g_configPath.c_str());
    s.colorHex = colorBuf;
    // Clamp (not reset-to-0) to match LoadSetupsFromIni and the live "channel"
    // handler, which treat an out-of-range value as clamped rather than reset.
    s.channelMode = ClampChannelMode(s.channelMode);
    s.volumePercent = ClampVolume(s.volumePercent);
    s.latencyMs = ClampLatencyMs(s.latencyMs);
    return s;
}

// --- Setups (named presets), persisted as [setup_N] sections in the INI ---

std::wstring SetupSection(size_t i) { return L"setup_" + std::to_wstring(i); }

std::vector<std::wstring> SplitPipes(const std::wstring& s) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (true) {
        size_t p = s.find(L'|', start);
        parts.push_back(s.substr(start, p - start));
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    return parts;
}

void SaveSetupsToIni() {
    if (g_configPath.empty()) return;
    // Delete every previously written section first so shrinking the list
    // doesn't leave orphaned [setup_N] sections behind.
    int oldCount = GetPrivateProfileIntW(L"setups", L"count", 0, g_configPath.c_str());
    for (int i = 0; i < oldCount; ++i) {
        WritePrivateProfileStringW(SetupSection(i).c_str(), nullptr, nullptr, g_configPath.c_str());
    }
    WritePrivateProfileStringW(L"setups", L"count", std::to_wstring(g_setups.size()).c_str(),
                               g_configPath.c_str());
    WritePrivateProfileStringW(L"setups", L"active", g_activeSetup.c_str(), g_configPath.c_str());
    WritePrivateProfileStringW(L"setups", L"editing", g_editingSetup.c_str(), g_configPath.c_str());
    for (size_t i = 0; i < g_setups.size(); ++i) {
        const auto& setup = g_setups[i];
        std::wstring section = SetupSection(i);
        WritePrivateProfileStringW(section.c_str(), L"name", setup.name.c_str(), g_configPath.c_str());
        WritePrivateProfileStringW(section.c_str(), L"devcount",
                                   std::to_wstring(setup.devices.size()).c_str(), g_configPath.c_str());
        for (size_t k = 0; k < setup.devices.size(); ++k) {
            const auto& [id, s] = setup.devices[k];
            // Endpoint ids never contain '|', so pipes are a safe separator.
            // Fields 5/6 keep the pre-multiband "bass"/"treble" slots (band 0
            // and the last band) so old parsers/saves stay compatible; the
            // remaining middle bands are appended after the muted flag.
            std::wstring v = id + L"|" + (s.checked ? L"1" : L"0") +
                             L"|" + std::to_wstring(s.channelMode) +
                             L"|" + std::to_wstring(s.volumePercent) +
                             L"|" + std::to_wstring(s.latencyMs) +
                             L"|" + std::to_wstring(s.eqDb[0]) +
                             L"|" + std::to_wstring(s.eqDb[kEqBandCount - 1]) +
                             L"|" + (s.muted ? L"1" : L"0");
            for (int b = 1; b < kEqBandCount - 1; ++b) v += L"|" + std::to_wstring(s.eqDb[b]);
            std::wstring key = L"dev" + std::to_wstring(k);
            WritePrivateProfileStringW(section.c_str(), key.c_str(), v.c_str(), g_configPath.c_str());
        }
    }
}

void LoadSetupsFromIni() {
    g_setups.clear();
    if (g_configPath.empty()) return;
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(L"setups", L"active", L"", buf, 512, g_configPath.c_str());
    g_activeSetup = buf;
    GetPrivateProfileStringW(L"setups", L"editing", L"", buf, 512, g_configPath.c_str());
    g_editingSetup = buf;
    int count = GetPrivateProfileIntW(L"setups", L"count", 0, g_configPath.c_str());
    for (int i = 0; i < count; ++i) {
        std::wstring section = SetupSection(i);
        Setup setup;
        GetPrivateProfileStringW(section.c_str(), L"name", L"", buf, 512, g_configPath.c_str());
        setup.name = buf;
        if (setup.name.empty()) continue;
        int devCount = GetPrivateProfileIntW(section.c_str(), L"devcount", 0, g_configPath.c_str());
        for (int k = 0; k < devCount; ++k) {
            std::wstring key = L"dev" + std::to_wstring(k);
            GetPrivateProfileStringW(section.c_str(), key.c_str(), L"", buf, 512, g_configPath.c_str());
            auto parts = SplitPipes(buf);
            // 7 fields = setups saved before the muted flag existed.
            if (parts.size() < 7 || parts[0].empty()) continue;
            OutputSettings s;
            s.checked = parts[1] == L"1";
            s.channelMode = ClampChannelMode(_wtoi(parts[2].c_str()));
            s.volumePercent = ClampVolume(_wtoi(parts[3].c_str()));
            s.latencyMs = ClampLatencyMs(_wtoi(parts[4].c_str()));
            s.eqDb[0] = ClampDb(_wtoi(parts[5].c_str()));
            s.eqDb[kEqBandCount - 1] = ClampDb(_wtoi(parts[6].c_str()));
            s.muted = parts.size() >= 8 && parts[7] == L"1";
            // Middle bands are absent in setups saved before the 5-band EQ
            // (or in the 2-band bass/treble era) -- default them to 0dB.
            for (int b = 1; b < kEqBandCount - 1; ++b) {
                size_t idx = 7 + static_cast<size_t>(b);
                s.eqDb[b] = (parts.size() > idx) ? ClampDb(_wtoi(parts[idx].c_str())) : 0;
            }
            setup.devices.emplace_back(parts[0], s);
        }
        if (!setup.devices.empty()) g_setups.push_back(std::move(setup));
    }
    // A dangling name (setup deleted in a previous run) must not stick around.
    bool activeExists = false, editingExists = false;
    for (const auto& s : g_setups) {
        if (s.name == g_activeSetup) activeExists = true;
        if (s.name == g_editingSetup) editingExists = true;
    }
    if (!activeExists) g_activeSetup.clear();
    if (!editingExists) g_editingSetup.clear();
}

// --- Launch at Windows startup (HKCU Run key) ---

constexpr const wchar_t* kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool IsLaunchAtStartupEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    bool found = RegQueryValueExW(key, L"AudioLinker", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

void SetLaunchAtStartup(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        // --tray: start hidden in the notification area instead of opening
        // the window on every logon.
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --tray";
        RegSetValueExW(key, L"AudioLinker", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"AudioLinker");
    }
    RegCloseKey(key);
}

// --- Endpoint management (rename + default device via IPolicyConfig) ---

// Note: the MMDevices registry keys are writable only by the audio service,
// even elevated — renaming must go through IPolicyConfig like the sound
// control panel does.
bool RenameEndpoint(const std::wstring& deviceId, const std::wstring& newName) {
    ComPtr<IPolicyConfig> pc;
    if (FAILED(CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL,
                                 __uuidof(IPolicyConfig),
                                 reinterpret_cast<void**>(pc.GetAddressOf())))) {
        return false;
    }
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_LPWSTR;
    pv.pwszVal = const_cast<LPWSTR>(newName.c_str());
    // pv wraps a string we don't own: no PropVariantClear.
    return SUCCEEDED(pc->SetPropertyValue(deviceId.c_str(), 0, PKEY_Device_DeviceDesc, &pv));
}

bool SetDefaultRenderDevice(const std::wstring& deviceId) {
    ComPtr<IPolicyConfig> pc;
    if (FAILED(CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL,
                                 __uuidof(IPolicyConfig),
                                 reinterpret_cast<void**>(pc.GetAddressOf())))) {
        return false;
    }
    bool ok = SUCCEEDED(pc->SetDefaultEndpoint(deviceId.c_str(), eConsole));
    pc->SetDefaultEndpoint(deviceId.c_str(), eMultimedia);
    // Without this, apps that pick their device via the communications role
    // (Teams, Discord, Skype...) stay on the old device when the group
    // starts, unlike the Sound panel which sets all three roles.
    pc->SetDefaultEndpoint(deviceId.c_str(), eCommunications);
    return ok;
}

std::wstring GetDefaultRenderId() {
    for (auto& d : g_deviceManager->enumerateRenderDevices()) {
        if (d.isDefault) return d.id;
    }
    return L"";
}

// --- Audio source apps (process-loopback candidates) ---

struct SourceApp {
    DWORD pid;
    std::wstring name;
};

struct ProcSnapshotEntry {
    DWORD parentPid;
    std::wstring exe;
};

std::map<DWORD, ProcSnapshotEntry> SnapshotProcesses() {
    std::map<DWORD, ProcSnapshotEntry> procs;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return procs;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            procs[pe.th32ProcessID] = { pe.th32ParentProcessID, pe.szExeFile };
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return procs;
}

// Multi-process apps (Chrome, Spotify, Discord...) attribute their audio
// sessions to child processes -- often a *sibling* of the process that
// actually renders the audio. Capturing that child's tree yields silence.
// Climb to the top-most ancestor running the same executable: its tree
// contains the whole app, audio service included.
DWORD RootProcessSameExe(DWORD pid, const std::map<DWORD, ProcSnapshotEntry>& procs) {
    DWORD current = pid;
    for (int depth = 0; depth < 32; ++depth) {
        auto it = procs.find(current);
        if (it == procs.end()) break;
        auto parent = procs.find(it->second.parentPid);
        if (parent == procs.end() || it->second.parentPid == current) break;
        if (_wcsicmp(parent->second.exe.c_str(), it->second.exe.c_str()) != 0) break;
        current = it->second.parentPid;
    }
    return current;
}

// --- Per-app endpoint routing ---
// The API behind Settings > "App volume and device preferences". Undocumented
// but stable (EarTrumpet, SoundSwitch...): Windows.Media.Internal.
// AudioPolicyConfig. Used to send a captured app's own output into the
// virtual cable for the session's duration -- muting its mixer session is
// NOT an option, the process-loopback tap sits after session volume, so a
// mute silences the capture too (hence the "son pendant 0,25s" symptom).

// IInspectable base is load-bearing: this is a WinRT interface, its vtable
// carries GetIids/GetRuntimeClassName/GetTrustLevel before everything else.
// Deriving from plain IUnknown shifts every call three slots early -- which
// crashes in SetPersistedDefaultAudioEndpoint.
struct IAudioPolicyConfigFactory : public IInspectable {
    // 19 vtable slots we don't use, in their exact order.
    virtual HRESULT STDMETHODCALLTYPE p01() = 0; virtual HRESULT STDMETHODCALLTYPE p02() = 0;
    virtual HRESULT STDMETHODCALLTYPE p03() = 0; virtual HRESULT STDMETHODCALLTYPE p04() = 0;
    virtual HRESULT STDMETHODCALLTYPE p05() = 0; virtual HRESULT STDMETHODCALLTYPE p06() = 0;
    virtual HRESULT STDMETHODCALLTYPE p07() = 0; virtual HRESULT STDMETHODCALLTYPE p08() = 0;
    virtual HRESULT STDMETHODCALLTYPE p09() = 0; virtual HRESULT STDMETHODCALLTYPE p10() = 0;
    virtual HRESULT STDMETHODCALLTYPE p11() = 0; virtual HRESULT STDMETHODCALLTYPE p12() = 0;
    virtual HRESULT STDMETHODCALLTYPE p13() = 0; virtual HRESULT STDMETHODCALLTYPE p14() = 0;
    virtual HRESULT STDMETHODCALLTYPE p15() = 0; virtual HRESULT STDMETHODCALLTYPE p16() = 0;
    virtual HRESULT STDMETHODCALLTYPE p17() = 0; virtual HRESULT STDMETHODCALLTYPE p18() = 0;
    virtual HRESULT STDMETHODCALLTYPE p19() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(
        UINT processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(
        UINT processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

ComPtr<IAudioPolicyConfigFactory> GetAudioPolicyConfigFactory() {
    static const wchar_t kClass[] = L"Windows.Media.Internal.AudioPolicyConfig";
    HSTRING_HEADER header;
    HSTRING cls = nullptr;
    if (FAILED(WindowsCreateStringReference(kClass, ARRAYSIZE(kClass) - 1, &header, &cls))) {
        return nullptr;
    }
    // Interface IID changed in 21H2; same vtable layout on both.
    static const GUID kIid21H2 = { 0x2a59116d, 0x6c4f, 0x45e0,
                                   { 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58 } };
    static const GUID kIidLegacy = { 0xab3d4648, 0xe242, 0x459f,
                                     { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 } };
    ComPtr<IAudioPolicyConfigFactory> factory;
    if (FAILED(RoGetActivationFactory(cls, kIid21H2,
                                      reinterpret_cast<void**>(factory.GetAddressOf())))) {
        RoGetActivationFactory(cls, kIidLegacy,
                               reinterpret_cast<void**>(factory.GetAddressOf()));
    }
    return factory;
}

// Empty mmDeviceId = reset the app back to "Default".
bool SetAppRenderEndpoint(DWORD pid, const std::wstring& mmDeviceId) {
    auto factory = GetAudioPolicyConfigFactory();
    if (!factory) return false;
    HSTRING device = nullptr;
    std::wstring path;
    if (!mmDeviceId.empty()) {
        // SWD device-interface path form, DEVINTERFACE_AUDIO_RENDER suffix.
        path = L"\\\\?\\SWD#MMDEVAPI#" + mmDeviceId + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
        if (FAILED(WindowsCreateString(path.c_str(), static_cast<UINT32>(path.size()), &device))) {
            return false;
        }
    }
    HRESULT hr1 = factory->SetPersistedDefaultAudioEndpoint(pid, eRender, eConsole, device);
    HRESULT hr2 = factory->SetPersistedDefaultAudioEndpoint(pid, eRender, eMultimedia, device);
    if (device) WindowsDeleteString(device);
    return SUCCEEDED(hr1) && SUCCEEDED(hr2);
}

// Apps we routed into the cable for the current group session; every stop
// path must undo this or the app stays silent after AudioLinker exits.
std::vector<DWORD> g_routedPids;

void SaveRoutedAppsToIni();

void UnrouteAllApps() {
    for (DWORD pid : g_routedPids) SetAppRenderEndpoint(pid, L"");
    g_routedPids.clear();
    if (!g_configPath.empty()) {
        WritePrivateProfileStringW(L"routing", L"apps", L"", g_configPath.c_str());
    }
}

// Apps currently holding a render session on any output device -- the
// candidates for direct per-application capture. Keyed by their root
// process, so Chrome's dozen per-tab sessions collapse into one entry.
std::vector<SourceApp> EnumerateSourceApps() {
    std::vector<SourceApp> result;
    DWORD selfPid = GetCurrentProcessId();
    auto procs = SnapshotProcesses();
    for (const auto& d : g_deviceManager->enumerateRenderDevices()) {
        auto device = g_deviceManager->getDeviceById(d.id);
        if (!device) continue;
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(manager.GetAddressOf())))) continue;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(sessions.GetAddressOf()))) continue;
        int count = 0;
        sessions->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> ctrl;
            if (FAILED(sessions->GetSession(i, ctrl.GetAddressOf()))) continue;
            AudioSessionState state = AudioSessionStateInactive;
            ctrl->GetState(&state);
            if (state == AudioSessionStateExpired) continue;
            ComPtr<IAudioSessionControl2> ctrl2;
            if (FAILED(ctrl.As(&ctrl2))) continue;
            if (ctrl2->IsSystemSoundsSession() == S_OK) continue;
            DWORD sessionPid = 0;
            if (FAILED(ctrl2->GetProcessId(&sessionPid)) || sessionPid == 0 || sessionPid == selfPid) continue;
            DWORD pid = RootProcessSameExe(sessionPid, procs);
            bool known = false;
            for (const auto& s : result) {
                if (s.pid == pid) known = true;
            }
            if (known) continue;

            std::wstring name;
            auto pit = procs.find(pid);
            if (pit != procs.end()) name = pit->second.exe;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, L".exe") == 0) {
                name.resize(name.size() - 4);
            }
            if (name.empty()) name = L"PID " + std::to_wstring(pid);
            result.push_back({ pid, name });
        }
    }
    std::sort(result.begin(), result.end(),
              [](const SourceApp& a, const SourceApp& b) { return a.name < b.name; });
    return result;
}

// Remembers which executables are currently routed into the cable, so a
// crash (no clean stop path) can be repaired at the next launch.
void SaveRoutedAppsToIni() {
    if (g_configPath.empty()) return;
    auto procs = SnapshotProcesses();
    std::wstring list;
    for (DWORD pid : g_routedPids) {
        auto it = procs.find(pid);
        if (it == procs.end()) continue;
        if (!list.empty()) list += L';';
        list += it->second.exe;
    }
    WritePrivateProfileStringW(L"routing", L"apps", list.c_str(), g_configPath.c_str());
}

// Startup repair: if a previous run died while apps were routed into the
// cable, put every still-running instance of those executables back on the
// default device. (An app not running keeps its stale route until it can be
// resolved to a PID again -- unavoidable with this API.)
void CleanupStaleRoutes() {
    if (g_configPath.empty()) return;
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(L"routing", L"apps", L"", buf, 1024, g_configPath.c_str());
    std::wstring list = buf;
    if (list.empty()) return;
    auto procs = SnapshotProcesses();
    std::vector<DWORD> roots;
    size_t start = 0;
    while (start <= list.size()) {
        size_t sep = list.find(L';', start);
        std::wstring exe = list.substr(start, sep == std::wstring::npos ? std::wstring::npos
                                                                        : sep - start);
        if (!exe.empty()) {
            for (const auto& [pid, info] : procs) {
                if (_wcsicmp(info.exe.c_str(), exe.c_str()) != 0) continue;
                DWORD root = RootProcessSameExe(pid, procs);
                if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
                    roots.push_back(root);
                }
            }
        }
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    for (DWORD root : roots) SetAppRenderEndpoint(root, L"");
    WritePrivateProfileStringW(L"routing", L"apps", L"", g_configPath.c_str());
}

// --- Device list ---

// User-arranged order first (drag & drop), then enumeration order for
// devices that were never moved (stable_sort keeps their relative order).
void SortOutputDevicesByOrder() {
    std::stable_sort(g_outputDevices.begin(), g_outputDevices.end(),
                     [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
                         return g_settings[a.id].order < g_settings[b.id].order;
                     });
}

void PopulateOutputList() {
    g_outputDevices.clear();
    for (auto& d : g_deviceManager->enumerateRenderDevices()) {
        if (IsVirtualDevice(d.name)) continue;
        if (g_settings.find(d.id) == g_settings.end()) {
            g_settings[d.id] = LoadDeviceSettings(d.id);
        }
        g_outputDevices.push_back(d);
    }
    SortOutputDevicesByOrder();
}

// --- JSON bridge to the web UI ---

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    wchar_t buf[8];
                    swprintf(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void PostJson(const std::wstring& json) {
    if (g_webview) g_webview->PostWebMessageAsJson(json.c_str());
    if (g_optionsWebview) g_optionsWebview->PostWebMessageAsJson(json.c_str());
}

std::wstring BoolJson(bool b) { return b ? L"true" : L"false"; }

const wchar_t* DeviceKindJson(AudioDeviceKind kind) {
    switch (kind) {
        case AudioDeviceKind::Headphones: return L"headphones";
        case AudioDeviceKind::Hdmi:        return L"hdmi";
        case AudioDeviceKind::Digital:     return L"digital";
        case AudioDeviceKind::Bluetooth:   return L"bluetooth";
        default:                           return L"speaker";
    }
}

void SendState() {
    COLORREF accent = GetWindowsAccentColor();
    wchar_t accentHex[8];
    swprintf(accentHex, 8, L"#%02x%02x%02x", GetRValue(accent), GetGValue(accent), GetBValue(accent));
    std::wstring j = L"{\"type\":\"state\",\"running\":" + BoolJson(g_engine.isRunning()) +
                     L",\"starting\":" + BoolJson(g_engine.isStarting()) +
                     L",\"accent\":\"" + accentHex +
                     L"\",\"autostart\":" + BoolJson(g_autostart) +
                     L",\"launchAtStartup\":" + BoolJson(IsLaunchAtStartupEnabled()) +
                     L",\"masterVolume\":" + std::to_wstring(g_masterVolumePercent) +
                     L",\"cushionMs\":" + std::to_wstring(g_baseCushionMs) +
                     L",\"cushionMsMin\":" + std::to_wstring(kMinBaseCushionMs) +
                     L",\"cushionMsMax\":" + std::to_wstring(kMaxBaseCushionMs) +
                     L",\"deviceFillMs\":" + std::to_wstring(g_deviceFillMs) +
                     L",\"deviceFillMsMin\":" + std::to_wstring(kMinDeviceFillMs) +
                     L",\"deviceFillMsMax\":" + std::to_wstring(kMaxDeviceFillMs) +
                     L",\"lowLatency\":" + BoolJson(g_lowLatency) +
                     L",\"optimizing\":" + BoolJson(g_optimizing) +
                     L",\"theme\":" + std::to_wstring(g_themeOverride) +
                     L",\"toggleHotkey\":\"" + JsonEscape(FormatHotkey(g_toggleHotkeyMods, g_toggleHotkeyVk)) +
                     L"\",\"switcherHotkey\":\"" + JsonEscape(FormatHotkey(g_switcherHotkeyMods, g_switcherHotkeyVk)) +
                     L"\",\"capturingHotkey\":\"" +
                     (g_hotkeyCaptureId == HOTKEY_TOGGLE_GROUP ? L"toggle" :
                      g_hotkeyCaptureId == HOTKEY_SETUP_SWITCHER ? L"switcher" : L"") +
                     L"\",\"devices\":[";
    for (size_t i = 0; i < g_outputDevices.size(); ++i) {
        const auto& d = g_outputDevices[i];
        const auto& s = g_settings[d.id];
        if (i) j += L',';
        j += L"{\"id\":\"" + JsonEscape(d.id) +
             L"\",\"name\":\"" + JsonEscape(d.name) +
             L"\",\"kind\":\"" + DeviceKindJson(d.kind) +
             L"\",\"alias\":\"" + JsonEscape(s.alias) +
             L"\",\"color\":\"" + JsonEscape(s.colorHex) +
             L"\",\"checked\":" + BoolJson(s.checked) +
             L",\"channel\":" + std::to_wstring(s.channelMode) +
             L",\"volume\":" + std::to_wstring(s.volumePercent) +
             L",\"latency\":" + std::to_wstring(s.latencyMs) +
             L",\"eq\":[";
        for (int b = 0; b < kEqBandCount; ++b) {
            if (b) j += L',';
            j += std::to_wstring(s.eqDb[b]);
        }
        j += L"]";
        j += L",\"muted\":" + BoolJson(s.muted) +
             L",\"sourcePid\":" + std::to_wstring(s.sourcePid) + L"}";
    }
    j += L"],\"setups\":[";
    for (size_t i = 0; i < g_setups.size(); ++i) {
        const auto& s = g_setups[i];
        size_t checkedCount = 0;
        for (const auto& [id, os] : s.devices) {
            if (os.checked) ++checkedCount;
        }
        if (i) j += L',';
        j += L"{\"name\":\"" + JsonEscape(s.name) +
             L"\",\"count\":" + std::to_wstring(checkedCount) + L"}";
    }
    j += L"],\"activeSetup\":\"" + JsonEscape(g_activeSetup) +
         L"\",\"editingSetup\":\"" + JsonEscape(g_editingSetup) +
         L"\",\"playingSetup\":\"" + JsonEscape(g_playingSetup) + L"\",\"sources\":[";
    auto sources = EnumerateSourceApps();
    for (size_t i = 0; i < sources.size(); ++i) {
        if (i) j += L',';
        j += L"{\"pid\":" + std::to_wstring(sources[i].pid) +
             L",\"name\":\"" + JsonEscape(sources[i].name) + L"\"}";
    }
    // Physical capture devices, for the mic-calibration modal's input picker
    // (virtual endpoints filtered out: a cable "mic" can't hear a speaker).
    j += L"],\"inputs\":[";
    size_t inputCount = 0;
    for (const auto& d : g_deviceManager->enumerateCaptureDevices()) {
        if (IsVirtualDevice(d.name)) continue;
        if (inputCount++) j += L',';
        j += L"{\"id\":\"" + JsonEscape(d.id) +
             L"\",\"name\":\"" + JsonEscape(d.name) +
             L"\",\"default\":" + BoolJson(d.isDefault) + L"}";
    }
    j += L"]}";
    PostJson(j);
}

void SendStatus() {
    // getStatus() is a destructive read (peak resets to 0), so it is called
    // exactly once per timer tick, here.
    auto status = g_engine.getStatus();
    std::wstring j = L"{\"type\":\"status\",\"running\":" + BoolJson(g_engine.isRunning()) +
                     L",\"starting\":" + BoolJson(g_engine.isStarting()) + L",\"outputs\":[";
    for (size_t i = 0; i < status.size(); ++i) {
        const auto& st = status[i];
        if (i) j += L',';
        wchar_t peakBuf[16];
        swprintf(peakBuf, 16, L"%.3f", st.peak);
        j += L"{\"id\":\"" + JsonEscape(st.deviceId) +
             L"\",\"active\":" + BoolJson(st.active) +
             L",\"peak\":" + peakBuf +
             L",\"underruns\":" + std::to_wstring(st.underruns) +
             L",\"drift\":" + std::to_wstring(st.driftCorrections) +
             L",\"bands\":[";
        for (int b = 0; b < kSpectrumBandCount; ++b) {
            if (b) j += L',';
            wchar_t bandBuf[16];
            swprintf(bandBuf, 16, L"%.3f", st.spectrumBands[b]);
            j += bandBuf;
        }
        j += L"]}";
    }
    j += L"]}";
    PostJson(j);
}

// Progress of the mic calibration pass, polled by IDT_CAL_TIMER -- the
// modal's per-output rows render straight from this.
void SendCalStatus() {
    auto snap = g_calibrator.snapshot();
    std::wstring j = L"{\"type\":\"cal\",\"running\":" + BoolJson(snap.running) +
                     L",\"cancelled\":" + BoolJson(snap.cancelled) +
                     L",\"error\":\"" + JsonEscape(snap.error) + L"\",\"outputs\":[";
    for (size_t i = 0; i < snap.outcomes.size(); ++i) {
        const auto& o = snap.outcomes[i];
        const wchar_t* st = o.state == CalState::Pending   ? L"pending"
                          : o.state == CalState::Measuring ? L"measuring"
                          : o.state == CalState::Done      ? L"done"
                                                           : L"failed";
        if (i) j += L',';
        j += L"{\"id\":\"" + JsonEscape(o.deviceId) + L"\",\"state\":\"" + st +
             L"\",\"delay\":" + std::to_wstring(o.delayMs) + L"}";
    }
    j += L"]}";
    PostJson(j);
}

void SendError(const std::wstring& message) {
    PostJson(L"{\"type\":\"error\",\"message\":\"" + JsonEscape(message) + L"\"}");
}

void SendInfo(const std::wstring& message) {
    PostJson(L"{\"type\":\"info\",\"message\":\"" + JsonEscape(message) + L"\"}");
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
}

// --- Setup switcher popup (Ctrl+Alt+S) ---

// ensureRunning: force the group to (re)start even if it wasn't already
// running -- used by the setup row's dedicated play button. Other callers
// (hotkeys, tray menu, switcher popup) leave it false: a quick preset switch
// shouldn't start audio that wasn't already playing.
void ApplySetup(HWND hwnd, const std::wstring& name, bool ensureRunning = false); // defined with the setup actions below
void OpenOptionsWindow(HWND owner); // defined with the WebView2 bootstrap below

void CloseSwitcherHook() {
    if (g_switcherWheelHook) {
        UnhookWindowsHookEx(g_switcherWheelHook);
        g_switcherWheelHook = nullptr;
    }
}

namespace switcher {

// Mirrors the web UI theme (ui.html :root variables).
struct Palette { COLORREF bg, border, text, muted, faint; };

Palette CurrentPalette() {
    if (EffectiveLightTheme()) {
        return { RGB(0xFF, 0xFF, 0xFF), RGB(0xDC, 0xDF, 0xE4), RGB(0x1B, 0x1D, 0x22),
                 RGB(0x5B, 0x62, 0x70), RGB(0x9A, 0xA1, 0xAD) };
    }
    return { RGB(0x15, 0x18, 0x1E), RGB(0x2A, 0x30, 0x3C), RGB(0xEC, 0xEE, 0xF2),
             RGB(0x98, 0xA0, 0xAE), RGB(0x5C, 0x63, 0x70) };
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

    HFONT fontTitle = CreateFontW(-s(11), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT fontRow = CreateFontW(-s(15), 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET,
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
            HBRUSH b = CreateSolidBrush(GetWindowsAccentColor());
            HBRUSH oldB = static_cast<HBRUSH>(SelectObject(hdc, b));
            HPEN oldP = static_cast<HPEN>(SelectObject(hdc, GetStockObject(NULL_PEN)));
            RoundRect(hdc, row.left, row.top, row.right, row.bottom, s(10), s(10));
            SelectObject(hdc, oldP);
            SelectObject(hdc, oldB);
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
        drawText(g_setups[i].name, nameRc, cur ? RGB(255, 255, 255) : pal.text, fontRow,
                 DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        drawText(cnt, cntRc, cur ? RGB(230, 228, 255) : pal.faint, fontSmall, DT_RIGHT | DT_VCENTER);
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

// --- Automatic latency optimizer ---

// Total underruns/drops across every active output right now -- the signal
// the optimizer watches. Rising during a window means the current cushion is
// too low for this machine's scheduling.
uint64_t TotalGlitches() {
    uint64_t total = 0;
    for (const auto& st : g_engine.getStatus()) {
        total += st.underruns + st.driftCorrections;
    }
    return total;
}

void ApplyOptimizerCushion(int ms) {
    g_baseCushionMs = ClampCushionMs(ms);
    g_engine.setBaseCushionMs(g_baseCushionMs);
    g_engine.resyncAllOutputs();
}

void StopOptimizer(HWND hwnd, const std::wstring& infoMsg) {
    KillTimer(hwnd, IDT_OPT_TIMER);
    g_optimizing = false;
    SaveConfig();
    SendState();
    if (!infoMsg.empty()) SendInfo(infoMsg);
}

// One measurement window. Steps the cushion down while clean; on the first
// glitchy window it backs off (raises the cushion) and stops once clean
// again, landing on the lowest stable value.
void OptimizerTick(HWND hwnd) {
    if (!g_engine.isRunning()) { StopOptimizer(hwnd, L"Optimisation interrompue : le groupe s'est arrêté."); return; }
    if (++g_optTicks > kOptMaxTicks) { StopOptimizer(hwnd, L"Optimisation terminée."); return; }

    if (g_optSettle) {
        // A cushion change just landed; ignore its transient and start a
        // fresh clean-window measurement from here.
        g_optSettle = false;
        g_optBaselineGlitches = TotalGlitches();
        return;
    }

    bool glitched = TotalGlitches() > g_optBaselineGlitches;
    if (glitched) {
        if (g_baseCushionMs >= kMaxBaseCushionMs) {
            StopOptimizer(hwnd, L"Optimisation : craquements persistants même à la marge maximale. "
                                L"C'est probablement le lien Bluetooth, pas AudioLinker.");
            return;
        }
        ApplyOptimizerCushion(g_baseCushionMs + kOptStepMs);
        g_optBackingOff = true;
        g_optSettle = true;
        SendState(); // cushion slider follows live
    } else {
        if (g_optBackingOff) {
            StopOptimizer(hwnd, L"Optimisation terminée : marge stable trouvée à " +
                                std::to_wstring(g_baseCushionMs) + L" ms.");
            return;
        }
        if (g_baseCushionMs <= kMinBaseCushionMs) {
            StopOptimizer(hwnd, L"Optimisation terminée : latence minimale (" +
                                std::to_wstring(g_baseCushionMs) + L" ms) atteinte sans craquement.");
            return;
        }
        ApplyOptimizerCushion(g_baseCushionMs - kOptStepMs);
        g_optSettle = true;
        SendState();
    }
}

void StartOptimizer(HWND hwnd) {
    if (g_optimizing) return;
    if (!g_engine.isRunning()) {
        SendError(L"Démarrez le groupe avant de lancer l'optimisation de la latence.");
        return;
    }
    g_optimizing = true;
    g_optBackingOff = false;
    g_optSettle = true; // first window just seeds the baseline
    g_optTicks = 0;
    g_optBaselineGlitches = TotalGlitches();
    SetTimer(hwnd, IDT_OPT_TIMER, kOptWindowMs, nullptr);
    SendInfo(L"Optimisation en cours… la marge est réduite progressivement, "
             L"gardez une lecture audio active pendant la mesure.");
    SendState();
}

// --- Group start/stop ---

void OnCreateGroup(HWND hwnd) {
    // The sweep must stay the only thing playing while a mic calibration
    // pass runs -- and it will restart the group itself if it stopped one.
    if (g_calibrating) {
        SendError(L"Patientez : calage micro en cours.");
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
        cfg.muted = s.muted;
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
        SetDefaultRenderDevice(cableRender->id);
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
        UnrouteAllApps();
        if (!g_previousDefaultId.empty()) SetDefaultRenderDevice(g_previousDefaultId);
        SendError(error);
        return;
    }

    // The timer does double duty: while isStarting(), it polls for that to
    // finish (see WM_TIMER); once running, it drives the VU meters.
    g_playingSetup = g_activeSetup;
    g_awaitingGroupStart = true;
    SetTimer(hwnd, IDT_STATUS_TIMER, 250, nullptr);
    SendState();
}

void OnStop(HWND hwnd) {
    KillTimer(hwnd, IDT_STATUS_TIMER);
    if (g_optimizing) { KillTimer(hwnd, IDT_OPT_TIMER); g_optimizing = false; }
    g_awaitingGroupStart = false;
    g_playingSetup.clear();
    g_engine.stop();
    UnrouteAllApps(); // put captured apps back on their normal output
    if (!g_previousDefaultId.empty()) {
        SetDefaultRenderDevice(g_previousDefaultId);
        g_previousDefaultId.clear();
    }
    SendState();
}

void ToggleGroup(HWND hwnd) {
    if (g_engine.isRunning()) OnStop(hwnd);
    else if (!g_engine.isStarting()) OnCreateGroup(hwnd);
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
    SaveSetupsToIni();
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
    if (wasRunning) OnStop(hwnd);
    if (!MergeSetupIntoLiveSettings(name)) return;
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
    SaveSetupsToIni();
    SendState();
}

// --- Options: export/import the whole config file ---

void ExportConfig(HWND hwnd) {
    // Everything lives in g_configPath already (SaveConfig/SaveSetupsToIni/
    // SaveRoutedAppsToIni write straight to it as things change) -- flush
    // once more just before copying so the export reflects this exact moment.
    SaveConfig();
    SaveSetupsToIni();
    SaveRoutedAppsToIni();

    wchar_t path[MAX_PATH] = L"AudioLinker.ini";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Fichier de configuration (*.ini)\0*.ini\0Tous les fichiers (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"ini";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return; // cancelled

    if (CopyFileW(g_configPath.c_str(), path, FALSE)) {
        SendInfo(L"Configuration exportée.");
    } else {
        SendError(L"Échec de l'export (erreur " + std::to_wstring(GetLastError()) + L").");
    }
}

void ImportConfig(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Fichier de configuration (*.ini)\0*.ini\0Tous les fichiers (*.*)\0*.*\0";
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

    if (!CopyFileW(path, g_configPath.c_str(), FALSE)) {
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
        return;
    }
    if (cmd == L"toggle") { ToggleGroup(hwnd); return; }
    if (cmd == L"autolatency") {
        // Align every active speaker on the slowest one, using the device-
        // reported playback lag (WASAPI buffer + driver/Bluetooth buffering).
        if (!g_engine.isRunning()) {
            SendError(L"Démarrez le groupe avant de caler les latences.");
            return;
        }
        auto status = g_engine.getStatus();
        struct Lag { std::wstring id; int ms; };
        std::vector<Lag> lags;
        int maxLag = -1;
        std::wstring slowestId;
        bool notReady = false;
        for (const auto& st : status) {
            auto it = g_settings.find(st.deviceId);
            if (it == g_settings.end() || !it->second.checked || !st.active) continue;
            if (st.deviceLagMs <= 0) notReady = true;
            lags.push_back({ st.deviceId, st.deviceLagMs });
            if (st.deviceLagMs > maxLag) {
                maxLag = st.deviceLagMs;
                slowestId = st.deviceId;
            }
        }
        if (lags.size() < 2) {
            SendError(L"Il faut au moins deux enceintes actives pour les caler entre elles.");
            return;
        }
        if (notReady) {
            SendError(L"Mesure du retard en cours… réessayez dans quelques secondes.");
            return;
        }
        for (const auto& lag : lags) {
            int v = maxLag - lag.ms;
            v = ClampLatencyMs(v);
            g_settings[lag.id].latencyMs = v;
            g_engine.setLatencyMs(lag.id, v);
            g_engine.resyncOutput(lag.id); // snap to the new target within ~1s
        }
        g_activeSetup.clear(); // latencies just diverged from any preset
        SaveConfig();
        SendState();
        std::wstring slowestName = slowestId;
        for (const auto& d : g_outputDevices) {
            if (d.id != slowestId) continue;
            const auto& s = g_settings[d.id];
            slowestName = s.alias.empty() ? d.name : s.alias;
        }
        SendInfo(L"Latences calées sur « " + slowestName + L" » (l'enceinte la plus lente, " +
                 std::to_wstring(maxLag) + L" ms). Ajustement effectif d'ici 1 à 2 secondes.");
        return;
    }
    if (cmd == L"calstart") {
        // calstart|micId -- mic-based automatic latency alignment (see
        // Calibrator.h). The group must be silent during the sweeps; if it
        // was playing it is stopped here and restarted once the pass ends.
        if (g_engine.isStarting()) {
            SendError(L"Patientez : le groupe est en cours de démarrage.");
            return;
        }
        if (g_calibrating || rest.empty()) return;
        std::vector<std::wstring> ids;
        for (const auto& d : g_outputDevices) {
            if (g_settings[d.id].checked) ids.push_back(d.id);
        }
        if (ids.size() < 2) {
            SendError(L"Cochez au moins deux enceintes à caler entre elles.");
            return;
        }
        g_calRestartAfter = g_engine.isRunning();
        if (g_calRestartAfter) OnStop(hwnd);
        if (!g_calibrator.start(rest, ids, g_deviceFillMs)) return;
        g_calibrating = true;
        SetTimer(hwnd, IDT_CAL_TIMER, 250, nullptr);
        SendCalStatus();
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
    if (cmd == L"identify") {
        // identify|deviceId -- plays a couple of short beeps directly to
        // that one device, independent of the group, so the user can tell
        // which list entry is which physical speaker.
        g_engine.identifyOutput(rest);
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

    constexpr int kOptionsW = 480, kOptionsH = 400;
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
    if (g_webviewController) g_webviewController->put_IsVisible(TRUE);
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
            InitConfigPath();
            g_autostart = GetPrivateProfileIntW(L"group", L"autostart", 0, g_configPath.c_str()) != 0;
            g_autostartPending = g_autostart;
            g_masterVolumePercent = ClampVolume(
                static_cast<int>(GetPrivateProfileIntW(L"group", L"mastervolume", 100, g_configPath.c_str())));
            g_baseCushionMs = ClampCushionMs(static_cast<int>(GetPrivateProfileIntW(
                L"group", L"cushionms", kDefaultBaseCushionMs, g_configPath.c_str())));
            g_engine.setBaseCushionMs(g_baseCushionMs);
            g_deviceFillMs = ClampDeviceFillMs(static_cast<int>(GetPrivateProfileIntW(
                L"group", L"devicefillms", kDefaultDeviceFillMs, g_configPath.c_str())));
            g_engine.setDeviceFillMs(g_deviceFillMs);
            g_lowLatency = GetPrivateProfileIntW(L"group", L"lowlatency", 0, g_configPath.c_str()) != 0;
            g_engine.setLowLatency(g_lowLatency);
            g_themeOverride = ClampThemeOverride(
                static_cast<int>(GetPrivateProfileIntW(L"group", L"theme", 0, g_configPath.c_str())));
            LoadHotkeysFromIni();
            LoadSetupsFromIni();
            CleanupStaleRoutes(); // repair app routing left over by a crash

            PopulateOutputList();
            AddTrayIcon(hwnd);
            g_deviceManager->registerNotifications(&g_deviceNotifications);
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
            for (int i = 0; i < kSetupHotkeyCount; ++i) {
                RegisterHotKey(hwnd, HOTKEY_SETUP_FIRST + i, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
                               static_cast<UINT>('1' + i));
            }
            InitWebView(hwnd);
            return 0;
        }
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                ShowWindow(hwnd, SW_HIDE);
                if (g_webviewController) g_webviewController->put_IsVisible(FALSE);
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
            ShowWindow(hwnd, SW_HIDE);
            if (g_webviewController) g_webviewController->put_IsVisible(FALSE);
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
                SendState();
                return 0;
            }
            if (wParam == IDT_OPT_TIMER) {
                if (g_optimizing) OptimizerTick(hwnd);
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
                        KillTimer(hwnd, IDT_STATUS_TIMER);
                        UnrouteAllApps();
                        if (!g_previousDefaultId.empty()) {
                            SetDefaultRenderDevice(g_previousDefaultId);
                            g_previousDefaultId.clear();
                        }
                        SendState();
                        SendError(error);
                        return 0;
                    }
                    SaveConfig();
                    SendState();
                    return 0;
                }
                SendStatus();
                if (!g_engine.isRunning()) {
                    // Engine stopped unexpectedly (e.g. device unplugged)
                    OnStop(hwnd);
                }
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
                UnrouteAllApps();
                if (!g_previousDefaultId.empty()) {
                    SetDefaultRenderDevice(g_previousDefaultId);
                    g_previousDefaultId.clear();
                }
            }
            return 0;
        case WM_DESTROY:
            // Skipped after a config import: the imported file must reach
            // the relaunched instance untouched (see ImportConfig).
            if (!g_skipSaveOnExit) SaveConfig();
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
            g_deviceManager->unregisterNotifications(&g_deviceNotifications);
            UnregisterHotKey(hwnd, HOTKEY_TOGGLE_GROUP);
            UnregisterHotKey(hwnd, HOTKEY_SETUP_SWITCHER);
            for (int i = 0; i < kSetupHotkeyCount; ++i) {
                UnregisterHotKey(hwnd, HOTKEY_SETUP_FIRST + i);
            }
            Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
            g_engine.stop();
            UnrouteAllApps();
            if (!g_previousDefaultId.empty()) {
                SetDefaultRenderDevice(g_previousDefaultId);
            }
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

} // namespace

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

    // Pre-load just the theme override (WM_CREATE loads the rest of the ini
    // once the window exists) so the very first class background brush,
    // created below before the window does, already reflects it.
    InitConfigPath();
    g_themeOverride = ClampThemeOverride(
        static_cast<int>(GetPrivateProfileIntW(L"group", L"theme", 0, g_configPath.c_str())));

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

    // Per-monitor DPI aware: scale the default (logical-pixel) size by the
    // system DPI for the initial creation; WM_DPICHANGED takes over if the
    // window then lands on a monitor with a different scale.
    UINT dpi = GetDpiForSystem();
    if (dpi == 0) dpi = 96;
    RECT r = { 0, 0, MulDiv(kDefaultClientW, static_cast<int>(dpi), 96),
                     MulDiv(kDefaultClientH, static_cast<int>(dpi), 96) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
                             FALSE, 0, dpi);
    HWND hwnd = CreateWindowExW(0, className, L"AudioLinker — Groupe d'enceintes",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME |
            WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
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
