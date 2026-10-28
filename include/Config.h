#pragma once

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "AudioEngine.h" // kEqBandCount, kDefault* tuning constants

// Everything AudioLinker persists, as one plain struct, and the store that
// reads/writes it as %APPDATA%\AudioLinker\config.json. The previous
// format (%APPDATA%\AudioLinker.ini, rewritten key by key through
// WritePrivateProfileString) is migrated on first load and kept as .bak.

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
    // System name last seen for this endpoint id -- the key for recognizing
    // the same device back under a new id (re-paired Bluetooth, USB port).
    std::wstring name;
    // User-chosen display name; empty = show the system name. Cosmetic only,
    // deliberately not part of setups (renaming survives applying a preset).
    std::wstring alias;
    // User-chosen accent color for this speaker's row/icon (e.g. "e07a5f");
    // empty = use the default kind-based color. Same treatment as alias.
    std::wstring colorHex;
    // Unix time (seconds) this endpoint was last enumerated; 0 = unknown.
    // Devices unseen for kDevicePurgeDays and referenced by no setup are
    // dropped at save time so the file doesn't accumulate ghosts forever.
    long long lastSeen = 0;
    // Per-speaker source: 0 = system-wide capture (virtual cable), otherwise
    // the PID of the app captured for this speaker. Runtime-only (PIDs don't
    // survive a reboot), hence neither persisted nor stored in setups.
    unsigned long sourcePid = 0;
};

// A named snapshot of a full group configuration: every device known at save
// time with its checked state and settings. Applying one replaces the current
// selection wholesale (devices absent from the setup get unchecked).
struct Setup {
    std::wstring name;
    std::vector<std::pair<std::wstring, OutputSettings>> devices;
    // Capture endpoint made the Windows default mic when this setup is
    // applied. Empty = this setup leaves the default mic alone.
    std::wstring micId;
    // Windows input level/mute of that mic, snapshotted at save time and
    // re-applied with it. micVolume -1 = saved before this existed (or no
    // mic): leave the level alone.
    int micVolume = -1;
    bool micMute = false;
};

// "When this speaker connects/disconnects" rules (see the automation code
// in the GUI). onDisconnect: "" = nothing, "stop" = stop the group.
struct AutomationRule {
    std::wstring deviceId;
    std::wstring onConnect; // setup name to apply, empty = nothing
    std::wstring onDisconnect;
};

struct WindowPlacement {
    int x = 0, y = 0, w = 0, h = 0; // the window's normal rect
    bool valid() const { return w >= 200 && h >= 150; }
};

// One floating window of the KDX-style UI (see Panels.cpp): where it sits,
// whether it was open when the app last ran, whether it was rolled up to its
// title bar. Keyed by the panel's name ("outputs", "setups", ...); the
// launcher (main window) keeps using AppConfig::window.
struct PanelPlacement {
    int x = 0, y = 0, w = 0, h = 0; // the window's normal rect, screen px
    bool open = false;
    bool shaded = false;
    bool valid() const { return w >= 120 && h >= 60; }
};

struct AppConfig {
    bool autostart = false; // recreate the group when the app opens
    int masterVolume = 100;
    std::wstring micId; // mic tied to the live config (INPUTS tab)
    int cushionMs = kDefaultBaseCushionMs;
    int deviceFillMs = kDefaultDeviceFillMs;
    bool lowLatency = false;
    int theme = 0; // 0 = follow Windows, 1 = light, 2 = dark
    // 0 = virtual cable (the group becomes Windows' default output);
    // 1 = experimental "no cable": system audio captured by process
    // loopback excluding this app, the real default output keeps playing
    // natively and serves as the timing reference (see README).
    int captureMode = 0;
    // Default output displaced by a running group -- non-empty only while
    // one runs, so a crash leaves a record of what to restore.
    std::wstring previousDefault;

    unsigned toggleMods = 0x0002 | 0x0001; // MOD_CONTROL | MOD_ALT
    unsigned toggleVk = 'G';
    unsigned switcherMods = 0x0002 | 0x0001;
    unsigned switcherVk = 'S';
    bool setupDigitHotkeys = true;

    std::wstring activeSetup;
    std::wstring editingSetup;
    std::vector<Setup> setups;

    std::map<std::wstring, OutputSettings> devices; // keyed by endpoint id

    // Executables currently routed into the cable (crash repair, see the
    // app-routing code in the GUI).
    std::vector<std::wstring> routedApps;

    WindowPlacement window;
    std::map<std::string, PanelPlacement> panels;
    std::vector<AutomationRule> automation;
};

constexpr int kDevicePurgeDays = 90;

// Clamps shared by the loaders and the live command handlers.
int ClampDb(int v);
int ClampChannelMode(int v);
int ClampVolume(int v);
int ClampLatencyMs(int v);
int ClampCushionMs(int v);
int ClampDeviceFillMs(int v);
int ClampThemeOverride(int v);

// "a|b|c" -> {"a","b","c"} (keeps empty fields). Used by the legacy INI
// setup format and the web bridge's reorder command.
std::vector<std::wstring> SplitPipes(const std::wstring& s);

// Pure (de)serialization, testable without any file.
std::string SerializeConfig(const AppConfig& cfg); // UTF-8 JSON text
bool ParseConfig(const std::string& jsonUtf8, AppConfig& cfg); // false = not valid JSON
// Legacy reader: the pre-1.1 INI layout, straight from a file path.
bool ParseLegacyIni(const std::wstring& iniPath, AppConfig& cfg);
// Drops devices unseen for kDevicePurgeDays (relative to `now`, unix
// seconds) that no setup references. Returns how many were removed.
int PurgeStaleDevices(AppConfig& cfg, long long now);

class ConfigStore {
public:
    // Resolves the paths; creates the %APPDATA%\AudioLinker folder.
    void init();
    const std::wstring& path() const { return jsonPath_; }
    const std::wstring& legacyIniPath() const { return iniPath_; }

    // Loads config.json into cfg. If it does not exist but the legacy INI
    // does, migrates it (writes config.json, renames the INI to .bak).
    // Returns false when neither existed (cfg keeps its defaults).
    bool load(AppConfig& cfg);
    // Purges stale devices, then writes atomically (temp file + rename).
    bool save(AppConfig& cfg);
    // Writes cfg as JSON to an arbitrary file (export) / reads one back
    // (import: .json, or a legacy .ini).
    bool exportTo(const std::wstring& file, const AppConfig& cfg) const;
    bool importFrom(const std::wstring& file, AppConfig& cfg) const;

private:
    bool writeFileAtomic(const std::wstring& file, const std::string& bytes) const;
    std::wstring jsonPath_;
    std::wstring iniPath_;
};
