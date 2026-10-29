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
    # PC 端使用 XNA API（GetBackBufferData + Texture2D + SpriteBatch），
    # 不依赖 GL/D3D，Windows / Linux 共用 main_pc.c。
    add_library(LightsAndShadows SHARED
            main_pc.c
            tefkernel/tef_api_imp.c
    )
    target_link_directories(LightsAndShadows PUBLIC ./mod-api ./tefkernel)
    target_link_libraries(LightsAndShadows m)
endif()
