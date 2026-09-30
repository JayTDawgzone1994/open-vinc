cmake_minimum_required(VERSION 3.20)
project(vinc_root LANGUAGES CXX)

include(CTest)

add_subdirectory(vinc)


# Windows command-line build (from an x64 MSVC developer environment):
# cmake -S src -B build/windows-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release
# cmake --build build/windows-ninja
# Alternatively, from this directory: cmake --preset windows-msvc-release
