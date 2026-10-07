#pragma once

// Runtime support used by Ribosome-generated classes: bound scalar/string fields,
// value-copy behavior and schema-checked conversion between classes and byte images.

#include <vinc/message/storage.hpp>
#include <vinc/message/detail/image_format.hpp>
#include <vinc/message/base_message.hpp>
#include <ostream>
#include <string_view>

namespace vinc::message {
// A scalar field stays bound when assigned, but copying it produces a local value.
template <class T>
    requires detail::scalar<T>
class Scalar {
    friend class detail::RecordAccess;

public:
    Scalar() = default;

    Scalar(T value) : value_(value) {
    }

    Scalar(const Scalar& other) : value_(other.get()) {
    }

    Scalar(Scalar&& other) : Scalar(static_cast<const Scalar&>(other)) {
    }

    Scalar& operator=(const Scalar& other) {
        return *this = other.get();
    }

    Scalar& operator=(Scalar&& other) {
        return *this = other.get();
    }

    Scalar& operator=(T value) {
        if (binding_.state) {
            detail::contract_value(detail::write(binding_.state, binding_.handle, 0, detail::encode(value)));
        } else {
            value_ = value;
        }

        return *this;
    }

    T get() const {
        if (binding_.state) {
            return detail::decode<T>(
                detail::contract_value(detail::read(binding_.state, binding_.handle, 0)));
        }

        return value_;
    }

    operator T() const {
        return get();
    }

    Scalar& operator+=(T value) {
        return *this = static_cast<T>(get() + value);
    }

    Scalar& operator-=(T value) {
        return *this = static_cast<T>(get() - value);
    }

    friend std::ostream& operator<<(std::ostream& stream, const Scalar& value) {
        if constexpr (sizeof(T) == 1 && !std::is_same_v<T, bool>) {
            return stream << +value.get();
        } else {
            return stream << value.get();
        }
    }

private:
    Scalar(detail::BindTag, detail::Binding bound) : binding_(std::move(bound)) {
    }

    detail::Binding binding_;
    T value_{};
};

class MessageString;

namespace detail {
template <> struct IsArray<MessageString> : std::true_type {
    using Element = std::uint8_t;
};
}

// String bytes share the parent arena and byte budget. str() returns a local copy;
// there is no borrowed std::string pointer into shared memory.
class MessageString {
    friend class detail::RecordAccess;

public:
    using StringTag = void;
    MessageString() = default;

    MessageString(std::string_view) = delete;
    MessageString(const MessageString&) = default;
    MessageString(MessageString&&) = default;
    MessageString& operator=(const MessageString&) = delete;
    MessageString& operator=(MessageString&&) = delete;
    MessageString& operator=(std::string_view) = delete;

    [[nodiscard]] Result<void> assign(const MessageString& other) {
        return bytes_.assign(other.bytes_);
    }

    Result<void> assign(std::string_view text) {
        auto bound = binding();
        (void)detail::node_epoch(bound.state, bound.handle);
        auto ready = detail::preflight_assign(bound.state, bound.handle, text.size());
        if (!ready) {
            return ready;
        }

        return detail::assign(bound.state, bound.handle, std::as_bytes(std::span(text.data(), text.size())),
                              text.size());
    }

    std::string str() const {
        const auto count = bytes_.size();
        std::string output(static_cast<std::size_t>(count), '\0');
        for (std::uint64_t i = 0; i < count; ++i) {
            output[static_cast<std::size_t>(i)] = static_cast<char>(bytes_[i]);
        }
        return output;
    }

    operator std::string() const {
        return str();
    }

    std::uint64_t size() const {
        return bytes_.size();
    }

    bool empty() const {
        return bytes_.empty();
    }

    Result<void> resize(std::uint64_t n) {
        return bytes_.resize(n);
    }

    Result<void> clear() {
        return bytes_.clear();
    }

    auto operator[](std::uint64_t i) {
        return bytes_[i];
    }

    std::uint8_t operator[](std::uint64_t i) const {
        return bytes_[i];
    }

    friend std::ostream& operator<<(std::ostream& out, const MessageString& text) {
        return out << text.str();
    }

private:
    MessageString(detail::BindTag, detail::Binding bound)
        : bytes_(detail::RecordAccess::bind<MessageArray<std::uint8_t>>(std::move(bound))) {
    }

    detail::Binding binding() const {
        return detail::RecordAccess::binding(bytes_);
    }

    MessageArray<std::uint8_t> bytes_;
};

template <class T, bool ReadOnly> class RecordRef {
    friend class detail::RecordAccess;

public:
    using ProxyTag = void;
    using ValueType = T;

    explicit RecordRef(detail::Access access) : access_(std::move(access)) {
    }

    RecordRef(const RecordRef&) = default;
    RecordRef(RecordRef&&) = default;
    RecordRef& operator=(const RecordRef&) = delete;
    RecordRef& operator=(RecordRef&&) = delete;

    auto data() {
        detail::validate_access(access_);
        return typename T::template DataRef<ReadOnly>(access_);
    }

    auto data() const {
        detail::validate_access(access_);
        return typename T::template DataRef<true>(access_);
    }

    [[nodiscard]] Result<T> copy() const {
        return detail::copy_element<T>(access_);
    }

    template <class U>
    [[nodiscard]] Result<void> assign(const U& other)
        requires(!ReadOnly)
    {
        auto destination = binding();
        auto source = detail::source_binding(other);
        if (!source) {
            return std::unexpected(source.error());
        }

        return detail::replace(destination, *source);
    }

    std::string to_string() const {
        return T::to_string(data());
    }

private:
    detail::Binding binding() const {
        detail::validate_access(access_);
        if (!access_.state) {
            vinc::contract_violation("immutable record has no mutable binding");
        }

        return {access_.state, access_.handle, true};
    }

    detail::Access access_;
};

template <bool ReadOnly> class StringRef : public ArrayRef<std::uint8_t, ReadOnly> {
    friend class detail::RecordAccess;

public:
    using ValueType = MessageString;

    explicit StringRef(detail::Access access)
        : ArrayRef<std::uint8_t, ReadOnly>(access), access_(std::move(access)) {
    }

    StringRef(const StringRef&) = default;
    StringRef(StringRef&&) = default;
    StringRef& operator=(const StringRef&) = delete;
    StringRef& operator=(StringRef&&) = delete;

    [[nodiscard]] Result<MessageString> copy() const {
        return detail::copy_element<MessageString>(access_);
    }

    [[nodiscard]] Result<void> assign(const MessageString& value)
        requires(!ReadOnly)
    {
        return ArrayRef<std::uint8_t, ReadOnly>::assign(value);
    }

    template <bool OtherReadOnly>
    [[nodiscard]] Result<void> assign(const StringRef<OtherReadOnly>& value)
        requires(!ReadOnly)
    {
        return ArrayRef<std::uint8_t, ReadOnly>::assign(value);
    }

    [[nodiscard]] Result<void> assign(std::string_view text)
        requires(!ReadOnly)
    {
        auto destination = binding();
        auto ready = detail::preflight_assign(destination.state, destination.handle, text.size());
        if (!ready) {
            return ready;
        }

        return detail::assign(destination.state, destination.handle,
                              std::as_bytes(std::span(text.data(), text.size())), text.size());
    }

    std::string str() const {
        const auto count = this->size();
        std::string output(static_cast<std::size_t>(count), '\0');
        for (std::uint64_t i = 0; i < count; ++i) {
            output[static_cast<std::size_t>(i)] = static_cast<char>((*this)[i]);
        }
        return output;
    }

    friend std::ostream& operator<<(std::ostream& out, const StringRef& value) {
        return out << value.str();
    }

private:
    detail::Binding binding() const {
        detail::validate_access(access_);
        if (!access_.state) {
            vinc::contract_violation("immutable string has no mutable binding");
        }

        return {access_.state, access_.handle, true};
    }

    detail::Access access_;
};

// Generator field types select borrowing wrappers without constructing owning T.
template <class T, bool ReadOnly> struct FieldRefType {
    using type = decltype(detail::make_element<T, ReadOnly>(std::declval<detail::Access>()));
};

template <class T, bool ReadOnly> struct FieldRefType<Scalar<T>, ReadOnly> {
    using type = ScalarRef<T, ReadOnly>;
};
template <class T, bool ReadOnly> using FieldRef = typename FieldRefType<T, ReadOnly>::type;

namespace detail {
template <class T> void print(std::ostream& out, const T& value) {
    if constexpr (requires { value.to_string(); }) {
        out << value.to_string();
    } else if constexpr (requires { value.str(); }) {
        out << value.str();
    } else if constexpr ((is_array<T> && !std::is_same_v<T, MessageString>) ||
                         requires { value.to_vector(); }) {
        out << '[';
        for (std::uint64_t i = 0; i < value.size(); ++i) {
            if (i) {
                out << ", ";
            }

            print(out, value[i]);
        }

        out << ']';
    } else if constexpr (scalar<T> && sizeof(T) == 1 && !std::is_same_v<T, bool>) {
        out << +value;
    } else {
        out << value;
    }
}
}

// Process-local class facade; only its described payload is put in shared memory.
template <class Derived> class GeneratedMessage : public vinc::BaseMessage {
    friend class detail::RecordAccess;

public:
    ~GeneratedMessage() override {
        if (!binding_.field) {
            detail::release_message(binding_.state);
        }
    }

    std::uint64_t type_hash() const override {
        return Derived::schema().hash;
    }

    static std::uint64_t get_type_hash_static() {
        return Derived::schema().hash;
    }

    std::string msg_name_str() const override {
        return std::string(Derived::message_name);
    }

    Result<Usage> usage() const {
        (void)detail::node_epoch(binding_.state, binding_.handle);
        return detail::usage(binding_.state);
    }

    Result<void> compact() {
        (void)detail::node_epoch(binding_.state, binding_.handle);
        return detail::compact(binding_.state);
    }

    Result<Snapshot> snapshot(std::uint64_t maximum = UINT64_MAX) const {
        (void)detail::node_epoch(binding_.state, binding_.handle);
        if (!binding_.field) {
            return detail::snapshot(binding_.state, maximum);
        }

        auto local = detail::clone(binding_);
        if (!local) {
            return std::unexpected(local.error());
        }

        return detail::snapshot(local->state, maximum);
    }

    Result<void> assign(const Derived& other) {
        (void)detail::node_epoch(binding_.state, binding_.handle);
        (void)detail::node_epoch(other.binding_.state, other.binding_.handle);
        return detail::replace(binding_, detail::RecordAccess::binding(other));
    }

    static Result<std::unique_ptr<Derived>> from_bytes(std::span<const std::byte> bytes) {
        auto bound = detail::receive_record(bytes, Derived::schema());
        if (!bound) {
            return std::unexpected(bound.error());
        }

        try {
            return std::unique_ptr<Derived>(
                new Derived(detail::RecordAccess::bind<Derived>(std::move(*bound))));
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Bind});
        }
    }

    [[nodiscard]] Result<void> serialize(std::vector<std::uint8_t>& buffer) const override {
        auto image = snapshot();
        if (!image) {
            return std::unexpected(image.error());
        }

        if (image->bytes().size() > buffer.max_size() - buffer.size()) {
            return std::unexpected(Error{ErrorCode::SizeOverflow, ErrorOperation::Serialize});
        }

        // uint8_t is unsigned char on supported targets; only byte-wise aliasing
        // is used, with no native message layout or alignment assumptions.
        const auto* first = reinterpret_cast<const std::uint8_t*>(image->bytes().data());
        try {
            buffer.insert(buffer.end(), first, first + image->bytes().size());
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Serialize});
        } catch (const std::length_error&) {
            return std::unexpected(Error{ErrorCode::SizeOverflow, ErrorOperation::Serialize});
        }
    }

    [[nodiscard]] Result<void> deserialize(const std::vector<std::uint8_t>& buffer) override {
        (void)detail::node_epoch(binding_.state, binding_.handle);
        if (buffer.size() < detail::format::header::size || encoded_length(buffer, 0) != buffer.size()) {
            return std::unexpected(Error{ErrorCode::InvalidData, ErrorOperation::Deserialize});
        }

        std::size_t offset = 0;
        return deserialize(buffer, offset);
    }

    [[nodiscard]] Result<void> deserialize(const std::vector<std::uint8_t>& buffer,
                                         std::size_t& offset) override {
        (void)detail::node_epoch(binding_.state, binding_.handle);
        if (offset > buffer.size()) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Deserialize});
        }

        if (buffer.size() - offset < detail::format::header::size) {
            return std::unexpected(Error{ErrorCode::InvalidData, ErrorOperation::Deserialize});
        }

        const auto length = encoded_length(buffer, offset);
        if (length < detail::format::header::size || length > buffer.size() - offset) {
            return std::unexpected(Error{ErrorCode::InvalidData, ErrorOperation::Deserialize});
        }

        auto received = detail::receive_record(
            std::as_bytes(std::span(buffer).subspan(offset, static_cast<std::size_t>(length))),
            Derived::schema());
        if (!received) {
            return std::unexpected(received.error());
        }

        auto replaced = detail::replace(binding_, *received);
        if (!replaced) {
            return std::unexpected(replaced.error());
        }

        // The existing binding commits first; callers can retry a failed decode
        // with the original cursor and destination contents intact.
        offset += static_cast<std::size_t>(length);
        return {};
    }

protected:
    explicit GeneratedMessage(detail::Binding bound) : binding_(std::move(bound)) {
    }

    static detail::Binding copy_binding(const Derived& other) {
        return detail::copy_local(detail::RecordAccess::binding(other));
    }

    // A nested record belongs to its parent: moving it out creates a local copy.
    // Only a root record may transfer its entire binding to the destination object.
    static detail::Binding move_binding(Derived& other) {
        auto& bound = static_cast<GeneratedMessage&>(other).binding_;
        (void)detail::node_epoch(bound.state, bound.handle);
        if (bound.field) {
            return detail::copy_local(bound);
        }

        return std::exchange(bound, {});
    }

    detail::Binding binding_;

private:
    detail::Binding binding() const {
        (void)detail::node_epoch(binding_.state, binding_.handle);
        return binding_;
    }

    static std::uint64_t encoded_length(const std::vector<std::uint8_t>& buffer, std::size_t offset) {
        std::uint64_t length = 0;
        for (unsigned i = 0; i < detail::format::integer_bytes; ++i) {
            length |= std::uint64_t(buffer[offset + detail::format::header::total_bytes + i]) << (8 * i);
        }

        return length;
    }
};
} // namespace vinc::message

namespace vinc {
// Public message creation always allocates process-local storage.
// Obtain shared storage through Publisher::get_next(), never through this factory.
template <class T, class... Args>
    requires std::is_base_of_v<message::GeneratedMessage<T>, T>
std::unique_ptr<T> make_message(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}
}
