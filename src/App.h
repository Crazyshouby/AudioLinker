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
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_DEVICES_CHANGED = WM_APP + 2;
// Windows' default render endpoint changed (see DeviceNotificationClient).
constexpr UINT WM_DEFAULT_CHANGED = WM_APP + 3;
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
extern int& g_baseCushionMs;
extern int& g_deviceFillMs;
extern bool& g_lowLatency;
extern bool& g_setupDigitHotkeys;
extern std::vector<std::wstring> g_pendingJson;
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
extern HBRUSH g_optionsHBrushWindowBg;
extern HICON g_hAppIcon;
extern NOTIFYICONDATAW g_trayIcon;
extern ComPtr<ICoreWebView2Controller> g_webviewController;
extern ComPtr<ICoreWebView2> g_webview;
extern EventRegistrationToken g_webMessageToken;
extern ComPtr<ICoreWebView2Environment> g_webviewEnvironment;
extern HWND g_optionsWnd;
extern ComPtr<ICoreWebView2Controller> g_optionsController;
extern ComPtr<ICoreWebView2> g_optionsWebview;
extern EventRegistrationToken g_optionsMessageToken;
extern HHOOK g_switcherWheelHook;
extern HWND g_switcherWnd;
extern HWND g_mainWnd;
// Apps routed into the cable for the current group session (SystemAudio.cpp).
extern std::vector<DWORD> g_routedPids;

COLORREF GetWindowsAccentColor();
bool IsWindowsLightThemeEnabled();
bool EffectiveLightTheme();
COLORREF WindowBgForTheme(bool light);

// --- Gui.cpp: main window ---
void ApplyWindowTheme(HWND hwnd);
std::wstring LoadHtmlResource(int resId);
void SuspendMainWebView();
void ResumeMainWebView();
void SaveWindowPlacement(HWND hwnd);
bool LoadWindowPlacement(RECT& out);

// --- Devices.cpp ---
bool IsVirtualDevice(const std::wstring& name);
bool IsCableDevice(const std::wstring& name);
IMMNotificationClient& DeviceNotificationSink();
void SortOutputDevicesByOrder();
void PopulateOutputList();

// --- Group.cpp: group lifecycle, setups, config saving ---
void FlushConfig();
void SaveConfig();
int EffectiveVolume(const OutputSettings& s);
void OnCreateGroup(HWND hwnd);
void OnStop(HWND hwnd);
void ToggleGroup(HWND hwnd);
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
void SendSources();
void SendStatus();
void SendCalStatus();
void SendError(const std::wstring& message);
void SendInfo(const std::wstring& message);
void SendErrorWithAction(const std::wstring& message, const std::wstring& command,
                         const std::wstring& label);
void HandleWebMessage(HWND hwnd, const std::wstring& msg);

// --- Switcher.cpp ---
void CloseSwitcherHook();
namespace switcher {
void Open();
}

// --- Optimizer.cpp ---
void StartOptimizer(HWND hwnd);
void StopOptimizer(HWND hwnd, const std::wstring& infoMsg);
void OptimizerTick(HWND hwnd);

// --- OptionsWindow.cpp ---
void ApplyOptionsWindowTheme();
void OpenOptionsWindow(HWND owner);

// --- Tray.cpp ---
void AddTrayIcon(HWND hwnd);
void ShowTrayBalloon(const std::wstring& text, DWORD iconFlag);
void ShowTrayMenu(HWND hwnd);
void RestoreFromTray(HWND hwnd);

} // namespace al
