#pragma once

// Writes a minidump to %LOCALAPPDATA%\AudioLinker\crash\ on an unhandled
// exception (the last few are kept). Call once, early in startup.
void InstallCrashHandler();
