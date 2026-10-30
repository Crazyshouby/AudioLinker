#pragma once

// Internal header of the GUI side (everything under src/ except the engine,
// calibrator and config modules, which have their own public headers).
// Declares the state shared by the GUI translation units and the functions
// they call across files. Definitions live in the file named in each
// section's comment.

#include "Gui.h"
#include "DeviceManager.h"
#include "AudioEngine.h"
#include "Calibrator.h"
#include "Config.h"
#include "Log.h"
#include "PolicyConfig.h"
#include "Utf8.h"
#include "resource.h"
#include "version.h"

#include <shellapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl.h>
#include <wrl/client.h>
#include <WebView2.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <shellscalingapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::ordered_json;

namespace al {

// --- Constants ---

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
// Bounds a hotkey capture (see StartHotkeyCapture): the low-level hook
// swallows every keystroke system-wide while armed, so it must never be
// left waiting forever.
constexpr UINT_PTR IDT_HK_CAPTURE = 5;
constexpr int kHotkeyCaptureTimeoutMs = 10000;
// Delay between a resume-from-sleep notification and the group restart.
constexpr UINT_PTR IDT_RESUME = 6;
// Deferred config write (see SaveConfig): coalesces the burst of changes a
// slider drag or a setup apply produces into one file write.
constexpr UINT_PTR IDT_SAVE = 7;
constexpr int kSaveDelayMs = 500;
// Polls AudioEngine::isStopping() after an asynchronous stop, then runs
// whatever was waiting for it (see AfterStop / RunAfterStop).
constexpr UINT_PTR IDT_STOP_POLL = 8;
// Delay between a speaker's (re)appearance and the setup its automation
// rule starts: a Bluetooth endpoint is enumerated before its stream is
// really ready to open.
constexpr UINT_PTR IDT_AUTOMATION = 9;
constexpr int kAutomationDelayMs = 1500;
// Level meters (VU bars, spectrum): faster than the status timer so the
// bars move instead of stepping, with a message that carries nothing else.
constexpr UINT_PTR IDT_VU_TIMER = 10;
// Polls the KDX suite settings (%APPDATA%\KDX\suite.json) for an accent change, see KdxLink.cpp.
constexpr UINT_PTR IDT_SUITE = 11;
constexpr int kSuiteIntervalMs = 250;
constexpr int kVuIntervalMs = 100;
constexpr int kStatusIntervalMs = 250;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_DEVICES_CHANGED = WM_APP + 2;
// Windows' default render endpoint changed (see DeviceNotificationClient).
constexpr UINT WM_DEFAULT_CHANGED = WM_APP + 3;
// A line of the diagnostic log reached the Journal (see Journal.cpp); the
// LPARAM owns a heap-allocated JournalLine.
constexpr UINT WM_JOURNAL = WM_APP + 4;
// The link with the other KDX applications (KdxLink.cpp): WPARAM 0 = a client connected, 1 = a command (LPARAM owns a
// heap-allocated std::wstring), 2 = the client asks for the state.
constexpr UINT WM_KDX_LINK = WM_APP + 5;
constexpr int HOTKEY_TOGGLE_GROUP = 1;   // Ctrl+Alt+G
constexpr int HOTKEY_SETUP_SWITCHER = 2; // Ctrl+Alt+S
// Ctrl+Shift+1..9 applies setup #1..9 directly. Ctrl+Alt+digit is off-limits:
// that's AltGr+digit on French layouts (~ # { [ | \ ^ @...).
constexpr int HOTKEY_SETUP_FIRST = 10;
constexpr int kSetupHotkeyCount = 9;

// The floating windows of the UI (Panels.cpp). The launcher is the app's main
// window; the others are created when opened.
enum class PanelId { Launcher, Outputs, Setups, Levels, Journal, Options };
constexpr int kPanelCount = 6;
constexpr int kLauncherW = 224; // logical px; the launcher has a fixed size
constexpr int kLauncherH = 520;

// Latency optimizer (see Optimizer.cpp).
constexpr int kOptStepMs = 20;
constexpr int kOptWindowMs = 3500;
constexpr int kOptMaxTicks = 60; // hard stop (~3.5min) against any pathological loop

// --- Shared state (AppState.cpp) ---

extern ConfigStore g_store;
extern AppConfig g_cfg;
extern COLORREF g_windowBg;
extern int& g_themeOverride;
extern std::unique_ptr<DeviceManager> g_deviceManager;
extern AudioEngine g_engine;
extern Calibrator g_calibrator;
extern bool g_calibrating;
extern bool g_calRestartAfter;
extern bool g_optimizing;
extern bool g_optSettle;
extern bool g_optBackingOff;
extern uint64_t g_optBaselineGlitches;
extern int g_optTicks;
extern std::vector<AudioDeviceInfo> g_outputDevices;
extern std::map<std::wstring, OutputSettings>& g_settings;
extern std::vector<Setup>& g_setups;
extern std::wstring& g_activeSetup;
extern std::wstring& g_editingSetup;
extern std::wstring g_playingSetup;
extern std::wstring& g_previousDefaultId;
extern bool& g_autostart;
extern int& g_masterVolumePercent;
extern std::wstring& g_setupMicId;
extern bool g_micMetering;
extern bool g_statusTimerForGroup;
// Whole-group mute (header button): applied on top of every output's own
// mute, never persisted -- a quick "shut up" that survives neither a
// restart of the app nor a setup change.
extern bool g_groupMuted;
// "No cable" mode, while a group runs: the real default output, left out of
// the group and playing natively (empty otherwise). The page tags its row.
extern std::wstring g_nativeReferenceId;
extern int& g_baseCushionMs;
extern int& g_deviceFillMs;
extern bool& g_lowLatency;
extern bool& g_setupDigitHotkeys;
extern std::vector<std::wstring> g_pendingJson; // toasts posted while no window was visible
extern ULONGLONG g_lastDefaultChangeTick;
extern UINT& g_toggleHotkeyMods;
extern UINT& g_toggleHotkeyVk;
extern UINT& g_switcherHotkeyMods;
extern UINT& g_switcherHotkeyVk;
extern HHOOK g_hotkeyCaptureHook;
extern int g_hotkeyCaptureId;
extern bool g_awaitingGroupStart;
extern bool g_autostartPending;
extern bool g_skipSaveOnExit;
extern std::wstring g_startupWarning;
extern UINT g_showAppMessage;
extern HBRUSH g_hBrushWindowBg;
extern HICON g_hAppIcon;
extern NOTIFYICONDATAW g_trayIcon;
extern ComPtr<ICoreWebView2Environment> g_webviewEnvironment; // shared by every window
extern HHOOK g_switcherWheelHook;
extern HWND g_switcherWnd;
extern HWND g_mainWnd;
// Apps routed into the cable for the current group session (SystemAudio.cpp).
extern std::vector<DWORD> g_routedPids;

COLORREF GetWindowsAccentColor();
bool IsWindowsLightThemeEnabled();
bool EffectiveLightTheme();
COLORREF WindowBgForTheme(bool light);

// --- Gui.cpp: launcher window procedure, RunApplication ---
std::wstring LoadHtmlResource(int resId);
// Common settings of the WebView2 pages: no context menu / devtools /
// accelerator keys, and no navigation away from the embedded page.
void HardenWebView(ICoreWebView2* webview);
void SaveWindowPlacement(HWND hwnd);
bool LoadWindowPlacement(RECT& out);

// --- Panels.cpp: the floating windows ---
// Square corners, no DWM outline: the page draws the whole frame.
void ApplyFlatFrame(HWND hwnd);
const char* PanelKey(PanelId id);
bool PanelFromKey(const std::wstring& key, PanelId& out);
bool IsPanelOpen(PanelId id);
bool IsPanelShaded(PanelId id);
bool IsLauncherHidden();
bool AnyPanelVisible();
// Called once the launcher's HWND exists: which windows were open last time.
void AttachLauncherWindow(HWND hwnd, bool startHidden);
// Creates the shared WebView2 environment, then the launcher page and the
// windows that were open.
void InitPanelWebViewEnvironment(HWND launcher);
void OpenPanel(PanelId id);
void ClosePanel(PanelId id); // the launcher: hides everything to the tray
void TogglePanel(PanelId id);
void ShadePanel(PanelId id, int mode); // mode: -1 toggle, 0 unroll, 1 roll up
// JSON to one window / every open window / the one window a toast belongs in
// (false = no window is showing).
void PostToPanel(PanelId id, const std::wstring& jsonText);
void PostToPanels(const std::wstring& jsonText);
bool PostToastTarget(const std::wstring& jsonText);
void SavePanelPlacements();
void PanelsHideToTray();
void PanelsRestoreFromTray();
void DestroyAllPanels();
void SuspendLauncherPage();
void ResumeLauncherPage();
void HideLauncherToTray();
void ShowLauncherFromTray();
// Chrome and window commands posted by a page; false = not one of them.
bool HandlePanelMessage(PanelId from, const std::wstring& cmd, const std::wstring& rest);
// WM_* shared by the launcher and the other windows; handled=false when the
// message is not one of theirs.
LRESULT PanelCommonProc(PanelId id, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool& handled);

// --- Journal.cpp: the Journal window's data ---
struct JournalLine {
    long long ts; // unix ms
    int level;    // 0 info, 1 warning, 2 error
    std::wstring text;
};
// Hooks the diagnostic log (Log.h) so every line also reaches the Journal.
void JournalInstall();
// UI thread only. Adds a line and pushes it to the Journal window if open.
void JournalAdd(int level, const std::wstring& text);
// Takes ownership of a JournalLine posted with WM_JOURNAL.
void JournalFromMessage(LPARAM lParam);
void SendJournal(PanelId to);
void JournalClear(); // empties the ring and every open Journal window

// --- Devices.cpp ---
bool IsVirtualDevice(const std::wstring& name);
bool IsCableDevice(const std::wstring& name);
IMMNotificationClient& DeviceNotificationSink();
void SortOutputDevicesByOrder();
void PopulateOutputList();
// Setup an automation rule asked to start, applied by IDT_AUTOMATION.
extern std::wstring g_pendingAutoSetup;

// --- Group.cpp: group lifecycle, setups, config saving ---
void FlushConfig();
void SaveConfig();
int EffectiveVolume(const OutputSettings& s);
// Starts/stops the status and VU timers according to who needs them (a
// playing group, the INPUTS tab's mic metering). Call after changing
// g_statusTimerForGroup or g_micMetering.
void UpdateMeterTimers();
// What the engine should actually mute for this output: its own flag or
// the whole-group mute.
bool EffectiveMuted(const OutputSettings& s);
void OnCreateGroup(HWND hwnd);
void OnStop(HWND hwnd);
void ToggleGroup(HWND hwnd);
// What to do once an asynchronous stop (AudioEngine::stopAsync) completes:
// OnCreateGroup and BeginMicCalibration queue themselves here when they
// find the engine still winding down, and IDT_STOP_POLL fires RunAfterStop.
enum class AfterStop { None, Start, Calibrate };
void QueueAfterStop(HWND hwnd, AfterStop action, const std::wstring& arg = L"");
void RunAfterStop(HWND hwnd);
// Mic-based latency alignment pass (stops a running group first and
// restarts it once the pass ends).
void BeginMicCalibration(HWND hwnd, const std::wstring& micId);
void SaveSetupFromCurrent(std::wstring name);
// ensureRunning: force the group to (re)start even if it wasn't already
// running -- used by the setup row's dedicated play button. Other callers
// (hotkeys, tray menu, switcher popup) leave it false: a quick preset switch
// shouldn't start audio that wasn't already playing.
void ApplySetup(HWND hwnd, const std::wstring& name, bool ensureRunning = false);
void SelectSetup(const std::wstring& name);
void DeleteSetup(const std::wstring& name);
void ExportConfig(HWND hwnd);
void ImportConfig(HWND hwnd);

// --- Hotkeys.cpp ---
std::wstring FormatHotkey(UINT mods, UINT vk);
void UpdateSetupDigitHotkeys(HWND hwnd);
bool RebindHotkey(HWND hwnd, int id, UINT& modsVar, UINT& vkVar, UINT newMods, UINT newVk);
void StopHotkeyCapture();
void StartHotkeyCapture(int id);

// --- SystemAudio.cpp ---
bool IsLaunchAtStartupEnabled();
void SetLaunchAtStartup(bool enable);
bool RenameEndpoint(const std::wstring& deviceId, const std::wstring& newName);
bool SetDefaultEndpointAllRoles(const std::wstring& deviceId);
std::wstring GetDefaultRenderId();
void PersistPreviousDefault();
bool RestoreDefaultRender(const std::wstring& preferredId);
bool GetCaptureVolume(const std::wstring& deviceId, int& volPercent, bool& muted);
bool SetCaptureVolume(const std::wstring& deviceId, int volPercent);
bool SetCaptureMute(const std::wstring& deviceId, bool muted);
void StartMicMetering();
void StopMicMetering();
struct SourceApp {
    DWORD pid;
    std::wstring name;
};
std::vector<SourceApp> EnumerateSourceApps();
bool SetAppRenderEndpoint(DWORD pid, const std::wstring& mmDeviceId);
void UnrouteAllApps();
void RestoreDefaultAndUnroute();
void SaveRoutedAppsToIni();
void RepairAfterCrash();

// --- WebBridge.cpp ---
void PostJson(const json& j);
void SendState();
void SendStateTo(PanelId to);
// Replays toasts that found no window to show them (see RememberHiddenToast).
void FlushPendingToasts();
void SendSources();
void SendStatus();
void SendPeaks();
void SendCalStatus();
// Persistent notice under the main page's header; empty message = clear.
void SendBanner(const std::wstring& message, const std::wstring& actionCmd = L"",
                const std::wstring& actionLabel = L"");
void SendError(const std::wstring& message);
void SendInfo(const std::wstring& message);
void SendErrorWithAction(const std::wstring& message, const std::wstring& command,
                         const std::wstring& label);
void HandleWebMessage(HWND hwnd, const std::wstring& msg);
json BuildState();

// --- KdxLink.cpp ---
void KdxLinkStart();
void KdxLinkStop();
void KdxLinkPublish(const json& state, bool force = false);
void KdxLinkHandle(HWND hwnd, WPARAM what, LPARAM param);
// A notification for the KDX shell's quick centre (no answer expected, sent from a worker thread, dropped if the shell is not running).
void KdxNotifyShell(const char* level, const std::wstring& text);
void KdxSuiteStart(HWND hwnd);
void KdxSuiteTick();
const std::string& KdxSuiteAccent();

// --- Switcher.cpp ---
void CloseSwitcherHook();
namespace switcher {
void Open();
}

// --- Optimizer.cpp ---
void StartOptimizer(HWND hwnd);
void StopOptimizer(HWND hwnd, const std::wstring& infoMsg);
void OptimizerTick(HWND hwnd);

// --- Tray.cpp ---
void AddTrayIcon(HWND hwnd);
void ShowTrayBalloon(const std::wstring& text, DWORD iconFlag);
void ShowTrayMenu(HWND hwnd);
void RestoreFromTray(HWND hwnd);
// Icon variant (plain / "group active" dot) and tooltip follow the engine
// state; cheap to call often, only talks to the shell on a change.
void UpdateTrayState();

} // namespace al
