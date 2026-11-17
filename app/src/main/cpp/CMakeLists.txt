cmake_minimum_required(VERSION 3.22.1)

project("sgre_wrapper")

add_library(sgre_wrapper SHARED
    jni_bridge.cpp
    input_injector.cpp
    steam_hook.cpp
    payload_loader.cpp
    d3d11_bridge.cpp
    dxbc_reflector.cpp
    bc7_decoder.cpp
    resource_registry.cpp
    dinput_bridge.cpp
    xaudio2_bridge.cpp
    win32_bridge.cpp
    media_foundation_bridge.cpp
    webm_decoder.cpp
    audio_sink.cpp
    wmadeci.c
    wma_decoder.cpp
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles3-lib GLESv3)
find_library(mediandk-lib mediandk)
find_library(aaudio-lib aaudio)

target_link_libraries(sgre_wrapper
    ${log-lib}
    ${android-lib}
    ${egl-lib}
    ${gles3-lib}
    ${mediandk-lib}
    ${aaudio-lib}
)
