cmake_minimum_required(VERSION 3.10)
project(lswtcs_arm64 C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(lswtcs_arm64
    src/main.c
    src/so_util.c
    src/imports.c
    src/android_shim.c
    src/egl_shim.c
    src/opensles_shim.c
    src/jni_shim.c
    src/util.c
    src/error.c
)

target_include_directories(lswtcs_arm64 PRIVATE
    ${SDL2_INCLUDE_DIRS}
    src
)

target_link_libraries(lswtcs_arm64 PRIVATE
    ${SDL2_LIBRARIES}
    GLESv2
    EGL
    m
    dl
    pthread
    stdc++
)

target_compile_options(lswtcs_arm64 PRIVATE
    ${SDL2_CFLAGS_OTHER}
    -Wall -Wextra -Wno-unused-parameter
)
