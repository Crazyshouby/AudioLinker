#pragma once
#include <windows.h>

// startHidden: begin in the notification area without showing the window
// (used by the "launch with Windows" Run entry via the --tray flag).
// waitForPrevious: this launch replaces an instance that is still shutting
// down (config-import relaunch via --restart) -- wait for its single-instance
// mutex to be released instead of deferring to it and exiting.
int RunApplication(HINSTANCE hInstance, int nCmdShow, bool startHidden, bool waitForPrevious);

// --quit: asks the running instance (if any) to shut down the way the tray's
// Quitter does -- group stopped, Windows' default output restored -- and waits
// for it to exit. The installer uses it before replacing or removing the exe.
// True when no instance is left (none was running, or it exited in time).
bool QuitRunningInstance(DWORD timeoutMs);
