// The Journal window's data: a ring of recent events (what the diagnostic log
// records -- reconnections, calibration, errors -- plus the messages shown as
// toasts), pushed live to the Journal window and replayed when it opens.

#include "App.h"

#include <deque>

namespace al {

namespace {

constexpr size_t kMaxLines = 300;
std::deque<JournalLine> g_lines;

long long NowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>((u.QuadPart - 116444736000000000ULL) / 10000ULL); // 1601 -> 1970, 100ns -> ms
}

json LineJson(const JournalLine& l) {
    return { { "ts", l.ts }, { "level", l.level }, { "text", ToUtf8(l.text) } };
}

// Log observer: runs on whatever thread logged, with the log mutex held, so
// it only hands the line to the UI thread.
void OnLogLine(const wchar_t* level, const std::wstring& msg) {
    if (!g_mainWnd) return;
    int lv = (level && level[0] == L'W') ? 1 : (level && level[0] == L'E') ? 2 : 0;
    auto* line = new JournalLine{ NowMs(), lv, msg };
    if (!PostMessageW(g_mainWnd, WM_JOURNAL, 0, reinterpret_cast<LPARAM>(line))) delete line;
}

} // namespace

void JournalInstall() { SetLogSink(OnLogLine); }

void JournalAdd(int level, const std::wstring& text) {
    JournalLine line{ NowMs(), level, text };
    // An event is often both logged and shown as a toast, with the same
    // words: keep it once.
    int checked = 0;
    for (auto it = g_lines.rbegin(); it != g_lines.rend() && checked < 8; ++it, ++checked) {
        if (it->text == text && line.ts - it->ts < 3000) return;
    }
    g_lines.push_back(line);
    while (g_lines.size() > kMaxLines) g_lines.pop_front();
    // The Journal window shows it; the launcher counts what it has not seen.
    json j = { { "type", "journalAdd" }, { "entry", LineJson(line) } };
    std::wstring msg = FromUtf8(j.dump());
    PostToPanel(PanelId::Journal, msg);
    PostToPanel(PanelId::Launcher, msg);
}

void JournalFromMessage(LPARAM lParam) {
    std::unique_ptr<JournalLine> line(reinterpret_cast<JournalLine*>(lParam));
    if (line) JournalAdd(line->level, line->text);
}

void JournalClear() {
    g_lines.clear();
    PostToPanels(L"{\"type\":\"journal\",\"entries\":[]}");
}

void SendJournal(PanelId to) {
    json j = { { "type", "journal" }, { "entries", json::array() } };
    for (const auto& l : g_lines) j["entries"].push_back(LineJson(l));
    PostToPanel(to, FromUtf8(j.dump()));
}

} // namespace al
