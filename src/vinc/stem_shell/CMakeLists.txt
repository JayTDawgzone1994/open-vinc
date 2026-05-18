cmake_minimum_required(VERSION 3.15)
project(stem_shell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(include)

set(SOURCES
    src/stem_shell.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE 
    module
)

get_property(ALL_VINC_MODULES GLOBAL PROPERTY VINC_MODULES_LIST)


# 重要：解決「懶惰連結」問題
if(MSVC)
    # MSVC 使用 /WHOLEARCHIVE 強制載入所有符號
    foreach(mod ${ALL_VINC_MODULES})
        target_link_libraries(${PROJECT_NAME} PRIVATE -WHOLEARCHIVE:${mod})
    endforeach()
else()
    # GCC/Clang 使用 --whole-archive
    target_link_libraries(${PROJECT_NAME} PRIVATE 
        -Wl,--whole-archive ${ALL_VINC_MODULES} -Wl,--no-whole-archive
    )
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()