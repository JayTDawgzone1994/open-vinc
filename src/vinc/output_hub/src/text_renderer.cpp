#include <vinc/time.hpp>
#include <vinc/output_hub/text_renderer.hpp>
#include <algorithm>

namespace vinc::output_hub {
namespace {
// Escape controls (including ESC/C1) and malformed UTF-8. Incomplete trailing
// characters survive ordinary partial-line flushes, but not EOF or a data gap.
std::string consume_display_text(std::string& input, bool force) {
    std::string output;
    const auto escape = [&](unsigned char byte) {
        constexpr char hex[] = "0123456789ABCDEF";
        output += "\\x";
        output += hex[byte >> 4];
        output += hex[byte & 15];
    };
    std::size_t offset = 0;
    while (offset < input.size()) {
        const auto lead_byte = static_cast<unsigned char>(input[offset]);
        if (lead_byte < 128) {
            if (lead_byte < 32 || lead_byte == 127) {
                escape(lead_byte);
            } else {
                output += static_cast<char>(lead_byte);
            }
            ++offset;
            continue;
        }

        // UTF-8 encoding constants belong here, not in configurable system policy.
        std::size_t sequence_bytes = 0;
        std::uint32_t lead_mask = 0;
        std::uint32_t minimum_code_point = 0;
        if (lead_byte >= 0xc2 && lead_byte <= 0xdf) {
            sequence_bytes = 2;
            lead_mask = 0x1f;
            minimum_code_point = 0x80;
        } else if (lead_byte >= 0xe0 && lead_byte <= 0xef) {
            sequence_bytes = 3;
            lead_mask = 0x0f;
            minimum_code_point = 0x800;
        } else if (lead_byte >= 0xf0 && lead_byte <= 0xf4) {
            sequence_bytes = 4;
            lead_mask = 0x07;
            minimum_code_point = 0x10000;
        }

        if (!sequence_bytes) {
            escape(lead_byte);
            ++offset;
            continue;
        }

        if (input.size() - offset < sequence_bytes) {
            if (!force) {
                break;
            }

            escape(lead_byte);
            ++offset;
            continue;
        }

        std::uint32_t code_point = lead_byte & lead_mask;
        bool valid = true;
        for (std::size_t j = 1; j < sequence_bytes; ++j) {
            const auto continuation = static_cast<unsigned char>(input[offset + j]);
            if ((continuation & 0xc0) != 0x80) {
                valid = false;
                break;
            }

            code_point = (code_point << 6) | (continuation & 63);
        }
        const bool is_surrogate = code_point >= 0xd800 && code_point <= 0xdfff;
        // Reject overlong sequences, surrogate values and out-of-range scalars.
        valid = valid && code_point >= minimum_code_point && code_point <= 0x10ffff && !is_surrogate;
        if (!valid) {
            escape(lead_byte);
            ++offset;
            continue;
        }

        if (code_point >= 0x80 && code_point <= 0x9f) {
            for (std::size_t j = 0; j < sequence_bytes; ++j) {
                escape(static_cast<unsigned char>(input[offset + j]));
            }
        } else {
            output.append(input, offset, sequence_bytes);
        }
        offset += sequence_bytes;
    }
    // Keep only an incomplete suffix when the caller permits another fragment.
    input.erase(0, offset);
    return output;
}

std::string prefix(const Source& source) {
    auto name = source.name;
    return "[" + consume_display_text(name, true) + "#" + std::to_string(source.generation) +
           (source.stream == Stream::Stdout ? " stdout] " : " stderr] ");
}
}

std::string TextRenderer::emit(Line& line, bool force) {
    auto text = consume_display_text(line.bytes, force);
    if (text.empty()) {
        return {};
    }

    return prefix(*line.source) + text + '\n';
}

std::string TextRenderer::render(const Batch& batch, vinc::Deadline now) {
    std::string output;
    if (batch.skipped.events) {
        lines_.clear(); // Never join fragments across a destination's missing data.
        output += "[output] skipped " + std::to_string(batch.skipped.bytes) + " bytes / " +
                  std::to_string(batch.skipped.events) + " events; partial text reset\n";
    }
    for (const auto& event : batch.events) {
        auto line_entry = lines_.find(event->source->id);
        if (line_entry == lines_.end()) {
            // Preserve the existing policy: evict the lowest source ID, not LRU.
            if (lines_.size() == config::output_hub_text_max_sources) {
                output += emit(lines_.begin()->second, true);
                lines_.erase(lines_.begin());
            }
            line_entry = lines_.emplace(event->source->id, Line{event->source, {}, now}).first;
        }
        auto& line = line_entry->second;
        for (char ch : event->bytes) {
            if (ch == '\n') {
                if (!line.bytes.empty() && line.bytes.back() == '\r') {
                    line.bytes.pop_back();
                }
                output += line.bytes.empty() ? prefix(*line.source) + '\n' : emit(line, true);
                line.since = now;
            } else {
                if (line.bytes.empty()) {
                    line.since = now;
                }
                line.bytes += ch;
                if (line.bytes.size() >= config::output_hub_text_line_bytes) {
                    output += emit(line, false);
                    line.since = now;
                }
            }
        }
        if (event->kind != EventKind::Data) {
            output += emit(line, true);
            if (event->kind == EventKind::Truncated) {
                output += prefix(*line.source) + "[output not fully drained: deadline expired]\n";
            }
            if (event->kind == EventKind::Failed) {
                output += prefix(*line.source) + "[output read failed]\n";
            }
            lines_.erase(line_entry);
        }
    }
    output += flush(now);
    return output;
}

std::string TextRenderer::flush(vinc::Deadline now, bool all) {
    std::string output;
    for (auto line_entry = lines_.begin(); line_entry != lines_.end();) {
        if (all || now - line_entry->second.since >= config::output_hub_text_flush_interval) {
            // After the display deadline even an incomplete UTF-8 suffix is
            // made visible; arbitrary binary output cannot wait indefinitely.
            output += emit(line_entry->second, true);
            line_entry = lines_.erase(line_entry);
        } else {
            ++line_entry;
        }
    }
    return output;
}
}
