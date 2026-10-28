#include "Config.h"
#include "Log.h"
#include "Utf8.h"

#include <windows.h>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

// ordered_json keeps keys in insertion order: a stable, diff-friendly file.
using json = nlohmann::ordered_json;

int ClampDb(int v) { return std::min(12, std::max(-12, v)); }
int ClampChannelMode(int v) { return std::min(2, std::max(0, v)); }
int ClampVolume(int v) { return std::min(100, std::max(0, v)); }
int ClampLatencyMs(int v) { return std::min(500, std::max(0, v)); }
int ClampCushionMs(int v) { return std::min(kMaxBaseCushionMs, std::max(kMinBaseCushionMs, v)); }
int ClampDeviceFillMs(int v) { return std::min(kMaxDeviceFillMs, std::max(kMinDeviceFillMs, v)); }
int ClampThemeOverride(int v) { return std::min(2, std::max(0, v)); }

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

namespace {

// Clamp every field read from any source: a hand-edited file or an old
// version must never push an out-of-range value into the engine.
void Sanitize(OutputSettings& s) {
    s.channelMode = ClampChannelMode(s.channelMode);
    s.volumePercent = ClampVolume(s.volumePercent);
    s.latencyMs = ClampLatencyMs(s.latencyMs);
    for (int& db : s.eqDb) db = ClampDb(db);
    if (s.alias.size() > 60) s.alias.resize(60);
}

// --- JSON ---

json SettingsToJson(const OutputSettings& s, bool deviceLevel) {
    json j;
    j["checked"] = s.checked;
    j["latency"] = s.latencyMs;
    j["channel"] = s.channelMode;
    j["volume"] = s.volumePercent;
    j["eq"] = s.eqDb;
    j["muted"] = s.muted;
    if (deviceLevel) {
        // Device-level state (not part of setups): identity and cosmetics.
        j["order"] = s.order;
        j["name"] = ToUtf8(s.name);
        j["alias"] = ToUtf8(s.alias);
        j["color"] = ToUtf8(s.colorHex);
        j["lastSeen"] = s.lastSeen;
    }
    return j;
}

template <typename T>
T Get(const json& j, const char* key, const T& fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (...) {
        return fallback;
    }
}

std::wstring GetW(const json& j, const char* key, const std::wstring& fallback = L"") {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return fallback;
    return FromUtf8(it->get<std::string>());
}

OutputSettings SettingsFromJson(const json& j) {
    OutputSettings s;
    s.checked = Get(j, "checked", false);
    s.latencyMs = Get(j, "latency", 0);
    s.channelMode = Get(j, "channel", 0);
    s.volumePercent = Get(j, "volume", 100);
    auto eq = j.find("eq");
    if (eq != j.end() && eq->is_array()) {
        for (size_t b = 0; b < static_cast<size_t>(kEqBandCount) && b < eq->size(); ++b) {
            if ((*eq)[b].is_number()) s.eqDb[b] = (*eq)[b].get<int>();
        }
    }
    s.muted = Get(j, "muted", false);
    s.order = Get(j, "order", 1000000);
    s.name = GetW(j, "name");
    s.alias = GetW(j, "alias");
    s.colorHex = GetW(j, "color");
    s.lastSeen = Get<long long>(j, "lastSeen", 0);
    Sanitize(s);
    return s;
}

} // namespace

std::string SerializeConfig(const AppConfig& cfg) {
    json j;
    j["format"] = 1;
    json& g = j["group"];
    g["autostart"] = cfg.autostart;
    g["masterVolume"] = cfg.masterVolume;
    g["micId"] = ToUtf8(cfg.micId);
    g["cushionMs"] = cfg.cushionMs;
    g["deviceFillMs"] = cfg.deviceFillMs;
    g["lowLatency"] = cfg.lowLatency;
    g["theme"] = cfg.theme;
    g["captureMode"] = cfg.captureMode;
    g["previousDefault"] = ToUtf8(cfg.previousDefault);

    json& h = j["hotkeys"];
    h["toggleMods"] = cfg.toggleMods;
    h["toggleVk"] = cfg.toggleVk;
    h["switcherMods"] = cfg.switcherMods;
    h["switcherVk"] = cfg.switcherVk;
    h["setupDigits"] = cfg.setupDigitHotkeys;

    json& st = j["setups"];
    st["active"] = ToUtf8(cfg.activeSetup);
    st["editing"] = ToUtf8(cfg.editingSetup);
    st["list"] = json::array();
    for (const auto& s : cfg.setups) {
        json js;
        js["name"] = ToUtf8(s.name);
        js["micId"] = ToUtf8(s.micId);
        js["micVolume"] = s.micVolume;
        js["micMute"] = s.micMute;
        js["devices"] = json::array();
        for (const auto& [id, os] : s.devices) {
            json jd = SettingsToJson(os, false);
            jd["id"] = ToUtf8(id);
            js["devices"].push_back(std::move(jd));
        }
        st["list"].push_back(std::move(js));
    }

    json& d = j["devices"];
    d = json::object();
    for (const auto& [id, os] : cfg.devices) d[ToUtf8(id)] = SettingsToJson(os, true);

    j["routedApps"] = json::array();
    for (const auto& exe : cfg.routedApps) j["routedApps"].push_back(ToUtf8(exe));

    json& w = j["window"];
    w["x"] = cfg.window.x;
    w["y"] = cfg.window.y;
    w["w"] = cfg.window.w;
    w["h"] = cfg.window.h;

    json& pj = j["panels"];
    pj = json::object();
    for (const auto& [key, pp] : cfg.panels) {
        pj[key] = { { "x", pp.x }, { "y", pp.y }, { "w", pp.w }, { "h", pp.h },
                    { "open", pp.open }, { "shaded", pp.shaded } };
    }

    j["automation"] = json::array();
    for (const auto& r : cfg.automation) {
        json jr;
        jr["deviceId"] = ToUtf8(r.deviceId);
        jr["onConnect"] = ToUtf8(r.onConnect);
        jr["onDisconnect"] = ToUtf8(r.onDisconnect);
        j["automation"].push_back(std::move(jr));
    }
    return j.dump(2);
}

bool ParseConfig(const std::string& text, AppConfig& cfg) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    AppConfig c;

    if (auto g = j.find("group"); g != j.end() && g->is_object()) {
        c.autostart = Get(*g, "autostart", false);
        c.masterVolume = ClampVolume(Get(*g, "masterVolume", 100));
        c.micId = GetW(*g, "micId");
        c.cushionMs = ClampCushionMs(Get(*g, "cushionMs", kDefaultBaseCushionMs));
        c.deviceFillMs = ClampDeviceFillMs(Get(*g, "deviceFillMs", kDefaultDeviceFillMs));
        c.lowLatency = Get(*g, "lowLatency", false);
        c.theme = ClampThemeOverride(Get(*g, "theme", 0));
        c.captureMode = Get(*g, "captureMode", 0) == 1 ? 1 : 0;
        c.previousDefault = GetW(*g, "previousDefault");
    }
    if (auto h = j.find("hotkeys"); h != j.end() && h->is_object()) {
        c.toggleMods = Get<unsigned>(*h, "toggleMods", c.toggleMods);
        c.toggleVk = Get<unsigned>(*h, "toggleVk", c.toggleVk);
        c.switcherMods = Get<unsigned>(*h, "switcherMods", c.switcherMods);
        c.switcherVk = Get<unsigned>(*h, "switcherVk", c.switcherVk);
        c.setupDigitHotkeys = Get(*h, "setupDigits", true);
    }
    if (auto st = j.find("setups"); st != j.end() && st->is_object()) {
        c.activeSetup = GetW(*st, "active");
        c.editingSetup = GetW(*st, "editing");
        if (auto list = st->find("list"); list != st->end() && list->is_array()) {
            for (const auto& js : *list) {
                if (!js.is_object()) continue;
                Setup s;
                s.name = GetW(js, "name");
                if (s.name.empty()) continue;
                s.micId = GetW(js, "micId");
                s.micVolume = Get(js, "micVolume", -1);
                if (s.micVolume >= 0) s.micVolume = ClampVolume(s.micVolume);
                s.micMute = Get(js, "micMute", false);
                if (auto devs = js.find("devices"); devs != js.end() && devs->is_array()) {
                    for (const auto& jd : *devs) {
                        if (!jd.is_object()) continue;
                        std::wstring id = GetW(jd, "id");
                        if (id.empty()) continue;
                        s.devices.emplace_back(id, SettingsFromJson(jd));
                    }
                }
                if (!s.devices.empty()) c.setups.push_back(std::move(s));
            }
        }
    }
    if (auto d = j.find("devices"); d != j.end() && d->is_object()) {
        for (auto it = d->begin(); it != d->end(); ++it) {
            if (!it.value().is_object()) continue;
            c.devices[FromUtf8(it.key())] = SettingsFromJson(it.value());
        }
    }
    if (auto r = j.find("routedApps"); r != j.end() && r->is_array()) {
        for (const auto& e : *r) {
            if (e.is_string()) c.routedApps.push_back(FromUtf8(e.get<std::string>()));
        }
    }
    if (auto w = j.find("window"); w != j.end() && w->is_object()) {
        c.window.x = Get(*w, "x", 0);
        c.window.y = Get(*w, "y", 0);
        c.window.w = Get(*w, "w", 0);
        c.window.h = Get(*w, "h", 0);
    }
    if (auto pj = j.find("panels"); pj != j.end() && pj->is_object()) {
        for (const auto& [key, v] : pj->items()) {
            if (!v.is_object()) continue;
            PanelPlacement pp;
            pp.x = Get(v, "x", 0);
            pp.y = Get(v, "y", 0);
            pp.w = Get(v, "w", 0);
            pp.h = Get(v, "h", 0);
            pp.open = Get(v, "open", false);
            pp.shaded = Get(v, "shaded", false);
            c.panels[key] = pp;
        }
    }
    if (auto a = j.find("automation"); a != j.end() && a->is_array()) {
        for (const auto& jr : *a) {
            if (!jr.is_object()) continue;
            AutomationRule r;
            r.deviceId = GetW(jr, "deviceId");
            if (r.deviceId.empty()) continue;
            r.onConnect = GetW(jr, "onConnect");
            r.onDisconnect = GetW(jr, "onDisconnect");
            c.automation.push_back(std::move(r));
        }
    }

    // A dangling name (setup deleted in a previous run) must not stick around.
    auto exists = [&](const std::wstring& name) {
        return std::any_of(c.setups.begin(), c.setups.end(), [&](const Setup& s) { return s.name == name; });
    };
    if (!exists(c.activeSetup)) c.activeSetup.clear();
    if (!exists(c.editingSetup)) c.editingSetup.clear();

    cfg = std::move(c);
    return true;
}

// --- Legacy INI (pre-1.1) ---

namespace {

std::wstring IniString(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(section, key, L"", buf, 1024, ini.c_str());
    return buf;
}

int IniInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
}

OutputSettings IniDeviceSettings(const std::wstring& ini, const std::wstring& section) {
    OutputSettings s;
    const wchar_t* sec = section.c_str();
    s.checked = IniInt(ini, sec, L"checked", 0) != 0;
    s.latencyMs = IniInt(ini, sec, L"latency", 0);
    s.channelMode = IniInt(ini, sec, L"channel", 0);
    s.volumePercent = IniInt(ini, sec, L"volume", 100);
    // Bands default from the oldest bass/treble keys (band 0 = low shelf,
    // the last = high shelf) so even 2-band-era configs migrate.
    int legacyBass = IniInt(ini, sec, L"bass", 0);
    int legacyTreble = IniInt(ini, sec, L"treble", 0);
    for (int b = 0; b < kEqBandCount; ++b) {
        int fallback = (b == 0) ? legacyBass : (b == kEqBandCount - 1) ? legacyTreble : 0;
        s.eqDb[b] = IniInt(ini, sec, (L"eq" + std::to_wstring(b)).c_str(), fallback);
    }
    s.muted = IniInt(ini, sec, L"muted", 0) != 0;
    s.order = IniInt(ini, sec, L"order", 1000000);
    s.name = IniString(ini, sec, L"name");
    s.alias = IniString(ini, sec, L"alias");
    s.colorHex = IniString(ini, sec, L"color");
    Sanitize(s);
    return s;
}

} // namespace

bool ParseLegacyIni(const std::wstring& ini, AppConfig& cfg) {
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    AppConfig c;
    c.autostart = IniInt(ini, L"group", L"autostart", 0) != 0;
    c.masterVolume = ClampVolume(IniInt(ini, L"group", L"mastervolume", 100));
    c.micId = IniString(ini, L"group", L"micid");
    c.cushionMs = ClampCushionMs(IniInt(ini, L"group", L"cushionms", kDefaultBaseCushionMs));
    c.deviceFillMs = ClampDeviceFillMs(IniInt(ini, L"group", L"devicefillms", kDefaultDeviceFillMs));
    c.lowLatency = IniInt(ini, L"group", L"lowlatency", 0) != 0;
    c.theme = ClampThemeOverride(IniInt(ini, L"group", L"theme", 0));
    c.previousDefault = IniString(ini, L"group", L"previousdefault");

    c.toggleMods = static_cast<unsigned>(IniInt(ini, L"hotkeys", L"togglemods", static_cast<int>(c.toggleMods)));
    c.toggleVk = static_cast<unsigned>(IniInt(ini, L"hotkeys", L"togglevk", static_cast<int>(c.toggleVk)));
    c.switcherMods = static_cast<unsigned>(IniInt(ini, L"hotkeys", L"switchermods", static_cast<int>(c.switcherMods)));
    c.switcherVk = static_cast<unsigned>(IniInt(ini, L"hotkeys", L"switchervk", static_cast<int>(c.switcherVk)));
    c.setupDigitHotkeys = IniInt(ini, L"hotkeys", L"setupdigits", 1) != 0;

    c.activeSetup = IniString(ini, L"setups", L"active");
    c.editingSetup = IniString(ini, L"setups", L"editing");
    int count = IniInt(ini, L"setups", L"count", 0);
    for (int i = 0; i < count; ++i) {
        std::wstring section = L"setup_" + std::to_wstring(i);
        Setup setup;
        setup.name = IniString(ini, section.c_str(), L"name");
        if (setup.name.empty()) continue;
        setup.micId = IniString(ini, section.c_str(), L"mic");
        setup.micVolume = IniInt(ini, section.c_str(), L"micvol", -1);
        if (setup.micVolume >= 0) setup.micVolume = ClampVolume(setup.micVolume);
        setup.micMute = IniInt(ini, section.c_str(), L"micmute", 0) != 0;
        int devCount = IniInt(ini, section.c_str(), L"devcount", 0);
        for (int k = 0; k < devCount; ++k) {
            auto parts = SplitPipes(IniString(ini, section.c_str(), (L"dev" + std::to_wstring(k)).c_str()));
            // Fields: id|checked|channel|volume|latency|eq0|eqLast|muted|eq1..eqN-2
            // (7 fields = before the muted flag, 8 = before the 5-band EQ).
            if (parts.size() < 7 || parts[0].empty()) continue;
            OutputSettings s;
            s.checked = parts[1] == L"1";
            s.channelMode = _wtoi(parts[2].c_str());
            s.volumePercent = _wtoi(parts[3].c_str());
            s.latencyMs = _wtoi(parts[4].c_str());
            s.eqDb[0] = _wtoi(parts[5].c_str());
            s.eqDb[kEqBandCount - 1] = _wtoi(parts[6].c_str());
            s.muted = parts.size() >= 8 && parts[7] == L"1";
            for (int b = 1; b < kEqBandCount - 1; ++b) {
                size_t idx = 7 + static_cast<size_t>(b);
                s.eqDb[b] = (parts.size() > idx) ? _wtoi(parts[idx].c_str()) : 0;
            }
            Sanitize(s);
            setup.devices.emplace_back(parts[0], s);
        }
        if (!setup.devices.empty()) c.setups.push_back(std::move(setup));
    }
    auto exists = [&](const std::wstring& name) {
        return std::any_of(c.setups.begin(), c.setups.end(), [&](const Setup& s) { return s.name == name; });
    };
    if (!exists(c.activeSetup)) c.activeSetup.clear();
    if (!exists(c.editingSetup)) c.editingSetup.clear();

    // Every endpoint section ("{0.0.0.00000000}.{guid}") becomes a device.
    std::vector<wchar_t> names(65536);
    DWORD len = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), ini.c_str());
    const long long now = static_cast<long long>(std::time(nullptr));
    if (len > 0) {
        for (const wchar_t* p = names.data(); *p; p += wcslen(p) + 1) {
            std::wstring section = p;
            if (section.rfind(L"{0.0.0.", 0) != 0) continue;
            OutputSettings s = IniDeviceSettings(ini, section);
            s.lastSeen = now; // give every migrated device the full grace period
            c.devices[section] = s;
        }
    }

    std::wstring routed = IniString(ini, L"routing", L"apps");
    size_t start = 0;
    while (start <= routed.size() && !routed.empty()) {
        size_t sep = routed.find(L';', start);
        std::wstring exe = routed.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start);
        if (!exe.empty()) c.routedApps.push_back(exe);
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }

    c.window.x = IniInt(ini, L"window", L"x", 0);
    c.window.y = IniInt(ini, L"window", L"y", 0);
    c.window.w = IniInt(ini, L"window", L"w", 0);
    c.window.h = IniInt(ini, L"window", L"h", 0);

    cfg = std::move(c);
    return true;
}

int PurgeStaleDevices(AppConfig& cfg, long long now) {
    const long long limit = static_cast<long long>(kDevicePurgeDays) * 24 * 3600;
    int removed = 0;
    for (auto it = cfg.devices.begin(); it != cfg.devices.end();) {
        bool stale = it->second.lastSeen == 0 || now - it->second.lastSeen > limit;
        bool referenced = false;
        for (const auto& s : cfg.setups) {
            for (const auto& [id, os] : s.devices) {
                if (id == it->first) referenced = true;
            }
        }
        if (stale && !referenced) {
            it = cfg.devices.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

// --- Store ---

void ConfigStore::init() {
    wchar_t buf[MAX_PATH] = {};
    std::wstring appData = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) > 0 ? buf : L".";
    std::wstring dir = appData + L"\\AudioLinker";
    CreateDirectoryW(dir.c_str(), nullptr);
    jsonPath_ = dir + L"\\config.json";
    iniPath_ = appData + L"\\AudioLinker.ini";
}

namespace {

bool ReadWholeFile(const std::wstring& file, std::string& out) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (64 << 20)) { CloseHandle(h); return false; }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    bool ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                              read == out.size());
    CloseHandle(h);
    return ok;
}

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

} // namespace

bool ConfigStore::writeFileAtomic(const std::wstring& file, const std::string& bytes) const {
    std::wstring tmp = file + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size();
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool ConfigStore::load(AppConfig& cfg) {
    std::string text;
    if (ReadWholeFile(jsonPath_, text)) {
        if (ParseConfig(text, cfg)) return true;
        // Unreadable JSON: keep it aside for inspection rather than
        // overwrite it at the next save, and start from defaults.
        LOG_ERROR(L"config.json illisible — mis de côté en config.json.broken, réglages par défaut.");
        MoveFileExW(jsonPath_.c_str(), (jsonPath_ + L".broken").c_str(), MOVEFILE_REPLACE_EXISTING);
        return false;
    }
    if (ParseLegacyIni(iniPath_, cfg)) {
        LOG_INFO(L"Configuration INI migrée vers " + jsonPath_ + L" (" +
                 std::to_wstring(cfg.devices.size()) + L" périphériques, " +
                 std::to_wstring(cfg.setups.size()) + L" setups).");
        if (writeFileAtomic(jsonPath_, SerializeConfig(cfg))) {
            MoveFileExW(iniPath_.c_str(), (iniPath_ + L".bak").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        return true;
    }
    return false;
}

bool ConfigStore::save(AppConfig& cfg) {
    int purged = PurgeStaleDevices(cfg, static_cast<long long>(std::time(nullptr)));
    if (purged > 0) LOG_INFO(std::to_wstring(purged) + L" périphérique(s) absent(s) depuis plus de 90 jours retiré(s) de la configuration.");
    if (!writeFileAtomic(jsonPath_, SerializeConfig(cfg))) {
        LOG_ERROR(L"Écriture de la configuration impossible : " + jsonPath_);
        return false;
    }
    return true;
}

bool ConfigStore::exportTo(const std::wstring& file, const AppConfig& cfg) const {
    return writeFileAtomic(file, SerializeConfig(cfg));
}

bool ConfigStore::importFrom(const std::wstring& file, AppConfig& cfg) const {
    if (EndsWithNoCase(file, L".ini")) return ParseLegacyIni(file, cfg);
    std::string text;
    return ReadWholeFile(file, text) && ParseConfig(text, cfg);
}
