@echo off
rem Dev build: configure (if needed) and compile into build-dev\, kept apart
rem from build\ so a running instance's exe is never overwritten.
rem Usage: build.cmd [tests]   -> "tests" also runs ctest after the build.
setlocal
rem A stray quote in a user PATH entry (e.g. "C:\Program Files\nodejs"") breaks
rem vsdevcmd.bat's parsing; strip every quote from PATH for this script only.
set "PATH=%PATH:"=%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist "build-dev\build.ninja" cmake -B build-dev -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build build-dev
if errorlevel 1 exit /b 1
if /i "%~1"=="tests" ctest --test-dir build-dev --output-on-failure
exit /b %errorlevel%
