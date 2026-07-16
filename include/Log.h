#pragma once

#include <string>

// Minimal thread-safe diagnostic log. Writes to
// %LOCALAPPDATA%\AudioLinker\audiolinker.log, rotated once past a size cap
// (the previous file kept as .1). Every audio path here is best-effort and
// fails with a bare HRESULT deep inside a worker thread; without this a
// user-reported "it won't start" has no cause attached. Cheap and always on
// -- a passive listening app writes a handful of lines per session, not a
// hot path (never call it from inside the per-callback render/capture loop).
namespace log_detail {
void Write(const wchar_t* level, const std::wstring& msg);
std::wstring Hr(long hr); // "0x88890004" + known-name when recognized
}

// Opens (creates) the log file and records a session banner. Safe to call
// more than once. Call once at startup.
void LogInit();

#define LOG_INFO(msg)  ::log_detail::Write(L"INFO", (msg))
#define LOG_WARN(msg)  ::log_detail::Write(L"WARN", (msg))
#define LOG_ERROR(msg) ::log_detail::Write(L"ERR ", (msg))
