#include "Log.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mutex>
#include <cstdio>

namespace {

std::mutex g_logMutex;
std::wstring g_logPath;
bool g_ready = false;

// Rotate past ~512KB: a session is normally a few KB, this only trips if
// something is looping and spamming, in which case one previous file is
// plenty of history.
constexpr long long kMaxBytes = 512 * 1024;

std::wstring LogDir() {
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) > 0) {
        return std::wstring(buf) + L"\\AudioLinker";
    }
    return L".";
}

void RotateIfNeeded() {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fad)) return;
    LARGE_INTEGER size;
    size.HighPart = fad.nFileSizeHigh;
    size.LowPart = fad.nFileSizeLow;
    if (size.QuadPart < kMaxBytes) return;
    std::wstring prev = g_logPath + L".1";
    DeleteFileW(prev.c_str());
    MoveFileW(g_logPath.c_str(), prev.c_str());
}

std::wstring Timestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

} // namespace

void LogInit() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::wstring dir = LogDir();
    CreateDirectoryW(dir.c_str(), nullptr); // no-op if it exists
    g_logPath = dir + L"\\audiolinker.log";
    g_ready = true;
    RotateIfNeeded();

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::wstring banner = L"\r\n===== " + Timestamp() + L" AudioLinker démarré (" + exe + L") =====\r\n";
    std::string utf8;
    int len = WideCharToMultiByte(CP_UTF8, 0, banner.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len > 1) {
        utf8.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, banner.c_str(), -1, utf8.data(), len, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(h);
}

namespace log_detail {

void Write(const wchar_t* level, const std::wstring& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_ready || g_logPath.empty()) return;
    std::wstring line = Timestamp() + L" [" + level + L"] " + msg + L"\r\n";
    std::string utf8;
    int len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return;
    utf8.resize(len - 1);
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
}

// Names the HRESULTs the audio paths actually hit, so the log reads without
// a lookup table handy; unknown codes still print in hex.
std::wstring Hr(long hr) {
    const wchar_t* name = nullptr;
    switch (static_cast<unsigned long>(hr)) {
        case 0x88890004: name = L"AUDCLNT_E_DEVICE_INVALIDATED"; break;
        case 0x88890001: name = L"AUDCLNT_E_NOT_INITIALIZED"; break;
        case 0x88890003: name = L"AUDCLNT_E_WRONG_ENDPOINT_TYPE"; break;
        case 0x88890008: name = L"AUDCLNT_E_UNSUPPORTED_FORMAT"; break;
        case 0x8889000A: name = L"AUDCLNT_E_DEVICE_IN_USE"; break;
        case 0x88890010: name = L"AUDCLNT_E_ENDPOINT_CREATE_FAILED"; break;
        case 0x88890019: name = L"AUDCLNT_E_EXCLUSIVE_MODE_ONLY"; break;
        case 0x8889001A: name = L"AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL"; break;
        case 0x80070005: name = L"E_ACCESSDENIED"; break;
        case 0x8007001F: name = L"ERROR_GEN_FAILURE"; break;
        case 0x80004005: name = L"E_FAIL"; break;
        case 0x80070490: name = L"ERROR_NOT_FOUND"; break;
        default: break;
    }
    wchar_t buf[24];
    swprintf(buf, 24, L"0x%08lX", static_cast<unsigned long>(hr));
    std::wstring out = buf;
    if (name) out += std::wstring(L" (") + name + L")";
    return out;
}

} // namespace log_detail
