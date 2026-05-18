#pragma once
#include <string_view>

namespace holon 
{
    // 透過 @ 符號，CMake 會在任何作業系統、任何編譯器下，精準填入無引號污染的絕對路徑
    constexpr std::string_view kDefaultInputPath = "/home/dawgzone/open_vinc/src/messages";
    constexpr std::string_view kDefaultOutputPath = "/home/dawgzone/open_vinc/src/holon/holon_messages/include/holon_messages";
}
