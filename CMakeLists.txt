cmake_minimum_required(VERSION 3.22.1)
project(LightsAndShadows)

set(CMAKE_C_STANDARD 11)

# xDL（MIT，© 2020-2025 HexHacking Team）：Android 符号查找回退。
# 普通 dlopen 拿不到 libil2cpp.so（RTLD_LOCAL 加载、不在搜索路径），
# xdl 从 /proc/self/maps 找到已映射的 ELF 再解析符号 —— LuaLoader 就是这么拿到
# il2cpp_gchandle_new 并给纹理钉 GC 根的。
add_subdirectory(xdl)
set_target_properties(xdl PROPERTIES C_VISIBILITY_PRESET hidden)

add_library(LightsAndShadows SHARED
        main.c
        tefkernel/tef_api_imp.c
)
target_include_directories(LightsAndShadows PRIVATE xdl/include)
target_link_directories(LightsAndShadows PUBLIC ./mod-api ./tefkernel)
target_link_libraries(LightsAndShadows xdl m dl log)

set_target_properties(LightsAndShadows PROPERTIES C_VISIBILITY_PRESET hidden)
target_compile_definitions(LightsAndShadows PRIVATE _GNU_SOURCE)
target_compile_options(LightsAndShadows PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
target_link_options(LightsAndShadows PRIVATE -Wl,--no-undefined)
