#pragma once

// Bounded plain-text presentation for terminals. Raw Hub events remain unchanged.
#include <vinc/time.hpp>
#include <vinc/output_hub/output_hub.hpp>
#include <map>

namespace vinc::output_hub {
// Each destination owns its renderer and calls it from one thread.
class TextRenderer {
public:
    std::string render(const Batch& batch, vinc::Deadline now = std::chrono::steady_clock::now());
    std::string flush(vinc::Deadline now = std::chrono::steady_clock::now(), bool all = false);

private:
    struct Line {
        std::shared_ptr<const Source> source;
        std::string bytes;
        vinc::Deadline since;
    };

    std::map<std::uint64_t, Line> lines_;
    std::string emit(Line& line, bool force);
};
}
