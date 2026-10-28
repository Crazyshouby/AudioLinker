// Crash reports: an unhandled exception writes a minidump next to the log
// (%LOCALAPPDATA%\AudioLinker\crash\) so a "it just disappeared" report comes
// with something to analyse. Nothing is sent anywhere. The audio default that
// a running group had displaced is repaired by the next start (see
// RepairAfterCrash), so a crash never needs a dialog here.

#include "CrashHandler.h"
#include "Log.h"

#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

constexpr size_t kKeepDumps = 5;

std::wstring CrashDir() {
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) == 0) return L"";
    return std::wstring(buf) + L"\\AudioLinker\\crash";
}

// Oldest dumps beyond kKeepDumps are removed at startup.
void PruneOldDumps(const std::wstring& dir) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        files.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end()); // names embed the date: oldest first
    while (files.size() > kKeepDumps) {
        DeleteFileW((dir + L"\\" + files.front()).c_str());
        files.erase(files.begin());
    }
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    static volatile LONG entered = 0;
    if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH; // crashed while crashing

    std::wstring dir = CrashDir();
    if (!dir.empty()) {
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t name[64];
        swprintf(name, 64, L"\\AudioLinker-%04d%02d%02d-%02d%02d%02d.dmp", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
        std::wstring path = dir + name;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei = {};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = info;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                              static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                              &mei, nullptr, nullptr);
            CloseHandle(file);
            wchar_t code[16];
            swprintf(code, 16, L"0x%08lX", info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0UL);
            LOG_ERROR(std::wstring(L"Plantage (exception ") + code + L") : rapport écrit dans " + path);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER; // the process ends; the next start repairs the audio default
}

} // namespace

void InstallCrashHandler() {
    std::wstring dir = CrashDir();
    if (!dir.empty()) PruneOldDumps(dir);
    SetUnhandledExceptionFilter(OnUnhandledException);
}
