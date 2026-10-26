#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace vinc::shell {
// Own the text across asynchronous dispatch. An engaged empty string is still
// an embedded script; a missing string means that the worker reads the file.
struct ScriptSource {
    std::filesystem::path path;
    std::optional<std::string> text;

    bool empty() const {
        return path.empty() && !text;
    }

    static ScriptSource embedded(std::string contents) {
        return {"embedded:startup.tcl", std::move(contents)};
    }
};
}
