#pragma once

// MessageArray and the relocatable byte-image format used by generated message fields.
// C++ wrappers and ownership metadata stay local; only encoded bytes cross process boundaries.

#include <vinc/message/error.hpp>
#include <vinc/contract.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <functional>
#include <ostream>
#include <string>
#include <utility>

namespace vinc::message {
static_assert(sizeof(void*) == 8, "message storage currently supports 64-bit targets only");

struct Usage {
    std::uint64_t compact_bytes = 0;   // canonical image including headers/alignment
    std::uint64_t allocated_bytes = 0; // construction image including reserved elements
    std::uint64_t limit_bytes = 0;     // 0 = local storage has no publisher limit
};
template <class T> class MessageArray;
template <class T> class MessageArrayView;
class Snapshot;
class MessageString;

namespace detail {
struct State;
struct Image;
class FrameworkAccess; // definition is in the framework-only detail contract

// Index identifies a local descriptor; generation rejects references to removed/reused nodes.
// These handles and Schema pointers are local bookkeeping, not the transmitted image format.
struct Handle {
    std::uint64_t index = 0;
    std::uint64_t generation = 0;
    friend bool operator==(Handle, Handle) = default;
};
struct Schema;

// depth 0 = record, depth 1 = array of scalars/records, depth > 1 = nested array.
struct Type {
    std::uint32_t scalar = 0;
    std::uint32_t depth = 1;
    const Schema* schema = nullptr;
    std::uint64_t hash = 0;
};

struct Field {
    std::string name;
    Type type;
    bool singular = false;
};

struct Schema {
    std::uint64_t hash;
    std::vector<Field> fields;
};

struct Binding {
    std::shared_ptr<State> state;
    Handle handle;
    bool field = false;
};

struct BindTag {};
class RecordAccess;

struct Info {
    std::uint64_t size = 0;
    std::uint64_t capacity = 0;
};

template <class T> struct IsArray : std::false_type {};

template <class T> struct IsArray<MessageArray<T>> : std::true_type {
    using Element = T;
};
template <class T> inline constexpr bool is_array = IsArray<T>::value;
template <class T>
concept Record = requires {
    { T::schema() } -> std::same_as<const Schema&>;
};
template <class T>
inline constexpr bool scalar =
    std::is_same_v<T, bool> || std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::int8_t> ||
    std::is_same_v<T, std::uint16_t> || std::is_same_v<T, std::int16_t> || std::is_same_v<T, std::uint32_t> ||
    std::is_same_v<T, std::int32_t> || std::is_same_v<T, std::uint64_t> || std::is_same_v<T, std::int64_t> ||
    std::is_same_v<T, float> || std::is_same_v<T, double>;

// Select arena-backed elements for scalars, generated records and nested arrays.
// Other C++ types use the local vector specialization below; Message Bus accepts generated messages only.
template <class T> consteval bool transferable() {
    if constexpr (is_array<T>) {
        return transferable<typename IsArray<T>::Element>();
    } else {
        return scalar<T> || Record<T>;
    }
}

template <class T> consteval std::uint32_t scalar_code() {
    if constexpr (std::is_same_v<T, std::uint8_t>) {
        return 1;
    } else if constexpr (std::is_same_v<T, std::int8_t>) {
        return 2;
    } else if constexpr (std::is_same_v<T, std::uint16_t>) {
        return 3;
    } else if constexpr (std::is_same_v<T, std::int16_t>) {
        return 4;
    } else if constexpr (std::is_same_v<T, std::uint32_t>) {
        return 5;
    } else if constexpr (std::is_same_v<T, std::int32_t>) {
        return 6;
    } else if constexpr (std::is_same_v<T, std::uint64_t>) {
        return 7;
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        return 8;
    } else if constexpr (std::is_same_v<T, float>) {
        static_assert(sizeof(T) == 4 && std::numeric_limits<T>::is_iec559);
        return 9;
    } else if constexpr (std::is_same_v<T, double>) {
        static_assert(sizeof(T) == 8 && std::numeric_limits<T>::is_iec559);
        return 10;
    } else if constexpr (std::is_same_v<T, bool>) {
        return 11;
    } else {
        static_assert(!sizeof(T), "message arrays accept fixed-width scalars or nested MessageArray<T>");
    }
}

template <class T> Type type() {
    if constexpr (is_array<T>) {
        auto inner = type<typename IsArray<T>::Element>();
        ++inner.depth;
        return inner;
    } else if constexpr (Record<T>) {
        return {0, 1, &T::schema(), T::schema().hash};
    } else {
        return {scalar_code<T>(), 1};
    }
}

template <Record T> Type record_type() {
    return {0, 0, &T::schema(), T::schema().hash};
}

// Canonical scalar bytes use little endian. bool is one byte containing 0 or 1,
// so no std::vector<bool> bit proxy or native bool representation enters the image.
template <class T> auto encode(T value) {
    if constexpr (std::is_same_v<T, bool>) {
        return std::array<std::byte, 1>{value ? std::byte{1} : std::byte{0}};
    } else {
        static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big);
        auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
        if constexpr (std::endian::native == std::endian::big) {
            std::reverse(bytes.begin(), bytes.end());
        }

        return bytes;
    }
}

template <class T> T decode(std::span<const std::byte> bytes) {
    if constexpr (std::is_same_v<T, bool>) {
        return bytes[0] != std::byte{0};
    } else {
        std::array<std::byte, sizeof(T)> value;
        std::copy_n(bytes.begin(), sizeof(T), value.begin());
        if constexpr (std::endian::native == std::endian::big) {
            std::reverse(value.begin(), value.end());
        }

        return std::bit_cast<T>(value);
    }
}

template <class T> T contract_value(Result<T> result) {
    if (!result) {
        vinc::contract_violation("invalid message access");
    }

    return std::move(*result);
}

inline void contract_value(Result<void> result) {
    if (!result) {
        vinc::contract_violation("invalid message access");
    }
}

std::shared_ptr<State> local_state();
void release_message(const std::shared_ptr<State>& state) noexcept;
Result<Handle> add_root(const std::shared_ptr<State>& state, Type type);
Result<Handle> root(const std::shared_ptr<State>& state, std::uint64_t index, Type type);
Result<Info> info(const std::shared_ptr<State>& state, Handle handle);
Result<Handle> child(const std::shared_ptr<State>& state, Handle handle, std::uint64_t index);
Result<std::array<std::byte, 8>> read(const std::shared_ptr<State>& state, Handle handle,
                                      std::uint64_t index);
Result<void> write(const std::shared_ptr<State>& state, Handle handle, std::uint64_t index,
                   std::span<const std::byte> bytes);
Result<void> resize(const std::shared_ptr<State>& state, Handle handle, std::uint64_t size,
                    bool reserve_only);
Result<void> append(const std::shared_ptr<State>& state, Handle handle, std::span<const std::byte> bytes);
Result<void> append_child(const std::shared_ptr<State>& state, Handle handle,
                          const std::shared_ptr<State>& source_state, Handle source_handle);
Result<void> assign(const std::shared_ptr<State>& state, Handle handle, std::span<const std::byte> bytes,
                    std::uint64_t size);
Result<void> preflight_assign(const std::shared_ptr<State>& state, Handle handle, std::uint64_t size);
Result<void> shrink(const std::shared_ptr<State>& state, Handle handle);
Result<void> compact(const std::shared_ptr<State>& state);
Result<Binding> clone(Binding binding);
Binding copy_local(Binding binding);
Binding construct_record(const Schema& schema);
Handle construct_root(const std::shared_ptr<State>& state, Type type);
Result<void> replace(Binding target, Binding source);
Result<void> assign_children(Binding binding, std::span<const Binding> sources);
Result<Binding> create_record(const Schema& schema);
Result<Binding> receive_record(std::span<const std::byte> bytes, const Schema& schema);
// Framework receive path: adopt the already copied image without a second copy.
Result<Binding> adopt_record(std::vector<std::byte> bytes, const Schema& schema);
Binding field(Binding binding, std::uint64_t index);
Result<Usage> usage(const std::shared_ptr<State>& state);
std::uint64_t epoch(const std::shared_ptr<State>& state);
void check_epoch(const std::shared_ptr<State>& state, std::uint64_t expected);
std::uint64_t node_epoch(const std::shared_ptr<State>& state, Handle handle);
void check_node_epoch(const std::shared_ptr<State>& state, Handle handle, std::uint64_t expected);
Result<Snapshot> snapshot(const std::shared_ptr<State>& state, std::uint64_t max_bytes);
Result<Snapshot> seal_binding(Binding binding);
Result<Snapshot> publish_binding(Binding binding, std::span<std::byte> destination,
                                 std::shared_ptr<void> owner);
Result<std::uint64_t> image_root(const std::shared_ptr<const Image>& image, std::uint64_t index, Type type);
std::uint64_t image_size(const std::shared_ptr<const Image>& image, std::uint64_t id);
std::uint64_t image_capacity(const std::shared_ptr<const Image>& image, std::uint64_t id);
std::uint64_t image_child(const std::shared_ptr<const Image>& image, std::uint64_t id, std::uint64_t index);
std::array<std::byte, 8> image_read(const std::shared_ptr<const Image>& image, std::uint64_t id,
                                    std::uint64_t index);
Result<Binding> copy_image_record(const std::shared_ptr<const Image>& image, std::uint64_t id, Type type);

struct Access {
    std::shared_ptr<State> state;
    Handle handle{};
    std::shared_ptr<const Image> image;
    std::uint64_t id = 0;
};

void validate_access(const Access& access);
std::uint64_t access_size(const Access& access);
Access access_child(const Access& access, std::uint64_t index);
std::array<std::byte, 8> access_read(const Access& access, std::uint64_t index);
std::uint64_t access_revision(const Access& access);
void check_revision(const Access& access, std::uint64_t revision);

template <class T, bool ReadOnly> auto make_element(Access access);
template <class T> Result<T> copy_element(const Access& access);
template <class T> Result<Binding> source_binding(const T& value);

class RecordAccess {
public:
    template <class T> static T bind(Binding binding) {
        return T(BindTag{}, std::move(binding));
    }

    template <class T> static Binding binding(const T& value) {
        return value.binding();
    }

    template <class T> static Access access(const T& value) {
        return value.access_;
    }
};
}

// Copy construction detaches into local storage. assign() preserves the
// destination binding, including a parent message's fixed capacity budget.
template <class T> class MessageArray {
    template <class> friend class MessageArray;
    friend class Message;
    friend class detail::RecordAccess;

public:
    MessageArray()
        : state_(detail::local_state()), handle_(detail::construct_root(state_, detail::type<T>())) {
    }

    MessageArray(const MessageArray& other)
        : MessageArray(detail::BindTag{}, detail::copy_local(other.binding())) {
        field_ = false;
    }

    MessageArray(MessageArray&& other) : MessageArray(detail::BindTag{}, take_binding(other)) {
    }

    MessageArray& operator=(const MessageArray&) = delete;
    MessageArray& operator=(MessageArray&&) = delete;

    [[nodiscard]] Result<void> assign(const MessageArray& other) {
        (void)detail::node_epoch(state_, handle_);
        (void)detail::node_epoch(other.state_, other.handle_);
        return detail::replace(binding(), other.binding());
    }

    template <class P>
        requires requires { typename P::ProxyTag; }
    [[nodiscard]] Result<void> assign(const P& other) {
        static_assert(std::same_as<typename P::ValueType, MessageArray<T>>);
        (void)detail::node_epoch(state_, handle_);
        auto source = detail::source_binding(other);
        if (!source) {
            return std::unexpected(source.error());
        }

        return detail::replace(binding(), *source);
    }

    std::uint64_t size() const {
        return detail::contract_value(detail::info(state_, handle_)).size;
    }

    std::uint64_t capacity() const {
        return detail::contract_value(detail::info(state_, handle_)).capacity;
    }

    bool empty() const {
        return size() == 0;
    }

    [[nodiscard]] Result<void> resize(std::uint64_t size) {
        (void)detail::node_epoch(state_, handle_);
        return detail::resize(state_, handle_, size, false);
    }

    [[nodiscard]] Result<void> reserve(std::uint64_t capacity) {
        (void)detail::node_epoch(state_, handle_);
        return detail::resize(state_, handle_, capacity, true);
    }

    [[nodiscard]] Result<void> clear() {
        return resize(0);
    }

    [[nodiscard]] Result<void> shrink_to_fit() {
        (void)detail::node_epoch(state_, handle_);
        return detail::shrink(state_, handle_);
    }

    [[nodiscard]] Result<void> pop_back() {
        (void)detail::node_epoch(state_, handle_);
        auto current = detail::info(state_, handle_);
        if (!current) {
            return std::unexpected(current.error());
        }

        if (!current->size) {
            return std::unexpected(Error{ErrorCode::OutOfRange, ErrorOperation::PopBack});
        }

        return resize(current->size - 1);
    }

    [[nodiscard]] Result<void> push_back(const T& value) {
        (void)detail::node_epoch(state_, handle_);
        if constexpr (!detail::scalar<T>) {
            auto from = detail::RecordAccess::binding(value);
            return detail::append_child(state_, handle_, from.state, from.handle);
        } else {
            const auto bytes = detail::encode(value);
            return detail::append(state_, handle_, bytes);
        }
    }

    template <class P>
        requires requires { typename P::ProxyTag; }
    [[nodiscard]] Result<void> push_back(const P& value) {
        (void)detail::node_epoch(state_, handle_);
        static_assert(std::same_as<typename P::ValueType, T>);
        auto from = detail::source_binding(value);
        if (!from) {
            return std::unexpected(from.error());
        }

        return detail::append_child(state_, handle_, from->state, from->handle);
    }

    template <class... Args> [[nodiscard]] Result<void> emplace_back(Args&&... args) {
        return push_back(T(std::forward<Args>(args)...));
    }

    [[nodiscard]] Result<void> set(std::uint64_t index, T value)
        requires(detail::scalar<T>)
    {
        (void)detail::node_epoch(state_, handle_);
        const auto bytes = detail::encode(value);
        return detail::write(state_, handle_, index, bytes);
    }

    [[nodiscard]] Result<void> set(std::uint64_t index, const T& value)
        requires(!detail::scalar<T>)
    {
        (void)detail::node_epoch(state_, handle_);
        auto child_handle = detail::child(state_, handle_, index);
        if (!child_handle) {
            return std::unexpected(child_handle.error());
        }

        return detail::replace({state_, *child_handle, true}, detail::RecordAccess::binding(value));
    }

    template <class P>
        requires requires { typename P::ProxyTag; }
    [[nodiscard]] Result<void> set(std::uint64_t index, const P& value) {
        static_assert(std::same_as<typename P::ValueType, T>);
        (void)detail::node_epoch(state_, handle_);
        auto target = detail::child(state_, handle_, index);
        if (!target) {
            return std::unexpected(target.error());
        }

        auto source = detail::source_binding(value);
        if (!source) {
            return std::unexpected(source.error());
        }

        return detail::replace({state_, *target, true}, *source);
    }

    // at() returns a value: nested records/arrays are cloned into local storage.
    // Use auto proxy = array[index] to borrow a nested element without allocating.
    [[nodiscard]] Result<T> at(std::uint64_t index) const {
        (void)detail::node_epoch(state_, handle_);
        if constexpr (!detail::scalar<T>) {
            auto result = detail::child(state_, handle_, index);
            if (!result) {
                return std::unexpected(result.error());
            }

            auto copy = detail::clone({state_, *result, true});
            if (!copy) {
                return std::unexpected(copy.error());
            }

            try {
                return detail::RecordAccess::bind<T>(std::move(*copy));
            } catch (const std::bad_alloc&) {
                return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Bind});
            }
        } else {
            auto result = detail::read(state_, handle_, index);
            if (!result) {
                return std::unexpected(result.error());
            }

            return detail::decode<T>(*result);
        }
    }

    // Scalar indexing uses a checked proxy, not a raw reference into a movable arena.
    // Its saved revision detects structural edits that invalidate the old element access.
    class Reference {
    public:
        operator T() const {
            detail::check_node_epoch(state_, handle_, epoch_);
            return detail::contract_value(MessageArray(state_, handle_).at(index_));
        }

        Reference& operator=(T value)
            requires(detail::scalar<T>)
        {
            detail::check_node_epoch(state_, handle_, epoch_);
            detail::contract_value(MessageArray(state_, handle_).set(index_, value));
            return *this;
        }

        Reference& operator=(const Reference& value)
            requires(detail::scalar<T>)
        {
            return *this = static_cast<T>(value);
        }

    private:
        friend class MessageArray;

        Reference(const MessageArray& array, std::uint64_t index)
            : state_(array.state_), handle_(array.handle_), index_(index),
              epoch_(detail::node_epoch(state_, handle_)) {
        }

        std::shared_ptr<detail::State> state_;
        detail::Handle handle_;
        std::uint64_t index_;
        std::uint64_t epoch_;
    };

    auto operator[](std::uint64_t index) {
        if constexpr (detail::scalar<T>) {
            (void)detail::contract_value(at(index));
            return Reference(*this, index);
        } else {
            return detail::make_element<T, false>(detail::access_child({state_, handle_}, index));
        }
    }

    auto operator[](std::uint64_t index) const {
        if constexpr (detail::scalar<T>) {
            return detail::contract_value(at(index));
        } else {
            return detail::make_element<T, true>(detail::access_child({state_, handle_}, index));
        }
    }

    // Iterators borrow their MessageArray wrapper; it must outlive iteration.
    // Structural changes to that array invalidate the saved revision.
    template <bool Constant> class BasicIterator {
    public:
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;
        BasicIterator() = default;

        decltype(auto) operator*() const {
            detail::check_node_epoch(state_, handle_, epoch_);
            return (*owner_)[index_];
        }

        BasicIterator& operator++() {
            detail::check_node_epoch(state_, handle_, epoch_);
            ++index_;
            return *this;
        }

        BasicIterator operator++(int) {
            auto old = *this;
            ++*this;
            return old;
        }

        friend bool operator==(const BasicIterator& left, const BasicIterator& right) {
            return left.owner_ == right.owner_ && left.index_ == right.index_;
        }

    private:
        friend class MessageArray;
        using Owner = std::conditional_t<Constant, const MessageArray, MessageArray>;

        BasicIterator(Owner& array, std::uint64_t index)
            : owner_(&array), state_(array.state_), handle_(array.handle_), index_(index),
              epoch_(detail::node_epoch(state_, handle_)) {
        }

        Owner* owner_ = nullptr;
        std::shared_ptr<detail::State> state_;
        detail::Handle handle_{};
        std::uint64_t index_ = 0;
        std::uint64_t epoch_ = 0;
    };

    using Iterator = BasicIterator<false>;
    using ConstIterator = BasicIterator<true>;

    Iterator begin() {
        (void)size();
        return Iterator(*this, 0);
    }

    Iterator end() {
        return Iterator(*this, size());
    }

    ConstIterator begin() const {
        (void)size();
        return ConstIterator(*this, 0);
    }

    ConstIterator end() const {
        return ConstIterator(*this, size());
    }

    // Import element by element through canonical encoding, including vector<bool>.
    // The destination message budget is checked before allocating the temporary encoded buffer.
    [[nodiscard]] Result<void> copy_from(const std::vector<T>& values)
        requires(detail::scalar<T>)
    {
        (void)detail::node_epoch(state_, handle_);
        constexpr auto stride = sizeof(detail::encode(T{}));
        if (values.size() > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) / stride) {
            return std::unexpected(Error{ErrorCode::SizeOverflow, ErrorOperation::Import});
        }

        auto ready = detail::preflight_assign(state_, handle_, values.size());
        if (!ready) {
            return ready;
        }

        try {
            std::vector<std::byte> bytes(values.size() * stride);
            std::size_t offset = 0;
            for (T value : values) {
                const auto encoded = detail::encode(value);
                std::copy(encoded.begin(), encoded.end(),
                          bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                offset += encoded.size();
            }

            return detail::assign(state_, handle_, bytes, values.size());
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Import});
        } catch (const std::length_error&) {
            return std::unexpected(Error{ErrorCode::SizeOverflow, ErrorOperation::Import});
        }
    }

    [[nodiscard]] Result<std::vector<T>> to_vector() const
        requires(detail::scalar<T>)
    {
        try {
            (void)detail::node_epoch(state_, handle_);
            auto current = detail::info(state_, handle_);
            if (!current) {
                return std::unexpected(current.error());
            }

            std::vector<T> values;
            values.reserve(static_cast<std::size_t>(current->size));
            for (std::uint64_t i = 0; i < current->size; ++i) {
                auto value = at(i);
                if (!value) {
                    return std::unexpected(value.error());
                }

                values.push_back(std::move(*value));
            }

            return values;
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Export});
        } catch (const std::length_error&) {
            return std::unexpected(Error{ErrorCode::SizeOverflow, ErrorOperation::Export});
        }
    }

    [[nodiscard]] Result<void> copy_from(const std::vector<T>& values)
        requires(!detail::scalar<T>)
    {
        try {
            (void)detail::node_epoch(state_, handle_);
            std::vector<detail::Binding> sources;
            sources.reserve(values.size());
            for (const auto& value : values) {
                sources.push_back(detail::RecordAccess::binding(value));
            }

            return detail::assign_children(binding(), sources);
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Import});
        } catch (const std::length_error&) {
            return std::unexpected(Error{ErrorCode::SizeOverflow, ErrorOperation::Import});
        }
    }

    [[nodiscard]] Result<std::vector<T>> to_vector() const
        requires(!detail::scalar<T>)
    {
        try {
            (void)detail::node_epoch(state_, handle_);
            auto current = detail::info(state_, handle_);
            if (!current) {
                return std::unexpected(current.error());
            }

            std::vector<T> result;
            result.reserve(static_cast<std::size_t>(current->size));
            for (std::uint64_t i = 0; i < current->size; ++i) {
                auto value = at(i);
                if (!value) {
                    return std::unexpected(value.error());
                }

                result.push_back(std::move(*value));
            }

            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Export});
        } catch (const std::length_error&) {
            return std::unexpected(Error{ErrorCode::SizeOverflow, ErrorOperation::Export});
        }
    }

    [[nodiscard]] Result<Snapshot> snapshot(std::uint64_t max_bytes = UINT64_MAX) const;

private:
    MessageArray(std::shared_ptr<detail::State> state, detail::Handle handle)
        : state_(std::move(state)), handle_(handle) {
    }

    MessageArray(detail::BindTag, detail::Binding bound)
        : state_(std::move(bound.state)), handle_(bound.handle), field_(bound.field) {
    }

    detail::Binding binding() const {
        (void)detail::node_epoch(state_, handle_);
        return {state_, handle_, field_};
    }

    // Moving a field must not steal storage from its parent message. Detached arrays
    // can transfer ownership; a bound field is copied into a separate local arena instead.
    static detail::Binding take_binding(MessageArray& other) {
        if (other.field_) {
            return detail::copy_local(other.binding());
        }

        return {std::exchange(other.state_, {}), other.handle_, false};
    }

    std::shared_ptr<detail::State> state_;
    detail::Handle handle_;
    bool field_ = false;
};

// Arbitrary C++ objects are valid local elements, but acquire no wire eligibility.
template <class T>
    requires(!detail::transferable<T>())
class MessageArray<T> {
public:
    using iterator = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;
    MessageArray() = default;
    MessageArray(const MessageArray&) = default;
    MessageArray(MessageArray&&) = default;
    MessageArray& operator=(const MessageArray&) = delete;
    MessageArray& operator=(MessageArray&&) = delete;

    [[nodiscard]] Result<void> assign(const MessageArray& other) {
        return attempt(ErrorOperation::Assign, [&] {
            values_ = other.values_;
        });
    }

    [[nodiscard]] Result<void> assign(MessageArray&& other) {
        if (this == &other) {
            return {};
        }

        return attempt(ErrorOperation::Assign, [&] {
            values_ = std::move(other.values_);
        });
    }

    std::size_t size() const noexcept {
        return values_.size();
    }

    std::size_t capacity() const noexcept {
        return values_.capacity();
    }

    bool empty() const noexcept {
        return values_.empty();
    }

    T& operator[](std::size_t i) {
        if (i >= values_.size()) {
            vinc::contract_index_violation("array index out of range", i, values_.size());
        }

        return values_[i];
    }

    const T& operator[](std::size_t i) const {
        if (i >= values_.size()) {
            vinc::contract_index_violation("array index out of range", i, values_.size());
        }

        return values_[i];
    }

    [[nodiscard]] Result<T> at(std::size_t i) const
        requires std::copy_constructible<T>
    {
        if (i >= values_.size()) {
            return std::unexpected(Error{ErrorCode::OutOfRange, ErrorOperation::Read});
        }

        try {
            return values_[i];
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Copy});
        } catch (const std::length_error&) {
            return std::unexpected(Error{ErrorCode::SizeOverflow, ErrorOperation::Copy});
        }
    }

    [[nodiscard]] Result<void> set(std::size_t i, const T& value) {
        if (i >= values_.size()) {
            return std::unexpected(Error{ErrorCode::OutOfRange, ErrorOperation::Write});
        }

        return attempt(ErrorOperation::Write, [&] {
            values_[i] = value;
        });
    }

    [[nodiscard]] Result<void> set(std::size_t i, T&& value) {
        if (i >= values_.size()) {
            return std::unexpected(Error{ErrorCode::OutOfRange, ErrorOperation::Write});
        }

        return attempt(ErrorOperation::Write, [&] {
            values_[i] = std::move(value);
        });
    }

    iterator begin() noexcept {
        return values_.begin();
    }

    iterator end() noexcept {
        return values_.end();
    }

    const_iterator begin() const noexcept {
        return values_.begin();
    }

    const_iterator end() const noexcept {
        return values_.end();
    }

    Result<void> resize(std::size_t size) {
        return attempt(ErrorOperation::Resize, [&] {
            values_.resize(size);
        });
    }

    Result<void> reserve(std::size_t capacity) {
        return attempt(ErrorOperation::Reserve, [&] {
            values_.reserve(capacity);
        });
    }

    Result<void> clear() {
        values_.clear();
        return {};
    }

    Result<void> shrink_to_fit() {
        return attempt(ErrorOperation::Shrink, [&] {
            values_.shrink_to_fit();
        });
    }

    Result<void> pop_back() {
        if (empty()) {
            return std::unexpected(Error{ErrorCode::OutOfRange, ErrorOperation::PopBack});
        }

        values_.pop_back();
        return {};
    }

    Result<void> push_back(const T& v) {
        return attempt(ErrorOperation::Append, [&] {
            values_.push_back(v);
        });
    }

    Result<void> push_back(T&& v) {
        return attempt(ErrorOperation::Append, [&] {
            values_.push_back(std::move(v));
        });
    }

    template <class... Args> Result<void> emplace_back(Args&&... args) {
        return attempt(ErrorOperation::Append, [&] {
            values_.emplace_back(std::forward<Args>(args)...);
        });
    }

private:
    template <class F> static Result<void> attempt(ErrorOperation action, F&& f) {
        try {
            f();
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, action});
        } catch (const std::length_error&) {
            return std::unexpected(Error{ErrorCode::SizeOverflow, action});
        }
    }

    std::vector<T> values_;
};

// A scalar reference stores a descriptor identity, not a pointer into movable bytes.
template <class T, bool ReadOnly> class ScalarRef {
public:
    explicit ScalarRef(detail::Access access, std::uint64_t index = 0)
        : access_(std::move(access)), index_(index), revision_(detail::access_revision(access_)) {
    }

    ScalarRef(const ScalarRef&) = default;
    ScalarRef(ScalarRef&&) = default;

    ScalarRef& operator=(const ScalarRef& other)
        requires(!ReadOnly)
    {
        return *this = other.get();
    }

    ScalarRef& operator=(ScalarRef&& other)
        requires(!ReadOnly)
    {
        return *this = other.get();
    }

    ScalarRef& operator=(T value)
        requires(!ReadOnly)
    {
        detail::check_revision(access_, revision_);
        detail::contract_value(detail::write(access_.state, access_.handle, index_, detail::encode(value)));
        return *this;
    }

    T get() const {
        detail::check_revision(access_, revision_);
        return detail::decode<T>(detail::access_read(access_, index_));
    }

    operator T() const {
        return get();
    }

    ScalarRef& operator+=(T value)
        requires(!ReadOnly)
    {
        return *this = static_cast<T>(get() + value);
    }

    ScalarRef& operator-=(T value)
        requires(!ReadOnly)
    {
        return *this = static_cast<T>(get() - value);
    }

    friend std::ostream& operator<<(std::ostream& out, const ScalarRef& value) {
        if constexpr (sizeof(T) == 1 && !std::is_same_v<T, bool>) {
            return out << +value.get();
        } else {
            return out << value.get();
        }
    }

private:
    detail::Access access_;
    std::uint64_t index_;
    std::uint64_t revision_;
};

template <class T, bool ReadOnly> class RecordRef;
template <bool ReadOnly> class StringRef;

// Proxy copies retain the same node. Only at()/copy() create independent values.
// No element cache, schema initialization, allocation or payload copy occurs on indexing.
template <class T, bool ReadOnly> class ArrayRef {
    friend class detail::RecordAccess;

public:
    using ProxyTag = void;
    using ValueType = MessageArray<T>;

    explicit ArrayRef(detail::Access access) : access_(std::move(access)) {
    }

    ArrayRef(const ArrayRef&) = default;
    ArrayRef(ArrayRef&&) = default;
    ArrayRef& operator=(const ArrayRef&) = delete;
    ArrayRef& operator=(ArrayRef&&) = delete;

    std::uint64_t size() const {
        return detail::access_size(access_);
    }

    bool empty() const {
        return size() == 0;
    }

    std::uint64_t capacity() const {
        detail::validate_access(access_);
        if (access_.image) {
            return detail::image_capacity(access_.image, access_.id);
        }

        return detail::contract_value(detail::info(access_.state, access_.handle)).capacity;
    }

    auto operator[](std::uint64_t index) {
        return index_as<ReadOnly>(index);
    }

    auto operator[](std::uint64_t index) const {
        return index_as<true>(index);
    }

    [[nodiscard]] Result<T> at(std::uint64_t index) const {
        if (index >= size()) {
            return std::unexpected(Error{ErrorCode::OutOfRange, ErrorOperation::Read});
        }

        if constexpr (detail::scalar<T>) {
            return detail::decode<T>(detail::access_read(access_, index));
        } else {
            return detail::copy_element<T>(detail::access_child(access_, index));
        }
    }

    [[nodiscard]] Result<MessageArray<T>> copy() const {
        return detail::copy_element<MessageArray<T>>(access_);
    }

    [[nodiscard]] Result<void> set(std::uint64_t index, const T& value)
        requires(!ReadOnly)
    {
        return mutable_array().set(index, value);
    }

    template <class P>
        requires(!ReadOnly && requires { typename P::ProxyTag; })
    [[nodiscard]] Result<void> set(std::uint64_t index, const P& value) {
        return mutable_array().set(index, value);
    }

    [[nodiscard]] Result<void> resize(std::uint64_t size)
        requires(!ReadOnly)
    {
        return mutable_array().resize(size);
    }

    [[nodiscard]] Result<void> reserve(std::uint64_t size)
        requires(!ReadOnly)
    {
        return mutable_array().reserve(size);
    }

    [[nodiscard]] Result<void> clear()
        requires(!ReadOnly)
    {
        return mutable_array().clear();
    }

    [[nodiscard]] Result<void> shrink_to_fit()
        requires(!ReadOnly)
    {
        return mutable_array().shrink_to_fit();
    }

    [[nodiscard]] Result<void> pop_back()
        requires(!ReadOnly)
    {
        return mutable_array().pop_back();
    }

    [[nodiscard]] Result<void> push_back(const T& value)
        requires(!ReadOnly)
    {
        return mutable_array().push_back(value);
    }

    template <class P>
        requires(!ReadOnly && requires { typename P::ProxyTag; })
    [[nodiscard]] Result<void> push_back(const P& value) {
        return mutable_array().push_back(value);
    }

    template <class U>
    [[nodiscard]] Result<void> assign(const U& other)
        requires(!ReadOnly)
    {
        detail::validate_access(access_);
        auto source = detail::source_binding(other);
        if (!source) {
            return std::unexpected(source.error());
        }

        return detail::replace(binding(), *source);
    }

    template <class... Args>
    [[nodiscard]] Result<void> emplace_back(Args&&... args)
        requires(!ReadOnly)
    {
        return mutable_array().emplace_back(std::forward<Args>(args)...);
    }

    [[nodiscard]] Result<void> copy_from(const std::vector<T>& values)
        requires(!ReadOnly)
    {
        return mutable_array().copy_from(values);
    }

    [[nodiscard]] Result<std::vector<T>> to_vector() const {
        const auto count = size();
        try {
            std::vector<T> values;
            values.reserve(static_cast<std::size_t>(count));
            for (std::uint64_t i = 0; i < count; ++i) {
                auto value = at(i);
                if (!value) {
                    return std::unexpected(value.error());
                }

                values.push_back(std::move(*value));
            }
            return values;
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Export});
        } catch (const std::length_error&) {
            return std::unexpected(Error{ErrorCode::SizeOverflow, ErrorOperation::Export});
        }
    }

    template <bool Constant> class Iterator {
    public:
        Iterator(detail::Access access, std::uint64_t index)
            : access_(std::move(access)), index_(index), revision_(detail::access_revision(access_)) {
        }

        auto operator*() const {
            detail::check_revision(access_, revision_);
            return ArrayRef<T, Constant>(access_)[index_];
        }

        Iterator& operator++() {
            detail::check_revision(access_, revision_);
            ++index_;
            return *this;
        }

        friend bool operator==(const Iterator& a, const Iterator& b) {
            return a.access_.state == b.access_.state && a.access_.handle == b.access_.handle &&
                   a.access_.image == b.access_.image && a.access_.id == b.access_.id && a.index_ == b.index_;
        }

    private:
        detail::Access access_;
        std::uint64_t index_;
        std::uint64_t revision_;
    };

    auto begin() {
        return Iterator<ReadOnly>(access_, 0);
    }

    auto end() {
        return Iterator<ReadOnly>(access_, size());
    }

    auto begin() const {
        return Iterator<true>(access_, 0);
    }

    auto end() const {
        return Iterator<true>(access_, size());
    }

private:
    template <bool Constant> auto index_as(std::uint64_t index) const {
        if (index >= size()) {
            vinc::contract_index_violation("array index out of range", index, size());
        }

        if constexpr (detail::scalar<T>) {
            if constexpr (Constant) {
                return detail::decode<T>(detail::access_read(access_, index));
            } else {
                return ScalarRef<T, false>(access_, index);
            }
        } else {
            return detail::make_element<T, Constant>(detail::access_child(access_, index));
        }
    }

    MessageArray<T> mutable_array() const
        requires(!ReadOnly)
    {
        return detail::RecordAccess::bind<MessageArray<T>>(binding());
    }

    detail::Binding binding() const {
        detail::validate_access(access_);
        if (!access_.state) {
            vinc::contract_violation("immutable image has no mutable binding");
        }

        return {access_.state, access_.handle, true};
    }

    detail::Access access_;
};

template <class T> class MessageArrayView : public ArrayRef<T, true> {
    friend class Snapshot;

    MessageArrayView(std::shared_ptr<const detail::Image> image, std::uint64_t id)
        : ArrayRef<T, true>({{}, {}, std::move(image), id}) {
    }
};

namespace detail {
template <class T, bool ReadOnly> auto make_element(Access access) {
    if constexpr (is_array<T>) {
        if constexpr (requires { typename T::StringTag; }) {
            return StringRef<ReadOnly>(std::move(access));
        } else {
            return ArrayRef<typename IsArray<T>::Element, ReadOnly>(std::move(access));
        }
    } else {
        return RecordRef<T, ReadOnly>(std::move(access));
    }
}

template <class T> Result<Binding> source_binding(const T& value) {
    if constexpr (requires { typename T::ProxyTag; }) {
        auto access = RecordAccess::access(value);
        validate_access(access);
        if (access.image) {
            using Value = typename T::ValueType;
            Type expected;
            if constexpr (is_array<Value>) {
                expected = type<typename IsArray<Value>::Element>();
            } else {
                expected = record_type<Value>();
            }
            return copy_image_record(access.image, access.id, expected);
        }

        return Binding{access.state, access.handle, true};
    } else {
        return RecordAccess::binding(value);
    }
}

template <class T> Result<T> copy_element(const Access& access) {
    validate_access(access);
    Type expected;
    if constexpr (is_array<T>) {
        expected = type<typename IsArray<T>::Element>();
    } else {
        expected = record_type<T>();
    }
    auto copied = access.image ? copy_image_record(access.image, access.id, expected)
                               : clone({access.state, access.handle, true});
    if (!copied) {
        return std::unexpected(copied.error());
    }

    try {
        return RecordAccess::bind<T>(std::move(*copied));
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Copy});
    }
}
}

// An immutable encoded image plus its owner. A borrowed framework snapshot keeps
// the mapping alive, but Message Bus is still responsible for preventing slot reuse during access.
class Snapshot {
public:
    Snapshot() = default;
    // Always copies into owned process memory, then validates that copy.
    [[nodiscard]] static Result<Snapshot> copy_from(std::span<const std::byte> bytes);
    std::span<const std::byte> bytes() const noexcept;
    std::uint64_t root_count() const noexcept;

    // Validate the selected subtree before exposing typed access. Record schema
    // traversal may allocate; subsequent indexing stays allocation-free. Concurrent
    // calls only read the image and do not install mutable validation metadata.
    template <class T> [[nodiscard]] Result<MessageArrayView<T>> array(std::uint64_t index) const {
        auto id = detail::image_root(image_, index, detail::type<T>());
        if (!id) {
            return std::unexpected(id.error());
        }

        return MessageArrayView<T>(image_, *id);
    }

private:
    friend Result<Snapshot> detail::snapshot(const std::shared_ptr<detail::State>&, std::uint64_t);
    friend class detail::FrameworkAccess;
    friend Result<Snapshot> detail::seal_binding(detail::Binding);
    friend Result<Snapshot> detail::publish_binding(detail::Binding, std::span<std::byte>,
                                                    std::shared_ptr<void>);

    explicit Snapshot(std::shared_ptr<const detail::Image> image) : image_(std::move(image)) {
    }

    std::shared_ptr<const detail::Image> image_;
};

template <class T> Result<Snapshot> MessageArray<T>::snapshot(std::uint64_t max_bytes) const {
    (void)detail::node_epoch(state_, handle_);
    auto valid = detail::info(state_, handle_);
    if (!valid) {
        return std::unexpected(valid.error());
    }

    return detail::snapshot(state_, max_bytes); // snapshots the entire owning message
}

// Low-level local arena for composing root arrays. Generated message classes provide
// the schema-bound application API; this owner is not a Message Bus ring or publisher.
class Message {
public:
    Message(); // local, growable; may throw bad_alloc during construction
    ~Message();
    Message(Message&&) noexcept;
    Message& operator=(Message&&) noexcept;
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    template <class T> [[nodiscard]] Result<MessageArray<T>> add_array() {
        (void)detail::epoch(state_);
        auto handle = detail::add_root(state_, detail::type<T>());
        if (!handle) {
            return std::unexpected(handle.error());
        }

        return MessageArray<T>(state_, *handle);
    }

    template <class T> [[nodiscard]] Result<MessageArray<T>> array(std::uint64_t index) {
        (void)detail::epoch(state_);
        auto handle = detail::root(state_, index, detail::type<T>());
        if (!handle) {
            return std::unexpected(handle.error());
        }

        return MessageArray<T>(state_, *handle);
    }

    [[nodiscard]] Result<Usage> usage() const {
        (void)detail::epoch(state_);
        return detail::usage(state_);
    }

    // Releases all reserved element capacity, including nested arrays.
    [[nodiscard]] Result<void> compact() {
        (void)detail::epoch(state_);
        return detail::compact(state_);
    }

    // Checks packed length, not reserved capacity; never consumes the source.
    [[nodiscard]] Result<Snapshot> snapshot(std::uint64_t max_bytes = UINT64_MAX) const {
        (void)detail::epoch(state_);
        return detail::snapshot(state_, max_bytes);
    }

private:
    friend class detail::FrameworkAccess;

    explicit Message(std::shared_ptr<detail::State> state) : state_(std::move(state)) {
    }

    std::shared_ptr<detail::State> state_;
};
} // namespace vinc::message

namespace vinc {
template <class T> using MessageArray = message::MessageArray<T>;
template <class T> using MessageArrayView = message::MessageArrayView<T>;
}
