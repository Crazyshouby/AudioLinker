// The link between AudioLinker and the other KDX applications (the shell's taskbar, for now).
//
// A named pipe, \\.\pipe\kdx.audiolinker, readable and writable by the current user only (remote clients are
// refused). One client at a time; lines of JSON in both directions, UTF-8, '\n' terminated:
//
// Messages are envelopes { "type", "from", "data" } (kdx-ui/docs/SUITE.md):
//   client -> AudioLinker   audio.get                                    ask for the state
//                           audio.toggle                                 start / stop the group
//                           audio.volume  data {v: 0..100, commit: bool}
//                           audio.mute    data {on: bool}                whole-group mute
//                           audio.setup   data {name}                    play a saved setup
//   AudioLinker -> client   audio.state   data {version, running, starting, volume, muted, playing, setups: [{name, count}]}
//                           sent when the client connects and whenever one of these values changes.
//
// Only the five types above exist: nothing is evaluated, a line that is not one of them is ignored. The pipe thread
// only reads, validates and posts; every command runs on the UI thread through HandleWebMessage, exactly as if
// the page had sent it.
#include "App.h"
#include "kdx/kdx_bus.h"
#include <sddl.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace al {
namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\kdx.audiolinker";
constexpr size_t kMaxLine = 8192;

enum LinkMsg : WPARAM { kConnected = 0, kCommand = 1, kGet = 2 };

HANDLE g_stop = nullptr;  // manual-reset: the application is closing
HANDLE g_wake = nullptr;  // auto-reset: a state line is waiting in g_out
std::thread g_thread;
std::mutex g_mx;
std::string g_out;                 // the newest state line, not yet written (older ones are dropped)
std::atomic<bool> g_connected{ false };
std::string g_lastSent;            // UI thread only

// An SDDL string granting the current user (and nobody else) full access to the pipe.
std::wstring OwnerOnlySddl() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return L"";
    DWORD n = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
    std::vector<BYTE> buf(n);
    std::wstring out;
    if (GetTokenInformation(tok, TokenUser, buf.data(), n, &n)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
            out = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
            LocalFree(sid);
        }
    }
    CloseHandle(tok);
    return out;
}

// One validated command, as the page would have sent it ("cmd|argument"), or "" for a line to ignore.
// "get" is the pseudo-command that only asks for the state.
std::wstring Translate(const std::string& line, bool& commit) {
    commit = false;
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("type") || !j["type"].is_string()) return L"";
    const std::string t = j["type"].get<std::string>();
    static const json kEmpty = json::object();
    const json& d = (j.contains("data") && j["data"].is_object()) ? j["data"] : kEmpty;
    if (t == "audio.get") return L"get";
    if (t == "audio.toggle") return L"toggle";
    if (t == "audio.mute" && d.contains("on") && d["on"].is_boolean()) return d["on"].get<bool>() ? L"mutegroup|1" : L"mutegroup|0";
    if (t == "audio.volume" && d.contains("v") && d["v"].is_number_integer()) {
        long long v = d["v"].get<long long>();
        if (v < 0 || v > 100) return L"";
        commit = d.contains("commit") && d["commit"].is_boolean() && d["commit"].get<bool>();
        return L"mastervolume|" + std::to_wstring(v);
    }
    if (t == "audio.setup" && d.contains("name") && d["name"].is_string()) {
        const std::string n = d["name"].get<std::string>();
        if (n.empty() || n.size() > 200) return L"";
        return L"setup_play|" + FromUtf8(n);
    }
    return L"";
}

void PostCommand(const std::wstring& cmd, bool commit) {
    if (!g_mainWnd) return;
    if (cmd == L"get") { PostMessageW(g_mainWnd, WM_KDX_LINK, kGet, 0); return; }
    auto* m = new std::wstring(cmd);
    // A volume released ("commit") is saved and shown in AudioLinker's own windows: a marker the UI thread strips.
    if (commit) m->append(L"\x01");
    if (!PostMessageW(g_mainWnd, WM_KDX_LINK, kCommand, reinterpret_cast<LPARAM>(m))) delete m;
}

// Waits for `ov` to finish or for the application to close. True = finished with *bytes transferred.
bool Finish(HANDLE pipe, OVERLAPPED& ov, DWORD& bytes, DWORD timeoutMs = INFINITE) {
    HANDLE h[2] = { g_stop, ov.hEvent };
    DWORD w = WaitForMultipleObjects(2, h, FALSE, timeoutMs);
    if (w != WAIT_OBJECT_0 + 1) { CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &bytes, TRUE); return false; }
    return GetOverlappedResult(pipe, &ov, &bytes, FALSE) != FALSE;
}

void Serve(HANDLE pipe) {
    OVERLAPPED rd{}, wr{};
    rd.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wr.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::string acc;
    char buf[2048];
    bool reading = false;
    bool alive = true;
    while (alive) {
        if (!reading) {
            ResetEvent(rd.hEvent);
            DWORD got = 0;
            if (!ReadFile(pipe, buf, sizeof(buf), &got, &rd) && GetLastError() != ERROR_IO_PENDING) break;
            reading = true;
        }
        HANDLE h[3] = { g_stop, rd.hEvent, g_wake };
        DWORD w = WaitForMultipleObjects(3, h, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_OBJECT_0 + 1) {
            DWORD got = 0;
            reading = false;
            if (!GetOverlappedResult(pipe, &rd, &got, FALSE) || got == 0) break;
            acc.append(buf, got);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                bool commit = false;
                std::wstring cmd = Translate(line, commit);
                if (!cmd.empty()) PostCommand(cmd, commit);
            }
            if (acc.size() > kMaxLine) break; // a line that never ends: not one of ours
        } else if (w == WAIT_OBJECT_0 + 2) {
            std::string line;
            { std::lock_guard<std::mutex> lk(g_mx); line.swap(g_out); }
            if (line.empty()) continue;
            ResetEvent(wr.hEvent);
            DWORD put = 0;
            if (!WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &put, &wr) && GetLastError() != ERROR_IO_PENDING) { alive = false; continue; }
            if (!Finish(pipe, wr, put, 2000)) alive = false;
        }
    }
    if (reading) { CancelIoEx(pipe, &rd); DWORD x; GetOverlappedResult(pipe, &rd, &x, TRUE); }
    CloseHandle(rd.hEvent);
    CloseHandle(wr.hEvent);
}

void Run() {
    std::wstring sddl = OwnerOnlySddl();
    if (sddl.empty()) { LOG_WARN(L"KDX link: no security descriptor, link disabled"); return; }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) return;
    // FIRST_PIPE_INSTANCE: if another process already owns the name, say so instead of serving beside it. The
    // instance is kept for the whole run (DisconnectNamedPipe between clients), so the name is never up for grabs.
    HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 16384, 16384, 0, &sa);
    LocalFree(sa.lpSecurityDescriptor);
    if (pipe == INVALID_HANDLE_VALUE) { LOG_WARN(L"KDX link: pipe unavailable (already in use?), link disabled"); return; }
    OVERLAPPED cn{};
    cn.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (;;) {
        ResetEvent(cn.hEvent);
        BOOL ok = ConnectNamedPipe(pipe, &cn);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
            HANDLE h[2] = { g_stop, cn.hEvent };
            if (WaitForMultipleObjects(2, h, FALSE, INFINITE) == WAIT_OBJECT_0) break;
        } else if (!ok && err != ERROR_PIPE_CONNECTED) {
            if (WaitForSingleObject(g_stop, 500) == WAIT_OBJECT_0) break;
            DisconnectNamedPipe(pipe);
            continue;
        }
        g_connected = true;
        { std::lock_guard<std::mutex> lk(g_mx); g_out.clear(); }
        if (g_mainWnd) PostMessageW(g_mainWnd, WM_KDX_LINK, kConnected, 0); // the UI thread answers with the state
        Serve(pipe);
        g_connected = false;
        DisconnectNamedPipe(pipe);
        if (WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) break;
    }
    CloseHandle(cn.hEvent);
    CloseHandle(pipe);
}

} // namespace

void KdxLinkStart() {
    if (g_thread.joinable()) return;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = std::thread(Run);
}

void KdxLinkStop() {
    if (!g_thread.joinable()) return;
    SetEvent(g_stop);
    g_thread.join();
    CloseHandle(g_stop);
    CloseHandle(g_wake);
    g_stop = g_wake = nullptr;
}

// ---- Notifications for the shell's quick centre ----
void KdxNotifyShell(const char* level, const std::wstring& text) {
    // The same line twice within 10 s is one notification (an error repeated by a retry loop must not flood the centre).
    static std::mutex mx;
    static std::wstring lastText;
    static ULONGLONG lastAt = 0;
    {
        std::lock_guard<std::mutex> lk(mx);
        ULONGLONG now = GetTickCount64();
        if (text == lastText && now - lastAt < 10000) return;
        lastText = text;
        lastAt = now;
    }
    json d;
    d["title"] = "AudioLinker";
    d["text"] = ToUtf8(text.size() > 400 ? text.substr(0, 400) : text);
    d["level"] = level;
    std::string data = d.dump();
    std::thread([data] { kdx::bus::Send("shell", "notify", data, "audiolinker"); }).detach();
}

// ---- The accent of the suite (%APPDATA%\KDX\suite.json) ----
// Read at startup, then polled every kSuiteIntervalMs on the UI thread; a change is pushed to every page as { type: "theme", value }.
// New pages learn it from the state message (suiteAccent).
static std::unique_ptr<kdx::suite::Watcher> g_suiteWatcher;
static std::string g_suiteAccent = "rouge";

void KdxSuiteStart(HWND hwnd) {
    g_suiteAccent = kdx::suite::ReadAccent("rouge");
    g_suiteWatcher = std::make_unique<kdx::suite::Watcher>(g_suiteAccent);
    SetTimer(hwnd, IDT_SUITE, kSuiteIntervalMs, nullptr);
}

void KdxSuiteTick() {
    std::string a;
    if (g_suiteWatcher && g_suiteWatcher->Changed(a)) {
        g_suiteAccent = a;
        LOG_INFO(L"KDX suite accent: " + FromUtf8(a));
        json j;
        j["type"] = "theme";
        j["value"] = a;
        PostJson(j);
    }
}

const std::string& KdxSuiteAccent() { return g_suiteAccent; }

// UI thread. `force`: the client has just connected, send even if nothing changed.
void KdxLinkPublish(const json& st, bool force) {
    if (!g_connected) { g_lastSent.clear(); return; }
    json d;
    d["version"] = st.value("version", "");
    d["running"] = st.value("running", false);
    d["starting"] = st.value("starting", false);
    d["volume"] = st.value("masterVolume", 100);
    d["muted"] = st.value("groupMuted", false);
    d["playing"] = st.value("playingSetup", "");
    d["setups"] = st.contains("setups") ? st["setups"] : json::array();
    json out;
    out["type"] = "audio.state";
    out["from"] = "audiolinker";
    out["data"] = d;
    std::string line = out.dump() + "\n";
    if (!force && line == g_lastSent) return;
    g_lastSent = line;
    { std::lock_guard<std::mutex> lk(g_mx); g_out = line; }
    SetEvent(g_wake);
}

// UI thread: a message of the link.
void KdxLinkHandle(HWND hwnd, WPARAM what, LPARAM param) {
    if (what == kCommand) {
        std::unique_ptr<std::wstring> m(reinterpret_cast<std::wstring*>(param));
        bool commit = !m->empty() && m->back() == L'\x01';
        if (commit) m->pop_back();
        if (m->rfind(L"setup_play|", 0) == 0) { // only a setup that exists
            const std::wstring name = m->substr(11);
            bool found = false;
            for (const auto& s : g_setups) if (s.name == name) { found = true; break; }
            if (!found) return;
        }
        HandleWebMessage(hwnd, *m);
        if (commit) { HandleWebMessage(hwnd, L"mastervolumecommit"); SendState(); }
        return;
    }
    KdxLinkPublish(BuildState(), true);
}

} // namespace al
