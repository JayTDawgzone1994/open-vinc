#pragma once
#include <vinc/error.hpp>
#include <optional>

namespace vinc::output_hub::test {
enum class Point {
    CreateService,
    ServiceCreated,
    Publish
};
// Only isolated fault targets define these functions. No production registry.
void checkpoint(Point);
std::optional<vinc::Error> monitor_error(std::optional<vinc::Error> actual);
}
