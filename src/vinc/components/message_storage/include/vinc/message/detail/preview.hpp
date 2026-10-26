#pragma once

// Framework diagnostic seam: inspect an already validated LOCAL generated message.
// Never pass a live shared write loan. Traversal is bounded independently of payload size.
#include <vinc/message/generated.hpp>
#include <iomanip>
#include <sstream>

namespace vinc::message::detail {
struct PreviewLimits {
    std::size_t bytes;
    std::size_t elements;
    std::size_t nodes;
    std::size_t depth;
    std::size_t string_bytes;
};

class PreviewWriter {
    PreviewLimits limits_;
    std::string text_;
    std::size_t visited_ = 0;

    template<class T> struct ValueType {
        using Type = T;
    };

    template<class T> struct ValueType<Scalar<T>> {
        using Type = T;
    };

    template<class T> struct Fields {
        PreviewWriter& writer;
        Binding binding;
        std::size_t depth;
        bool first = true;

        template<class Field> void field(const char* name, std::uint64_t index) {
            if (writer.full()) {
                return;
            }

            if (!first) {
                writer.append(", ");
            }
            first = false;
            writer.append(name);
            writer.append(": ");
            writer.template value<typename ValueType<Field>::Type>(detail::field(binding, index), depth + 1);
        }
    };

public:
    explicit PreviewWriter(PreviewLimits limits) : limits_(limits) {
    }

    bool full() const {
        return text_.size() >= limits_.bytes || visited_ >= limits_.nodes;
    }

    void append(std::string_view text) {
        text_.append(text.substr(0, limits_.bytes - text_.size()));
    }

    template<class T> void value(Binding binding, std::size_t depth = 0) {
        if (full() || depth >= limits_.depth) {
            append("...");
            return;
        }

        ++visited_;
        if constexpr (scalar<T>) {
            const auto bytes = read(binding.state, binding.handle, 0);
            if (!bytes) {
                append("<unavailable>");
                return;
            }

            std::ostringstream stream;
            stream << std::setprecision(std::numeric_limits<T>::max_digits10);
            if constexpr (std::is_same_v<T, bool>) {
                stream << (decode<T>(*bytes) ? "true" : "false");
            } else if constexpr (sizeof(T) == 1) {
                stream << +decode<T>(*bytes);
            } else {
                stream << decode<T>(*bytes);
            }
            append(stream.str());
        } else if constexpr (std::is_same_v<T, MessageString>) {
            const auto size = info(binding.state, binding.handle);
            if (!size) {
                append("<unavailable>");
                return;
            }

            append("\"");
            const auto count = (std::min)(size->size, static_cast<std::uint64_t>(limits_.string_bytes));
            for (std::uint64_t index = 0; index < count && !full(); ++index) {
                const auto bytes = read(binding.state, binding.handle, index);
                if (!bytes) {
                    break;
                }

                const auto byte = decode<std::uint8_t>(*bytes);
                if (byte >= 32 && byte < 127 && byte != '"' && byte != '\\') {
                    append(std::string(1, static_cast<char>(byte)));
                } else {
                    // Strings are byte sequences, not promised UTF-8. Escape non-ASCII
                    // bytes explicitly rather than guessing an encoding or inserting HTML.
                    constexpr char hex[] = "0123456789abcdef";
                    const char escaped[] = {'\\', 'x', hex[byte >> 4], hex[byte & 15]};
                    append(std::string_view(escaped, sizeof escaped));
                }
            }

            if (count < size->size) {
                append("...");
            }
            append("\"");
        } else if constexpr (is_array<T>) {
            using Element = typename IsArray<T>::Element;
            const auto size = info(binding.state, binding.handle);
            if (!size) {
                append("<unavailable>");
                return;
            }

            append("[");
            const auto count = (std::min)(size->size, static_cast<std::uint64_t>(limits_.elements));
            for (std::uint64_t index = 0; index < count && !full(); ++index) {
                if (index) {
                    append(", ");
                }

                if constexpr (scalar<Element>) {
                    const auto bytes = read(binding.state, binding.handle, index);
                    if (!bytes) {
                        append("<unavailable>");
                        break;
                    }

                    ++visited_;
                    std::ostringstream stream;
                    stream << std::setprecision(std::numeric_limits<Element>::max_digits10);
                    if constexpr (std::is_same_v<Element, bool>) {
                        stream << (decode<Element>(*bytes) ? "true" : "false");
                    } else if constexpr (sizeof(Element) == 1) {
                        stream << +decode<Element>(*bytes);
                    } else {
                        stream << decode<Element>(*bytes);
                    }
                    append(stream.str());
                } else {
                    const auto node = child(binding.state, binding.handle, index);
                    if (!node) {
                        append("<unavailable>");
                        break;
                    }

                    value<Element>({binding.state, *node, true}, depth + 1);
                }
            }

            if (count < size->size || full()) {
                append(", ...");
            }
            append("] (size=");
            append(std::to_string(size->size));
            append(")");
        } else {
            append("{");
            T::ribosome_visit_fields(Fields<T>{*this, binding, depth});
            append("}");
        }
    }

    std::string finish() {
        if (full() && text_.size() >= 3) {
            text_.replace(text_.size() - 3, 3, "...");
        }

        return std::move(text_);
    }
};

template<Record T> std::string preview(const T& message, PreviewLimits limits) {
    PreviewWriter writer(limits);
    writer.value<T>(RecordAccess::binding(message));
    return writer.finish();
}
}
