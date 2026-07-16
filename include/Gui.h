#pragma once
#include <windows.h>

// startHidden: begin in the notification area without showing the window
// (used by the "launch with Windows" Run entry via the --tray flag).
// waitForPrevious: this launch replaces an instance that is still shutting
// down (config-import relaunch via --restart) -- wait for its single-instance
// mutex to be released instead of deferring to it and exiting.
int RunApplication(HINSTANCE hInstance, int nCmdShow, bool startHidden, bool waitForPrevious);
