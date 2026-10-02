# Shared framework types; keep this layer independent of runtime components.
add_library(vinc_common STATIC src/error.cpp)
add_library(vinc::common ALIAS vinc_common)

target_include_directories(vinc_common PUBLIC include)
target_compile_features(vinc_common PUBLIC cxx_std_23)
set_target_properties(vinc_common PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(vinc_common PRIVATE /W4 /utf-8)
else()
    target_compile_options(vinc_common PRIVATE -Wall -Wextra -Wpedantic)
endif()
