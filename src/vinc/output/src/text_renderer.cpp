#include <vinc/output/text_renderer.hpp>
#include <algorithm>

namespace vinc::output {
namespace {
// Escape controls (including ESC/C1) and malformed UTF-8. Incomplete trailing
// characters survive ordinary partial-line flushes, but not EOF or a data gap.
std::string plain(std::string& input, bool force) {
    std::string out;
    const auto escape = [&](unsigned char byte) {
        constexpr char hex[] = "0123456789ABCDEF";
        out += "\\x"; out += hex[byte >> 4]; out += hex[byte & 15];
    };
    std::size_t i = 0;
    while (i < input.size()) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c < 128) {
            if (c < 32 || c == 127) escape(c); else out += static_cast<char>(c);
            ++i; continue;
        }
        const std::size_t length = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!length) { escape(c); ++i; continue; }
        if (input.size() - i < length) {
            if (!force) break;
            escape(c); ++i; continue;
        }
        std::uint32_t code = c & (length == 2 ? 31 : length == 3 ? 15 : 7);
        bool valid = true;
        for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(input[i + j]);
            if ((next & 0xc0) != 0x80) { valid = false; break; }
            code = (code << 6) | (next & 63);
        }
        valid = valid && code >= (length == 2 ? 0x80u : length == 3 ? 0x800u : 0x10000u) &&
            code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff);
        if (!valid) { escape(c); ++i; continue; }
        if (code >= 0x80 && code <= 0x9f) for (std::size_t j = 0; j < length; ++j) escape(static_cast<unsigned char>(input[i + j]));
        else out.append(input, i, length);
        i += length;
    }
    input.erase(0, i);
    return out;
}
std::string prefix(const Source& source) {
    auto name = source.name;
    return "[" + plain(name, true) + "#" + std::to_string(source.generation) +
        (source.stream == Stream::Stdout ? " stdout] " : " stderr] ");
}
}
std::string TextRenderer::emit(Line& line, bool force) {
    auto text = plain(line.bytes, force);
    if (text.empty()) return {};
    return prefix(*line.source) + text + '\n';
}
std::string TextRenderer::render(const Batch& batch, osal::Deadline now) {
    std::string out;
    if (batch.skipped.events) {
        lines_.clear(); // Never join fragments across a destination's missing data.
        out += "[output] skipped " + std::to_string(batch.skipped.bytes) + " bytes / " +
            std::to_string(batch.skipped.events) + " events; partial text reset\n";
    }
    for (const auto& event : batch.events) {
        auto it = lines_.find(event->source->id);
        if (it == lines_.end()) {
            if (lines_.size() == 256) { out += emit(lines_.begin()->second, true); lines_.erase(lines_.begin()); }
            it = lines_.emplace(event->source->id, Line{event->source, {}, now}).first;
        }
        auto& line = it->second;
        for (char ch : event->bytes) {
            if (ch == '\n') {
                if (!line.bytes.empty() && line.bytes.back() == '\r') line.bytes.pop_back();
                out += line.bytes.empty() ? prefix(*line.source) + '\n' : emit(line, true);
                line.since = now;
            } else {
                if (line.bytes.empty()) line.since = now;
                line.bytes += ch;
                if (line.bytes.size() >= 4096) { out += emit(line, false); line.since = now; }
            }
        }
        if (event->kind != EventKind::Data) {
            out += emit(line, true);
            if (event->kind == EventKind::Truncated) out += prefix(*line.source) + "[output not fully drained: deadline expired]\n";
            if (event->kind == EventKind::Failed) out += prefix(*line.source) + "[output read failed]\n";
            lines_.erase(it);
        }
    }
    out += flush(now);
    return out;
}
std::string TextRenderer::flush(osal::Deadline now, bool all) {
    std::string out;
    for (auto it = lines_.begin(); it != lines_.end();) {
        if (all || now - it->second.since >= std::chrono::milliseconds(100)) {
            // After the display deadline even an incomplete UTF-8 suffix is
            // made visible; arbitrary binary output cannot wait indefinitely.
            out += emit(it->second, true);
            it = lines_.erase(it);
        } else ++it;
    }
    return out;
}
}
