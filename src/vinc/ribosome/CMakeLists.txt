cmake_minimum_required(VERSION 3.15)
project(ribosome LANGUAGES CXX)

# 因為你用了 line.starts_with("//")，這需要 C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
# 1. 指定標頭檔目錄
# 這樣你在程式碼裡就能用 #include <ribosome/ribosome.hpp>
include_directories(include)

# 2. 收集原始碼檔案
# 如果你有新增 main.cpp，記得加進去
set(SOURCES
    src/ribosome.cpp
    # src/main.cpp  <-- 如果有進入點檔案請取消註解
)

# 3. 產出執行檔
add_executable(${PROJECT_NAME} ${SOURCES})

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ribosome/ribosome_config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ribosome_config.hpp"
)

target_include_directories(ribosome PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

# 4. 如果是在較舊的 Linux 環境，可能需要手動連結 libstdc++fs
# target_link_libraries(${PROJECT_NAME} stdc++fs)

# 5. 編譯器警告設定 (建議開啟，可以幫你抓到潛在錯誤)
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()