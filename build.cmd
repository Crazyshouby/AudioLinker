@echo off
rem Dev build: configure (if needed) and compile into build-dev\, kept apart
rem from build\ so a running instance's exe is never overwritten.
rem Usage: build.cmd [tests]   -> "tests" also runs ctest after the build.
rem If build-dev\AudioLinker.exe is running (locked), build elsewhere:
rem   set AL_BUILD_DIR=build-next && build.cmd
setlocal
if "%AL_BUILD_DIR%"=="" set "AL_BUILD_DIR=build-dev"
rem A stray quote in a user PATH entry (e.g. "C:\Program Files\nodejs"") breaks
rem vsdevcmd.bat's parsing; strip every quote from PATH for this script only.
set "PATH=%PATH:"=%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist "%AL_BUILD_DIR%\build.ninja" cmake -B %AL_BUILD_DIR% -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build %AL_BUILD_DIR%
if errorlevel 1 exit /b 1
if /i "%~1"=="tests" ctest --test-dir %AL_BUILD_DIR% --output-on-failure
exit /b %errorlevel%
