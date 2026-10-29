cmake_minimum_required(VERSION 4.2)
project(LightsAndShadows)

set(CMAKE_C_STANDARD 11)

# 手机端：EGL / GLESv2，eglSwapBuffers 钩子。
# PC 端：桌面 OpenGL + GraphicsDevice.Present 钩子，单独编译 main_pc.c。
if(ANDROID)
    # xDL（MIT，© 2020-2025 HexHacking Team）：Android 符号查找回退。
    add_subdirectory(xdl)
    set_target_properties(xdl PROPERTIES C_VISIBILITY_PRESET hidden)

    add_library(LightsAndShadows SHARED
            main.c
            tefkernel/tef_api_imp.c
    )
    target_include_directories(LightsAndShadows PRIVATE xdl/include)
    target_link_directories(LightsAndShadows PUBLIC ./mod-api ./tefkernel)
    target_link_libraries(LightsAndShadows xdl m dl log EGL GLESv2)
else()
    # main_pc.c 目前是桌面 OpenGL（FNA）后端。
    # Windows 原版走 XNA/D3D9，不能直接复用这套 GL 着色器。
    if(WIN32)
        message(FATAL_ERROR "main_pc.c 目前只支持 Linux/macOS 的 OpenGL 后端；Windows XNA/D3D9 需要单独的渲染实现")
    endif()
    add_library(LightsAndShadows SHARED
            main_pc.c
            tefkernel/tef_api_imp.c
    )
    set_target_properties(LightsAndShadows PROPERTIES
            OUTPUT_NAME "LightsAndShadows.linux.x64")
    target_link_directories(LightsAndShadows PUBLIC ./mod-api ./tefkernel)
    target_link_libraries(LightsAndShadows m dl GL)
endif()
