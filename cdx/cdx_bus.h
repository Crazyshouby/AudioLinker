// cdx_bus.h -- le client C++ des réglages communs et du bus de la suite CDX (voir docs/SUITE.md). En-tête seul, sans bibliothèque JSON :
// l'appelant garde la sienne pour lire les messages reçus.
//
//   cdx::suite::ReadAccent()        l'accent de %APPDATA%\CDX\suite.json ("rouge" si le fichier manque ou est abîmé)
//   cdx::suite::Watcher             sondage de la date et de la taille du fichier : Changed() à appeler toutes les 250 ms
//   cdx::bus::Send(app, type, data) un message « tire et oublie » vers le tube \\.\pipe\cdx.<app>
//   cdx::bus::Compose(...)          la ligne JSON d'un message (pour une connexion longue, comme celle d'AudioLinker)
#pragma once

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace cdx {

constexpr int kBusMaxLine = 65536;

namespace suite {

inline bool IsAccent(const std::string& s) {
    static const char* const kAccents[] = { "rouge", "bleu", "vert", "violet", "ambre", "sarcelle", "charbon" };
    for (const char* a : kAccents) if (s == a) return true;
    return false;
}

// CDX_SUITE_DIR remplace le dossier (réservé aux essais : ne jamais toucher aux vrais réglages).
inline std::wstring Dir() {
    wchar_t env[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"CDX_SUITE_DIR", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return env;
    wchar_t* p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) out = std::wstring(p) + L"\\CDX";
    if (p) CoTaskMemFree(p);
    return out;
}
inline std::wstring Path() { std::wstring d = Dir(); return d.empty() ? d : d + L"\\suite.json"; }

// Lit la valeur de la chaîne `key` à la main (le fichier est minuscule et écrit par nous) ; "" si absente.
inline std::string StringField(const std::string& json, const std::string& key) {
    size_t k = json.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    size_t c = json.find(':', k);
    if (c == std::string::npos) return "";
    size_t q1 = json.find('"', c);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 - q1 > 40) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// Le contenu d'un suite.json : l'accent s'il est connu et si la version est 1 (ou absente), sinon `fallback`.
inline std::string ParseAccent(const std::string& json, const std::string& fallback = "rouge") {
    size_t v = json.find("\"version\"");
    if (v != std::string::npos) {
        size_t c = json.find(':', v);
        if (c == std::string::npos) return fallback;
        const char* s = json.c_str() + c + 1;
        char* end = nullptr;
        long n = strtol(s, &end, 10);
        if (end == s || n != 1) return fallback;
    }
    std::string a = StringField(json, "accent");
    return IsAccent(a) ? a : fallback;
}

inline std::string ReadAccent(const std::string& fallback = "rouge") {
    std::wstring p = Path();
    if (p.empty()) return fallback;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fallback;
    char buf[4096];
    DWORD n = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(h);
    if (!ok) return fallback;
    return ParseAccent(std::string(buf, n), fallback);
}

// À appeler toutes les 250 ms : rend true, avec le nouvel accent dans `accent`, quand le fichier a changé et que l'accent n'est plus le même.
class Watcher {
public:
    explicit Watcher(std::string current = "rouge") : current_(std::move(current)) { stamp_ = Stamp(); }
    const std::string& Current() const { return current_; }
    bool Changed(std::string& accent) {
        ULONGLONG s = Stamp();
        if (s == stamp_) return false;
        stamp_ = s;
        std::string a = ReadAccent(current_);
        if (a == current_) return false;
        current_ = a;
        accent = a;
        return true;
    }
private:
    static ULONGLONG Stamp() {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        std::wstring p = Path();
        if (p.empty() || !GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return 0;
        return (static_cast<ULONGLONG>(d.ftLastWriteTime.dwHighDateTime) << 32 | d.ftLastWriteTime.dwLowDateTime) ^ (static_cast<ULONGLONG>(d.nFileSizeLow) << 1) ^ 1;
    }
    std::string current_;
    ULONGLONG stamp_ = 0;
};

} // namespace suite

namespace bus {

inline bool IsApp(const std::string& s) {
    return s == "shell" || s == "explorer" || s == "browser" || s == "audiolinker" || s == "kirc";
}

inline std::string Escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o;
}

// {"type":"...","from":"...","data":{...}} ; `dataJson` est un objet JSON déjà écrit ("{}" par défaut). Sans saut de ligne final.
inline std::string Compose(const std::string& type, const std::string& from, const std::string& dataJson = "{}") {
    return "{\"type\":\"" + Escape(type) + "\",\"from\":\"" + Escape(from) + "\",\"data\":" + (dataJson.empty() ? "{}" : dataJson) + "}";
}

// Un message « tire et oublie » vers \\.\pipe\cdx.<app>. False : l'application n'écoute pas (ou nom inconnu, ou ligne trop longue).
inline bool Send(const std::string& app, const std::string& type, const std::string& dataJson = "{}", const std::string& from = "kirc") {
    if (!IsApp(app)) return false;
    std::string line = Compose(type, from, dataJson) + "\n";
    if (line.size() > static_cast<size_t>(kBusMaxLine)) return false;
    wchar_t pre[64];
    DWORD pn = GetEnvironmentVariableW(L"CDX_PIPE_PREFIX", pre, 64);   // réservé aux essais
    std::wstring name = L"\\\\.\\pipe\\" + std::wstring(pn > 0 && pn < 64 ? pre : L"cdx.") + std::wstring(app.begin(), app.end());
    if (!WaitNamedPipeW(name.c_str(), 250)) { if (GetLastError() != ERROR_SEM_TIMEOUT && GetLastError() != ERROR_FILE_NOT_FOUND) {} }
    HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &put, nullptr);
    CloseHandle(h);
    return ok && put == line.size();
}

} // namespace bus
} // namespace cdx
