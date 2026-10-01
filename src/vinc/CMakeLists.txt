cmake_minimum_required(VERSION 3.20)
project(vinc LANGUAGES CXX)

# =================================================================
# ⚙️ 1. 全域 C++23 標準設定
# =================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source files contain UTF-8 text; do not depend on the Windows code page.
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

# 初始化全域模組註冊清單
set_property(GLOBAL PROPERTY VINC_MODULES_LIST "")

# =================================================================
# 🧬 2. 宣告訊息產生器任務（核糖體工廠）
# =================================================================
# 這裡先定義任務藍圖，此時 ribosome 執行檔還不需要真正出生
add_custom_target(generate_messages
    COMMAND $<TARGET_FILE:ribosome>
    COMMENT "🧬 Ribosome is synthesizing messages from inputs..."
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM
)

# =================================================================
# 🛠️ 3. 工業級自動化：子模組註冊巨集函式
# =================================================================
# 未來任何子元件只要呼叫這個函式，就會自動綁定所有核心依賴與編譯順序
function(vinc_add_module TARGET_NAME)
    # 將模組名稱紀錄至全域清單中
    get_property(current_list GLOBAL PROPERTY VINC_MODULES_LIST)
    list(APPEND current_list ${TARGET_NAME})
    set_property(GLOBAL PROPERTY VINC_MODULES_LIST "${current_list}")
    
    # 🔒 自動連結基礎 module 框架
    target_link_libraries(${TARGET_NAME} PRIVATE module)
    
    # 🔒 自動連結核心通訊公車 axon_bus
    # 這一行會觸發 CMake 黑魔法，強制 axon_bus 必須比該子模組先編譯完成
    target_link_libraries(${TARGET_NAME} PRIVATE axon_bus)
    
    # axon_bus publicly supplies vinc_messages, including generation ordering.
    
    message(STATUS "[Holon] Registered module: ${TARGET_NAME}")
endfunction()

# =================================================================
# 📂 4. 載入所有子專案目錄（Targets 在此正式誕生）
# =================================================================
# 這裡的載入順序不會影響底部的鋼鐵鎖鏈，但建議依邏輯層次排列
add_subdirectory(ribosome)       # 訊息生成器執行檔 (ribosome)
add_subdirectory(osal)           # Independent OS process primitives
add_subdirectory(message_storage) # Relocatable arrays; independent of Bus and Ribosome
add_subdirectory(control_channel) # Shared framed Socket sessions
add_subdirectory(bus)             # Shared-memory publish/subscribe
add_subdirectory(vinc_messages)  # Generated-header usage requirements
add_subdirectory(axon_bus)       # 核心無鎖通訊公車庫 (axon_bus)
add_subdirectory(module)         # 基礎模組框架庫 (module)

# 系統應用元件與測試（內部會呼叫 vinc_add_module）
add_subdirectory(vinc_test)
add_subdirectory(module_example)
add_subdirectory(bus_example)
add_subdirectory(stem_shell)

# =================================================================
# 🔒 5. 終極編譯鎖鏈（時空順序正位）
# =================================================================
# 必須寫在 add_subdirectory 的下方，此時 CMake 腦中才有 ribosome 與 axon_bus 的記憶

# 【鎖鏈一】強迫「執行代碼生成任務」前，必須先把 ribosome.exe 自己編譯出來！
add_dependencies(generate_messages ribosome)

# axon_bus and its consumers inherit generation ordering through vinc_messages.
