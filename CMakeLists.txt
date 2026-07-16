cmake_minimum_required(VERSION 3.20)
project(AudioLinker LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(AudioLinker WIN32
    src/main.cpp
    src/Gui.cpp
    src/DeviceManager.cpp
    src/AudioEngine.cpp
    src/Calibrator.cpp
    src/Log.cpp
    assets/AudioLinker.rc
    # CMake merges .manifest sources into the exe's embedded manifest via
    # mt.exe -- carries the PerMonitorV2 DPI-awareness declaration.
    assets/AudioLinker.manifest
)

# The RC compiler embeds ui.html/options.html as RCDATA but ninja can't
# discover that dependency itself -- without this, editing either file
# doesn't rebuild the exe.
set_property(SOURCE assets/AudioLinker.rc PROPERTY
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui.html
                   ${CMAKE_CURRENT_SOURCE_DIR}/assets/options.html)

# WebView2 SDK (vendored NuGet package, see third_party/webview2).
set(WEBVIEW2_NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/build/native)

target_include_directories(AudioLinker PRIVATE include ${WEBVIEW2_NATIVE}/include)

target_compile_definitions(AudioLinker PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(AudioLinker PRIVATE /utf-8)

target_link_libraries(AudioLinker PRIVATE
    ole32
    user32
    gdi32
    avrt
    shell32
    dwmapi
    version
    advapi32
    comdlg32 # GetSaveFileNameW/GetOpenFileNameW (Options: export/import config)
    mmdevapi # ActivateAudioInterfaceAsync (process-loopback capture)
    runtimeobject # RoGetActivationFactory/HSTRING (per-app endpoint routing)
    ${WEBVIEW2_NATIVE}/x64/WebView2LoaderStatic.lib
)
