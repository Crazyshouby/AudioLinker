#include "Gui.h"
#include "DeviceManager.h"
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <shellapi.h>

namespace {

void PrintDeviceList() {
    DeviceManager dm;
    wprintf(L"--- Capture (entrees) ---\n");
    for (auto& d : dm.enumerateCaptureDevices()) {
        wprintf(L"%s%s\n", d.name.c_str(), d.isDefault ? L"  [DEFAUT]" : L"");
    }
    wprintf(L"--- Rendu (sorties, utilisables aussi en source loopback) ---\n");
    for (auto& d : dm.enumerateRenderDevices()) {
        wprintf(L"%s%s\n", d.name.c_str(), d.isDefault ? L"  [DEFAUT]" : L"");
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool listOnly = false;
    bool startHidden = false;
    bool waitForPrevious = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--list-devices") == 0) listOnly = true;
        else if (wcscmp(argv[i], L"--tray") == 0) startHidden = true;
        // Relaunch after a config import: the previous instance is still
        // shutting down and holds the single-instance mutex -- wait for it
        // instead of bouncing off it (see RunApplication).
        else if (wcscmp(argv[i], L"--restart") == 0) waitForPrevious = true;
    }
    if (argv) LocalFree(argv);

    if (listOnly) {
        _setmode(_fileno(stdout), _O_U16TEXT);
        PrintDeviceList();
        fflush(stdout);
        return 0;
    }

    return RunApplication(hInstance, nCmdShow, startHidden, waitForPrevious);
}
