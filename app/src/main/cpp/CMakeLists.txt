cmake_minimum_required(VERSION 3.22.1)

project("sgre_wrapper")

add_library(sgre_wrapper SHARED
    jni_bridge.cpp
    input_injector.cpp
    steam_hook.cpp
    payload_loader.cpp
    d3d11_bridge.cpp
    dinput_bridge.cpp
    xaudio2_bridge.cpp
    win32_bridge.cpp
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv2)

target_link_libraries(sgre_wrapper
    ${log-lib}
    ${android-lib}
    ${egl-lib}
    ${gles-lib}
)
