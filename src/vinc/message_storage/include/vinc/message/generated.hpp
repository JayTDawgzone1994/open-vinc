#pragma once
#include <vinc/message/storage.hpp>
#include <base_message.hpp>
#include <ostream>
#include <string_view>

namespace vinc::message {
// A scalar field stays bound when assigned, but copying it produces a local value.
template<class T> requires detail::scalar<T>
class Scalar {
    friend class detail::RecordAccess;
public:
    Scalar() = default;
    Scalar(T value) : value_(value) {}
    Scalar(const Scalar& other) : value_(other.get()) {}
    Scalar(Scalar&& other) : Scalar(static_cast<const Scalar&>(other)) {}
    Scalar& operator=(const Scalar& other) { return *this = other.get(); }
    Scalar& operator=(Scalar&& other) { return *this = other.get(); }
    Scalar& operator=(T value) {
        if (binding_.state) detail::checked(detail::write(binding_.state, binding_.handle, 0, detail::encode(value)));
        else value_ = value;
        return *this;
    }
    T get() const { return binding_.state ? detail::decode<T>(detail::checked(detail::read(binding_.state, binding_.handle, 0))) : value_; }
    operator T() const { return get(); }
    Scalar& operator+=(T value) { return *this = static_cast<T>(get() + value); }
    Scalar& operator-=(T value) { return *this = static_cast<T>(get() - value); }
    friend std::ostream& operator<<(std::ostream& stream, const Scalar& value) {
        if constexpr (sizeof(T) == 1 && !std::is_same_v<T, bool>) return stream << +value.get();
        else return stream << value.get();
    }
private:
    Scalar(detail::BindTag, detail::Binding b) : binding_(std::move(b)) {}
    detail::Binding binding_;
    T value_{};
};

class MessageString;
namespace detail {
template<> struct IsArray<MessageString> : std::true_type { using Element = std::uint8_t; };
}
class MessageString {
    friend class detail::RecordAccess;
public:
    MessageString() = default;
    MessageString(std::string_view text) { detail::checked(assign(text)); }
    MessageString(const MessageString&) = default;
    MessageString(MessageString&&) = default;
    MessageString& operator=(const MessageString&) = default;
    MessageString& operator=(MessageString&&) = default;
    MessageString& operator=(std::string_view text) { detail::checked(assign(text)); return *this; }
    Result<void> assign(std::string_view text) {
        auto b = binding();
        auto ready = detail::preflight_assign(b.state, b.handle, text.size());
        if (!ready) return ready;
        return detail::assign(b.state, b.handle, std::as_bytes(std::span(text.data(), text.size())), text.size());
    }
    std::string str() const {
        auto bytes = detail::checked(bytes_.to_vector());
        return {bytes.begin(), bytes.end()};
    }
    operator std::string() const { return str(); }
    std::uint64_t size() const { return bytes_.size(); }
    bool empty() const { return bytes_.empty(); }
    Result<void> resize(std::uint64_t n) { return bytes_.resize(n); }
    Result<void> clear() { return bytes_.clear(); }
    auto operator[](std::uint64_t i) { return bytes_[i]; }
    std::uint8_t operator[](std::uint64_t i) const { return bytes_[i]; }
    friend std::ostream& operator<<(std::ostream& out, const MessageString& text) { return out << text.str(); }
private:
    MessageString(detail::BindTag, detail::Binding b) : bytes_(detail::RecordAccess::bind<MessageArray<std::uint8_t>>(std::move(b))) {}
    detail::Binding binding() const { return detail::RecordAccess::binding(bytes_); }
    MessageArray<std::uint8_t> bytes_;
};

namespace detail {
template<class T> void print(std::ostream& out, const T& value) {
    if constexpr (Record<T>) out << value.to_string();
    else if constexpr (is_array<T> && !std::is_same_v<T, MessageString>) {
        out << '[';
        for (std::uint64_t i = 0; i < value.size(); ++i) { if (i) out << ", "; print(out, value[i]); }
        out << ']';
    } else if constexpr (scalar<T> && sizeof(T) == 1 && !std::is_same_v<T, bool>) out << +value;
    else out << value;
}
}

// Process-local class facade; only its described payload is put in shared memory.
template<class Derived> class GeneratedMessage : public vinc::BaseMessage {
    friend class detail::RecordAccess;
public:
    ~GeneratedMessage() override { if (!binding_.field) detail::release_message(binding_.state); }
    std::uint64_t type_hash() const override { return Derived::schema().hash; }
    static std::uint64_t get_type_hash_static() { return Derived::schema().hash; }
    std::string msg_name_str() const override { return std::string(Derived::message_name); }
    Result<Usage> usage() const { return detail::usage(binding_.state); }
    Result<void> compact() { return detail::compact(binding_.state); }
    Result<Snapshot> snapshot(std::uint64_t maximum = UINT64_MAX) const {
        if (!binding_.field) return detail::snapshot(binding_.state, maximum);
        auto local = detail::clone(binding_);
        if (!local) return std::unexpected(local.error());
        return detail::snapshot(local->state, maximum);
    }
    Result<void> assign(const Derived& other) {
        return detail::replace(binding_, detail::RecordAccess::binding(other));
    }
    static Result<std::unique_ptr<Derived>> from_bytes(std::span<const std::byte> bytes) {
        auto b = detail::receive_record(bytes, Derived::schema());
        if (!b) return std::unexpected(b.error());
        try { return std::unique_ptr<Derived>(new Derived(detail::RecordAccess::bind<Derived>(std::move(*b)))); }
        catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::ResourceExhausted}); }
    }
    void serialize(std::vector<std::uint8_t>& buffer) const override {
        const auto image = detail::checked(snapshot());
        if (image.bytes().size() > buffer.max_size() - buffer.size()) throw AccessError({ErrorCode::SizeOverflow});
        const auto* first = reinterpret_cast<const std::uint8_t*>(image.bytes().data());
        buffer.insert(buffer.end(), first, first + image.bytes().size());
    }
    std::int8_t deserialize(const std::vector<std::uint8_t>& buffer) override {
        if (buffer.size() < 64 || encoded_length(buffer, 0) != buffer.size()) return -1;
        std::size_t offset = 0; return deserialize(buffer, offset);
    }
    std::int8_t deserialize(const std::vector<std::uint8_t>& buffer, std::size_t& offset) override {
        if (offset > buffer.size() || buffer.size() - offset < 64) return -1;
        const auto length = encoded_length(buffer, offset);
        if (length < 64 || length > buffer.size() - offset) return -1;
        auto received = detail::receive_record(std::as_bytes(std::span(buffer).subspan(offset, static_cast<std::size_t>(length))), Derived::schema());
        if (!received) return -1;
        if (!detail::replace(binding_, *received)) return -1;
        offset += static_cast<std::size_t>(length);
        return 0;
    }
protected:
    explicit GeneratedMessage(detail::Binding b) : binding_(std::move(b)) {}
    static detail::Binding copy_binding(const Derived& other) { return detail::checked(detail::clone(detail::RecordAccess::binding(other))); }
    static detail::Binding move_binding(Derived& other) {
        auto& b = static_cast<GeneratedMessage&>(other).binding_;
        if (b.field) return detail::checked(detail::clone(b));
        return std::exchange(b, {});
    }
    detail::Binding binding_;
private:
    detail::Binding binding() const { return binding_; }
    static std::uint64_t encoded_length(const std::vector<std::uint8_t>& b, std::size_t offset) {
        std::uint64_t length = 0;
        for (unsigned i = 0; i < 8; ++i) length |= std::uint64_t(b[offset + 16 + i]) << (8 * i);
        return length;
    }
};
} // namespace vinc::message

namespace vinc {
template<class T, class... Args> requires std::is_base_of_v<message::GeneratedMessage<T>, T>
std::unique_ptr<T> make_message(Args&&... args) { return std::make_unique<T>(std::forward<Args>(args)...); }
}
