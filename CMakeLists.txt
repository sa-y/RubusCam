cmake_minimum_required(VERSION 3.12)
project(RubusCam LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler optimization and warnings
add_compile_options(-Wall -Wextra -O2)

find_package(PkgConfig REQUIRED)

# Check for libcamera
pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)

# Check for SDL2, SDL2_image & SDL2_ttf
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_IMAGE REQUIRED IMPORTED_TARGET SDL2_image)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)

# Check for pthreads
find_package(Threads REQUIRED)

# Source files (main application and text renderer in C, libcamera wrapper in C++)
set(SOURCES
    src/main.c
    src/camera_stream.cpp
    src/text_renderer.c
    src/dcf_storage.c
)

add_executable(rubuscam ${SOURCES})

target_compile_definitions(rubuscam PRIVATE
    _POSIX_C_SOURCE=200809L
)

target_include_directories(rubuscam PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rubuscam PRIVATE
    PkgConfig::LIBCAMERA
    PkgConfig::SDL2
    PkgConfig::SDL2_IMAGE
    PkgConfig::SDL2_TTF
    Threads::Threads
)
