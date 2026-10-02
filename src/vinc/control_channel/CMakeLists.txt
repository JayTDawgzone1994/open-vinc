# Build framed control sessions on OSAL Socket; no application module code belongs here.

add_library(vinc_control src/channel.cpp src/protocol.cpp)
add_library(vinc::control ALIAS vinc_control)
target_include_directories(vinc_control PUBLIC include)
target_link_libraries(vinc_control PUBLIC vinc::osal)
target_compile_features(vinc_control PUBLIC cxx_std_23)
if(MSVC)
  target_compile_options(vinc_control PRIVATE /W4 /utf-8)
else()
  target_compile_options(vinc_control PRIVATE -Wall -Wextra -Wpedantic)
endif()
