cmake_minimum_required(VERSION 3.20)
# The one place the version lives: generated into gen/version.h for the C++
# side (Options window) and the resource script (VERSIONINFO).
project(AudioLinker VERSION 1.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: the exe must run on a clean Windows without the VC++
# Redistributable (it depends only on system DLLs). Matches the vendored
# WebView2LoaderStatic.lib, which is itself built against the static CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})
configure_file(cmake/version.h.in ${GEN_DIR}/version.h @ONLY)

# Both WebView2 pages share common.css/common.js, inlined at build time so
# the embedded pages stay self-contained (NavigateToString can't reference
# files). fonts.css / icons.js are generated assets (see
# tools/gen_assets.py) inlined into those two. Editing any input regenerates
# the page and rebuilds the exe.
set(ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets)
foreach(page ui options)
    add_custom_command(
        OUTPUT ${GEN_DIR}/${page}.gen.html
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${ASSETS}/${page}.html -DDST=${GEN_DIR}/${page}.gen.html
            -DCSS=${ASSETS}/common.css -DJS=${ASSETS}/common.js
            -DFONTS=${ASSETS}/fonts.css -DICONS=${ASSETS}/icons.js
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/InlineAssets.cmake
        DEPENDS ${ASSETS}/${page}.html ${ASSETS}/common.css ${ASSETS}/common.js
                ${ASSETS}/fonts.css ${ASSETS}/icons.js
                ${CMAKE_CURRENT_SOURCE_DIR}/cmake/InlineAssets.cmake
        COMMENT "Inlining common assets into ${page}.html"
        VERBATIM)
endforeach()

add_executable(AudioLinker WIN32
    src/main.cpp
    # GUI side, one file per concern (shared declarations in src/App.h)
    src/Gui.cpp
    src/AppState.cpp
    src/Devices.cpp
    src/Group.cpp
    src/Hotkeys.cpp
    src/SystemAudio.cpp
    src/WebBridge.cpp
    src/Switcher.cpp
    src/Optimizer.cpp
    src/Panels.cpp
    src/Journal.cpp
    src/Tray.cpp
    src/DeviceManager.cpp
    src/AudioEngine.cpp
    src/Calibrator.cpp
    src/WasapiStream.cpp
    src/Config.cpp
    src/Log.cpp
    assets/AudioLinker.rc
    # CMake merges .manifest sources into the exe's embedded manifest via
    # mt.exe -- carries the PerMonitorV2 DPI-awareness declaration.
    assets/AudioLinker.manifest
    # Generated pages (custom commands above); listed so the target builds
    # them, the .rc embeds them as RCDATA.
    ${GEN_DIR}/ui.gen.html
    ${GEN_DIR}/options.gen.html
)

# The RC compiler embeds the generated pages as RCDATA but ninja can't
# discover that dependency itself -- without this, regenerating a page
# doesn't rebuild the exe.
set_property(SOURCE assets/AudioLinker.rc PROPERTY
    OBJECT_DEPENDS ${GEN_DIR}/ui.gen.html ${GEN_DIR}/options.gen.html ${GEN_DIR}/version.h)

# WebView2 SDK (vendored NuGet package, see third_party/webview2).
set(WEBVIEW2_NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/build/native)

target_include_directories(AudioLinker PRIVATE
    include
    ${GEN_DIR}
    ${WEBVIEW2_NATIVE}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)

target_compile_definitions(AudioLinker PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# /guard:cf: control-flow guard; /permissive-: standards conformance; /W4 keeps
# the (currently warning-free) tree that way.
target_compile_options(AudioLinker PRIVATE /utf-8 /W4 /permissive- /guard:cf)
target_link_options(AudioLinker PRIVATE /guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT)

target_link_libraries(AudioLinker PRIVATE
    ole32
    user32
    gdi32
    avrt
    shell32
    dwmapi
    advapi32
    comdlg32 # GetSaveFileNameW/GetOpenFileNameW (Options: export/import config)
    shcore # GetDpiForMonitor (initial window size on the cursor's monitor)
    mmdevapi # ActivateAudioInterfaceAsync (process-loopback capture)
    runtimeobject # RoGetActivationFactory/HSTRING (per-app endpoint routing)
    ${WEBVIEW2_NATIVE}/x64/WebView2LoaderStatic.lib
)

# Unit tests (pure DSP/containers, see tests/). ON by default: they add a few
# tiny executables and run in well under a second via ctest.
option(AUDIOLINKER_BUILD_TESTS "Build the unit tests" ON)
if(AUDIOLINKER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
