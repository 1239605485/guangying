cmake_minimum_required(VERSION 4.2)
project(LightsAndShadows)

set(CMAKE_C_STANDARD 11)

# xDL（MIT，© 2020-2025 HexHacking Team）：Android 符号查找回退。
# 当前 GL 技术线已不建 Unity 贴图（不需要 il2cpp_gchandle_new），保留 xdl 只是
# 为以后可能要用 il2cpp 符号留的口子；不引用任何符号时静态库不会拉进产物。
add_subdirectory(xdl)
set_target_properties(xdl PROPERTIES C_VISIBILITY_PRESET hidden)

# EGL / GLESv2：GL 后处理（eglSwapBuffers 钩子 + 自己的着色器）。
# 必须真的链上，否则产物没有 DT_NEEDED，dlopen 时找不到 eglSwapBuffers。
add_library(LightsAndShadows SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_include_directories(LightsAndShadows PRIVATE xdl/include)
target_link_directories(LightsAndShadows PUBLIC ./mod-api ./tefkernel)
target_link_libraries(LightsAndShadows xdl m dl log EGL GLESv2)
