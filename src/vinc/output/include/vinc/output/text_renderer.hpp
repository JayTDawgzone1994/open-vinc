#pragma once

// Bounded plain-text presentation for terminals. Raw Hub events remain unchanged.
#include <vinc/output/hub.hpp>
#include <map>
namespace vinc::output {
class TextRenderer {
public:
    std::string render(const Batch&, osal::Deadline now = std::chrono::steady_clock::now());
    std::string flush(osal::Deadline now = std::chrono::steady_clock::now(), bool all = false);
private:
    struct Line {
        std::shared_ptr<const Source> source;
        std::string bytes;
        osal::Deadline since;
    };
    std::map<std::uint64_t, Line> lines_;
    std::string emit(Line&, bool force);
};
}
