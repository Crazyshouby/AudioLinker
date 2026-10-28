// Definitions of the state shared by the GUI's translation units (declared
// extern in App.h): persisted config, engine, device list, windows, flags.

#include "App.h"

namespace al {


// Everything persisted (see Config.h) and its store. Loaded once in
// RunApplication before any window exists; written back on a short timer
// after each change (SaveConfig) and flushed at exit (FlushConfig).
ConfigStore g_store;
AppConfig g_cfg;


COLORREF g_windowBg = RGB(0x14, 0x14, 0x14);

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
    return RGB(0x3B, 0x82, 0xF6); // matches --accent in common.css
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
// Options window's theme selector.
int& g_themeOverride = g_cfg.theme;

// What every theme-driven decision (native chrome + the web content's
// data-theme attribute) should actually use instead of the raw OS reading.
// The KDX-style look has a single dark theme: neither the OS setting nor the
// old Options override (g_themeOverride, kept so existing configs still load)
// changes it any more.
bool EffectiveLightTheme() {
    return false;
}

// Must match common.css's --frame (the host brush shows through while the
// web content repaints after a resize).
COLORREF WindowBgForTheme(bool /*light*/) {
    return RGB(0x22, 0x22, 0x22);
}

// OutputSettings / Setup live in Config.h with the rest of the persisted state.

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
std::vector<AudioDeviceInfo> g_outputDevices;
// Short names for the persisted pieces the GUI code touches constantly.
std::map<std::wstring, OutputSettings>& g_settings = g_cfg.devices;
std::vector<Setup>& g_setups = g_cfg.setups;
// Name of the last setup applied/saved, highlighted in the UI. Cleared as
// soon as any individual setting is touched: the current state no longer
// matches the preset, so keeping it highlighted would lie.
std::wstring& g_activeSetup = g_cfg.activeSetup;
// Unlike g_activeSetup, this does NOT clear on divergence -- it's the setup a
// "save changes" button in the UI would overwrite, so it must survive the
// very edits that make it useful (checking a device, tweaking a slider...).
// Set alongside g_activeSetup by ApplySetup()/SaveSetupFromCurrent(); only a
// different setup being applied/saved, or this one being deleted, changes it.
std::wstring& g_editingSetup = g_cfg.editingSetup;
// The setup whose config the engine is actually running, set when a group
// (successfully) starts and cleared on stop. Deliberately distinct from
// g_activeSetup: selecting a setup for editing (SelectSetup()) reassigns
// g_activeSetup without touching playback, so it alone can't tell the UI
// which row's play button should show "stop".
std::wstring g_playingSetup;
// Default output displaced by the running group (persisted so a crash can be
// repaired at the next launch -- see RepairAfterCrash). Empty = none.
std::wstring& g_previousDefaultId = g_cfg.previousDefault;
bool& g_autostart = g_cfg.autostart;
// Live attenuation applied on top of every output's own volume (which stays
// unchanged in g_settings/the config) -- lets one slider duck the whole
// group without losing the balance set between speakers.
int& g_masterVolumePercent = g_cfg.masterVolume;
// Mic tied to the current live configuration (INPUTS tab): snapshotted into
// setups on save, restored from them on apply (where it also becomes the
// Windows default capture device). Empty = don't manage the default mic.
std::wstring& g_setupMicId = g_cfg.micId;
// Mic VU metering is on (INPUTS tab visible). Drives who owns
// IDT_STATUS_TIMER alongside g_statusTimerForGroup: the timer runs when
// either wants it, and each stop path only kills it if the other is done.
bool g_micMetering = false;
// The status timer was started for a playing group -- distinguishes "the
// engine stopped unexpectedly" (WM_TIMER must call OnStop) from "the timer
// only runs for mic metering" (idle engine is normal).
bool g_statusTimerForGroup = false;
bool g_groupMuted = false;
std::wstring g_nativeReferenceId;
// Mirrors AudioEngine's base ring-buffer cushion (kDefaultBaseCushionMs) so
// it can be edited from the UI and persisted -- see the "cushion"/
// "cushioncommit" web commands.
int& g_baseCushionMs = g_cfg.cushionMs;
// Mirrors AudioEngine's device-buffer fill target (kDefaultDeviceFillMs) --
// see the "devicefill"/"devicefillcommit" web commands. Cushion + fill is
// the bulk of the group's total latency vs. native playback.
int& g_deviceFillMs = g_cfg.deviceFillMs;
// Low-latency period toggle (IAudioClient3) -- see AudioEngine::setLowLatency
// and the "lowlatency" web command.
bool& g_lowLatency = g_cfg.lowLatency;
// Ctrl+Maj+1..9 apply setup #1..9 directly -- optional, since those combos
// are also used by Office, editors... (see UpdateSetupDigitHotkeys).
bool& g_setupDigitHotkeys = g_cfg.setupDigitHotkeys;
// Toasts (error/info JSON) posted while the main window was hidden: the
// suspended page never saw them, so they are replayed by RestoreFromTray.
std::vector<std::wstring> g_pendingJson;
// When this app itself last changed Windows' default output: default-change
// notifications inside that window are our own doing, not the user's.
ULONGLONG g_lastDefaultChangeTick = 0;
// Customizable global hotkeys (see FormatHotkey/RebindHotkey).
UINT& g_toggleHotkeyMods = g_cfg.toggleMods;
UINT& g_toggleHotkeyVk = g_cfg.toggleVk;
UINT& g_switcherHotkeyMods = g_cfg.switcherMods;
UINT& g_switcherHotkeyVk = g_cfg.switcherVk;
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

HHOOK g_switcherWheelHook = nullptr;
HWND g_switcherWnd = nullptr;
HWND g_mainWnd = nullptr; // ApplySetup needs the main window (status timer)

// Forwards MMDevice hot-plug events to the GUI thread. Callbacks arrive on a
// COM worker thread, so nothing is touched here beyond a PostMessage; the
// window procedure debounces and re-enumerates. Stack/static lifetime, like
// the engine's EndpointVolumeCallback.

} // namespace al
