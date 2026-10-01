#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <functional>
#include <string>
#include <utility>
#include <unordered_map>

namespace vinc::message {
static_assert(sizeof(void*) == 8, "message storage currently supports 64-bit targets only");
enum class ErrorCode {
    CapacityExceeded, ResourceExhausted, SizeOverflow, OutOfRange,
    StaleHandle, Closed, InvalidData, TypeMismatch, InvalidArgument
};
struct Error { ErrorCode code; };
template<class T> using Result = std::expected<T, Error>;
class AccessError : public std::runtime_error {
public:
    explicit AccessError(Error error) : std::runtime_error("invalid message container access"), error_(error) {}
    Error error() const noexcept { return error_; }
private:
    Error error_;
};
struct Usage {
    std::uint64_t compact_bytes = 0;   // canonical image including headers/alignment
    std::uint64_t allocated_bytes = 0; // construction image including reserved elements
    std::uint64_t limit_bytes = 0;     // 0 = local storage has no publisher limit
};
template<class T> class MessageArray;
template<class T> class MessageArrayView;
class Snapshot;
namespace detail {
struct State;
struct Image;
class FrameworkAccess; // definition is in a framework-private, non-exported header
struct Handle { std::uint64_t index = 0, generation = 0; friend bool operator==(Handle, Handle) = default; };
struct Schema;
// depth 0 = record, depth 1 = array of scalars/records, depth > 1 = nested array.
struct Type {
    std::uint32_t scalar = 0, depth = 1;
    const Schema* schema = nullptr;
    std::uint64_t hash = 0;
};
struct Field { std::string name; Type type; bool singular = false; };
struct Schema { std::uint64_t hash; std::vector<Field> fields; };
struct Binding { std::shared_ptr<State> state; Handle handle; bool field = false; };
struct BindTag {};
class RecordAccess;
struct Info { std::uint64_t size = 0, capacity = 0; };
template<class T> struct IsArray : std::false_type {};
template<class T> struct IsArray<MessageArray<T>> : std::true_type { using Element = T; };
template<class T> inline constexpr bool is_array = IsArray<T>::value;
template<class T> concept Record = requires { { T::schema() } -> std::same_as<const Schema&>; };
template<class T> inline constexpr bool scalar =
    std::is_same_v<T, bool> || std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::int8_t> ||
    std::is_same_v<T, std::uint16_t> || std::is_same_v<T, std::int16_t> ||
    std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::int32_t> ||
    std::is_same_v<T, std::uint64_t> || std::is_same_v<T, std::int64_t> ||
    std::is_same_v<T, float> || std::is_same_v<T, double>;
template<class T> consteval bool transferable() {
    if constexpr (is_array<T>) return transferable<typename IsArray<T>::Element>();
    else return scalar<T> || Record<T>;
}
template<class T> consteval std::uint32_t scalar_code() {
    if constexpr (std::is_same_v<T, std::uint8_t>) return 1;
    else if constexpr (std::is_same_v<T, std::int8_t>) return 2;
    else if constexpr (std::is_same_v<T, std::uint16_t>) return 3;
    else if constexpr (std::is_same_v<T, std::int16_t>) return 4;
    else if constexpr (std::is_same_v<T, std::uint32_t>) return 5;
    else if constexpr (std::is_same_v<T, std::int32_t>) return 6;
    else if constexpr (std::is_same_v<T, std::uint64_t>) return 7;
    else if constexpr (std::is_same_v<T, std::int64_t>) return 8;
    else if constexpr (std::is_same_v<T, float>) { static_assert(sizeof(T) == 4 && std::numeric_limits<T>::is_iec559); return 9; }
    else if constexpr (std::is_same_v<T, double>) { static_assert(sizeof(T) == 8 && std::numeric_limits<T>::is_iec559); return 10; }
    else if constexpr (std::is_same_v<T, bool>) return 11;
    else { static_assert(!sizeof(T), "message arrays accept fixed-width scalars or nested MessageArray<T>"); }
}
template<class T> Type type() {
    if constexpr (is_array<T>) {
        auto inner = type<typename IsArray<T>::Element>();
        ++inner.depth; return inner;
    } else if constexpr (Record<T>) return {0, 1, &T::schema(), T::schema().hash};
    else return {scalar_code<T>(), 1};
}
template<Record T> Type record_type() { return {0, 0, &T::schema(), T::schema().hash}; }
template<class T> auto encode(T value) {
    if constexpr (std::is_same_v<T, bool>) return std::array<std::byte, 1>{value ? std::byte{1} : std::byte{0}};
    else {
        static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big);
        auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
        if constexpr (std::endian::native == std::endian::big) std::reverse(bytes.begin(), bytes.end());
        return bytes;
    }
}
template<class T> T decode(std::span<const std::byte> bytes) {
    if constexpr (std::is_same_v<T, bool>) return bytes[0] != std::byte{0};
    else {
        std::array<std::byte, sizeof(T)> value;
        std::copy_n(bytes.begin(), sizeof(T), value.begin());
        if constexpr (std::endian::native == std::endian::big) std::reverse(value.begin(), value.end());
        return std::bit_cast<T>(value);
    }
}
template<class T> T checked(Result<T> result) {
    if (!result) throw AccessError(result.error());
    return std::move(*result);
}
inline void checked(Result<void> result) { if (!result) throw AccessError(result.error()); }
std::shared_ptr<State> local_state();
void release_message(const std::shared_ptr<State>&) noexcept;
Result<Handle> add_root(const std::shared_ptr<State>&, Type);
Result<Handle> root(const std::shared_ptr<State>&, std::uint64_t, Type);
Result<Info> info(const std::shared_ptr<State>&, Handle);
Result<Handle> child(const std::shared_ptr<State>&, Handle, std::uint64_t);
Result<std::array<std::byte, 8>> read(const std::shared_ptr<State>&, Handle, std::uint64_t);
Result<void> write(const std::shared_ptr<State>&, Handle, std::uint64_t, std::span<const std::byte>);
Result<void> resize(const std::shared_ptr<State>&, Handle, std::uint64_t, bool reserve_only);
Result<void> append(const std::shared_ptr<State>&, Handle, std::span<const std::byte>);
Result<void> append_child(const std::shared_ptr<State>&, Handle, const std::shared_ptr<State>&, Handle);
Result<void> assign(const std::shared_ptr<State>&, Handle, std::span<const std::byte>, std::uint64_t);
Result<void> preflight_assign(const std::shared_ptr<State>&, Handle, std::uint64_t);
Result<void> shrink(const std::shared_ptr<State>&, Handle);
Result<void> compact(const std::shared_ptr<State>&);
Result<Binding> clone(Binding);
Result<void> replace(Binding target, Binding source);
Result<void> assign_children(Binding, std::span<const Binding>);
Result<Binding> create_record(const Schema&);
Result<Binding> receive_record(std::span<const std::byte>, const Schema&);
// Framework receive path: adopt the already copied image without a second copy.
Result<Binding> adopt_record(std::vector<std::byte>, const Schema&);
Binding field(Binding, std::uint64_t);
Result<Usage> usage(const std::shared_ptr<State>&);
std::uint64_t epoch(const std::shared_ptr<State>&);
void check_epoch(const std::shared_ptr<State>&, std::uint64_t);
std::uint64_t node_epoch(const std::shared_ptr<State>&, Handle);
void check_node_epoch(const std::shared_ptr<State>&, Handle, std::uint64_t);
Result<Snapshot> snapshot(const std::shared_ptr<State>&, std::uint64_t max_bytes);
Result<Snapshot> seal_binding(Binding);
Result<Snapshot> publish_binding(Binding, std::span<std::byte>, std::shared_ptr<void>);
Result<std::uint64_t> image_root(const std::shared_ptr<const Image>&, std::uint64_t, Type);
std::uint64_t image_size(const std::shared_ptr<const Image>&, std::uint64_t);
std::uint64_t image_child(const std::shared_ptr<const Image>&, std::uint64_t, std::uint64_t);
std::array<std::byte, 8> image_read(const std::shared_ptr<const Image>&, std::uint64_t, std::uint64_t);
Result<Binding> copy_image_record(const std::shared_ptr<const Image>&, std::uint64_t, Type);
class RecordAccess {
public:
    template<class T> static T bind(Binding binding) { return T(BindTag{}, std::move(binding)); }
    template<class T> static Binding binding(const T& value) { return value.binding(); }
};
}

// Copy construction detaches into local storage. Assignment preserves the
// destination binding, including a parent message's fixed capacity budget.
template<class T> class MessageArray {
    template<class> friend class MessageArray;
    friend class Message;
    friend class detail::RecordAccess;
public:
    MessageArray() : state_(detail::local_state()), handle_(detail::checked(detail::add_root(state_, detail::type<T>()))) {}
    MessageArray(const MessageArray& other) : MessageArray(detail::BindTag{}, detail::checked(detail::clone(other.binding()))) { field_ = false; }
    MessageArray(MessageArray&& other) : MessageArray(detail::BindTag{}, take_binding(other)) {
        if (!other.field_) elements_ = std::move(other.elements_);
    }
    MessageArray& operator=(const MessageArray& other) { detail::checked(assign(other)); return *this; }
    MessageArray& operator=(MessageArray&& other) { return *this = static_cast<const MessageArray&>(other); }
    [[nodiscard]] Result<void> assign(const MessageArray& other) { return detail::replace(binding(), other.binding()); }
    std::uint64_t size() const { return detail::checked(detail::info(state_, handle_)).size; }
    std::uint64_t capacity() const { return detail::checked(detail::info(state_, handle_)).capacity; }
    bool empty() const { return size() == 0; }
    [[nodiscard]] Result<void> resize(std::uint64_t size) { return detail::resize(state_, handle_, size, false); }
    [[nodiscard]] Result<void> reserve(std::uint64_t capacity) { return detail::resize(state_, handle_, capacity, true); }
    [[nodiscard]] Result<void> clear() { return resize(0); }
    [[nodiscard]] Result<void> shrink_to_fit() { return detail::shrink(state_, handle_); }
    [[nodiscard]] Result<void> pop_back() {
        auto current = detail::info(state_, handle_);
        if (!current) return std::unexpected(current.error());
        if (!current->size) return std::unexpected(Error{ErrorCode::OutOfRange});
        return resize(current->size - 1);
    }
    [[nodiscard]] Result<void> push_back(const T& value) {
        if constexpr (!detail::scalar<T>) {
            auto from = detail::RecordAccess::binding(value);
            return detail::append_child(state_, handle_, from.state, from.handle);
        }
        else { const auto bytes = detail::encode(value); return detail::append(state_, handle_, bytes); }
    }
    template<class... Args> [[nodiscard]] Result<void> emplace_back(Args&&... args) { return push_back(T(std::forward<Args>(args)...)); }
    [[nodiscard]] Result<void> set(std::uint64_t index, T value) requires (detail::scalar<T>) {
        const auto bytes = detail::encode(value);
        return detail::write(state_, handle_, index, bytes);
    }
    [[nodiscard]] Result<void> set(std::uint64_t index, const T& value) requires (!detail::scalar<T>) {
        auto h = detail::child(state_, handle_, index);
        if (!h) return std::unexpected(h.error());
        return detail::replace({state_, *h, true}, detail::RecordAccess::binding(value));
    }
    [[nodiscard]] Result<T> at(std::uint64_t index) const {
        if constexpr (!detail::scalar<T>) {
            auto result = detail::child(state_, handle_, index);
            if (!result) return std::unexpected(result.error());
            auto copy = detail::clone({state_, *result, true});
            if (!copy) return std::unexpected(copy.error());
            try { return detail::RecordAccess::bind<T>(std::move(*copy)); }
            catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::ResourceExhausted}); }
            catch (const AccessError& e) { return std::unexpected(e.error()); }
        } else {
            auto result = detail::read(state_, handle_, index);
            if (!result) return std::unexpected(result.error());
            return detail::decode<T>(*result);
        }
    }
    class Reference {
    public:
        operator T() const { detail::check_node_epoch(state_, handle_, epoch_); return detail::checked(MessageArray(state_, handle_).at(index_)); }
        Reference& operator=(T value) requires (detail::scalar<T>) {
            detail::check_node_epoch(state_, handle_, epoch_);
            detail::checked(MessageArray(state_, handle_).set(index_, value)); return *this;
        }
        Reference& operator=(const Reference& value) requires (detail::scalar<T>) { return *this = static_cast<T>(value); }
    private:
        friend class MessageArray;
        Reference(const MessageArray& array, std::uint64_t index) : state_(array.state_), handle_(array.handle_), index_(index), epoch_(detail::node_epoch(state_, handle_)) {}
        std::shared_ptr<detail::State> state_;
        detail::Handle handle_;
        std::uint64_t index_, epoch_;
    };
    decltype(auto) operator[](std::uint64_t index) {
        if constexpr (!detail::scalar<T>) return element(index);
        else { (void)detail::checked(at(index)); return Reference(*this, index); }
    }
    decltype(auto) operator[](std::uint64_t index) const {
        if constexpr (!detail::scalar<T>) return static_cast<const T&>(element(index));
        else return detail::checked(at(index));
    }
    template<bool Constant> class BasicIterator {
    public:
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;
        BasicIterator() = default;
        decltype(auto) operator*() const { detail::check_node_epoch(state_, handle_, epoch_); return (*owner_)[index_]; }
        BasicIterator& operator++() { detail::check_node_epoch(state_, handle_, epoch_); ++index_; return *this; }
        BasicIterator operator++(int) { auto old = *this; ++*this; return old; }
        friend bool operator==(const BasicIterator& a, const BasicIterator& b) { return a.owner_ == b.owner_ && a.index_ == b.index_; }
    private:
        friend class MessageArray;
        using Owner = std::conditional_t<Constant, const MessageArray, MessageArray>;
        BasicIterator(Owner& array, std::uint64_t index) : owner_(&array), state_(array.state_), handle_(array.handle_), index_(index), epoch_(detail::node_epoch(state_, handle_)) {}
        Owner* owner_ = nullptr;
        std::shared_ptr<detail::State> state_;
        detail::Handle handle_{};
        std::uint64_t index_ = 0, epoch_ = 0;
    };
    using Iterator = BasicIterator<false>;
    using ConstIterator = BasicIterator<true>;
    Iterator begin() { (void)size(); return Iterator(*this, 0); }
    Iterator end() { return Iterator(*this, size()); }
    ConstIterator begin() const { (void)size(); return ConstIterator(*this, 0); }
    ConstIterator end() const { return ConstIterator(*this, size()); }
    [[nodiscard]] Result<void> copy_from(const std::vector<T>& values) requires (detail::scalar<T>) {
        constexpr auto stride = sizeof(detail::encode(T{}));
        if (values.size() > static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) / stride)
            return std::unexpected(Error{ErrorCode::SizeOverflow});
        auto ready = detail::preflight_assign(state_, handle_, values.size());
        if (!ready) return ready;
        try {
            std::vector<std::byte> bytes(values.size() * stride);
            std::size_t offset = 0;
            for (T value : values) {
                const auto encoded = detail::encode(value);
                std::copy(encoded.begin(), encoded.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                offset += encoded.size();
            }
            return detail::assign(state_, handle_, bytes, values.size());
        } catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::ResourceExhausted}); }
          catch (const std::length_error&) { return std::unexpected(Error{ErrorCode::SizeOverflow}); }
    }
    [[nodiscard]] Result<std::vector<T>> to_vector() const requires (detail::scalar<T>) {
        try {
            auto current = detail::info(state_, handle_);
            if (!current) return std::unexpected(current.error());
            std::vector<T> values; values.reserve(static_cast<std::size_t>(current->size));
            for (std::uint64_t i = 0; i < current->size; ++i) values.push_back(detail::checked(at(i)));
            return values;
        } catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::ResourceExhausted}); }
          catch (const std::length_error&) { return std::unexpected(Error{ErrorCode::SizeOverflow}); }
    }
    [[nodiscard]] Result<void> copy_from(const std::vector<T>& values) requires (!detail::scalar<T>) {
        try {
            std::vector<detail::Binding> sources; sources.reserve(values.size());
            for (const auto& value : values) sources.push_back(detail::RecordAccess::binding(value));
            return detail::assign_children(binding(), sources);
        } catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::ResourceExhausted}); }
          catch (const std::length_error&) { return std::unexpected(Error{ErrorCode::SizeOverflow}); }
          catch (const AccessError& e) { return std::unexpected(e.error()); }
    }
    [[nodiscard]] Result<std::vector<T>> to_vector() const requires (!detail::scalar<T>) {
        try {
            std::vector<T> result; result.reserve(static_cast<std::size_t>(size()));
            for (std::uint64_t i = 0; i < size(); ++i) result.push_back((*this)[i]);
            return result;
        } catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::ResourceExhausted}); }
          catch (const std::length_error&) { return std::unexpected(Error{ErrorCode::SizeOverflow}); }
          catch (const AccessError& e) { return std::unexpected(e.error()); }
    }
    [[nodiscard]] Result<Snapshot> snapshot(std::uint64_t max_bytes = UINT64_MAX) const;
private:
    MessageArray(std::shared_ptr<detail::State> state, detail::Handle handle) : state_(std::move(state)), handle_(handle) {}
    MessageArray(detail::BindTag, detail::Binding b) : state_(std::move(b.state)), handle_(b.handle), field_(b.field) {}
    detail::Binding binding() const { return {state_, handle_, field_}; }
    static detail::Binding take_binding(MessageArray& other) {
        if (other.field_) return detail::checked(detail::clone(other.binding()));
        return {std::exchange(other.state_, {}), other.handle_, false};
    }
    T& element(std::uint64_t index) const {
        auto handle = detail::checked(detail::child(state_, handle_, index));
        // Wrappers survive sibling relocation; generation detects removed elements.
        const auto found = elements_.find(index);
        if (found != elements_.end() && found->second.first == handle.generation) return *found->second.second;
        auto value = std::unique_ptr<T>(new T(detail::RecordAccess::bind<T>({state_, handle, true})));
        auto [entry, inserted] = elements_.try_emplace(index, handle.generation, nullptr);
        (void)inserted;
        entry->second.first = handle.generation;
        entry->second.second = std::move(value);
        return *entry->second.second;
    }
    std::shared_ptr<detail::State> state_;
    detail::Handle handle_;
    bool field_ = false;
    mutable std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::unique_ptr<T>>> elements_;
};

// Arbitrary C++ objects are valid local elements, but acquire no wire eligibility.
template<class T> requires (!detail::transferable<T>())
class MessageArray<T> {
public:
    using iterator = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;
    std::size_t size() const noexcept { return values_.size(); }
    std::size_t capacity() const noexcept { return values_.capacity(); }
    bool empty() const noexcept { return values_.empty(); }
    T& operator[](std::size_t i) { return values_.at(i); }
    const T& operator[](std::size_t i) const { return values_.at(i); }
    T& at(std::size_t i) { return values_.at(i); }
    const T& at(std::size_t i) const { return values_.at(i); }
    iterator begin() noexcept { return values_.begin(); }
    iterator end() noexcept { return values_.end(); }
    const_iterator begin() const noexcept { return values_.begin(); }
    const_iterator end() const noexcept { return values_.end(); }
    Result<void> resize(std::size_t n) { return attempt([&] { values_.resize(n); }); }
    Result<void> reserve(std::size_t n) { return attempt([&] { values_.reserve(n); }); }
    Result<void> clear() { values_.clear(); return {}; }
    Result<void> shrink_to_fit() { return attempt([&] { values_.shrink_to_fit(); }); }
    Result<void> pop_back() { if (empty()) return std::unexpected(Error{ErrorCode::OutOfRange}); values_.pop_back(); return {}; }
    Result<void> push_back(const T& v) { return attempt([&] { values_.push_back(v); }); }
    Result<void> push_back(T&& v) { return attempt([&] { values_.push_back(std::move(v)); }); }
    template<class... Args> Result<void> emplace_back(Args&&... args) { return attempt([&] { values_.emplace_back(std::forward<Args>(args)...); }); }
private:
    template<class F> static Result<void> attempt(F&& f) {
        try { f(); return {}; }
        catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::ResourceExhausted}); }
        catch (const std::length_error&) { return std::unexpected(Error{ErrorCode::SizeOverflow}); }
    }
    std::vector<T> values_;
};

template<class T> class MessageArrayView {
    template<class> friend class MessageArrayView;
    friend class Snapshot;
public:
    std::uint64_t size() const { return detail::image_size(image_, id_); }
    bool empty() const { return size() == 0; }
    auto operator[](std::uint64_t index) const {
        if constexpr (detail::Record<T>) {
            auto b = detail::checked(detail::copy_image_record(image_, detail::image_child(image_, id_, index), detail::record_type<T>()));
            return detail::RecordAccess::bind<T>(std::move(b));
        } else if constexpr (detail::is_array<T>) {
            using Element = typename detail::IsArray<T>::Element;
            return MessageArrayView<Element>(image_, detail::image_child(image_, id_, index));
        } else return detail::decode<T>(detail::image_read(image_, id_, index));
    }
    class Iterator {
    public:
        auto operator*() const { return MessageArrayView(image_, id_)[index_]; }
        Iterator& operator++() { ++index_; return *this; }
        friend bool operator==(const Iterator& a, const Iterator& b) { return a.image_ == b.image_ && a.id_ == b.id_ && a.index_ == b.index_; }
    private:
        friend class MessageArrayView;
        Iterator(std::shared_ptr<const detail::Image> image, std::uint64_t id, std::uint64_t index) : image_(std::move(image)), id_(id), index_(index) {}
        std::shared_ptr<const detail::Image> image_;
        std::uint64_t id_, index_;
    };
    Iterator begin() const { return Iterator(image_, id_, 0); }
    Iterator end() const { return Iterator(image_, id_, size()); }
private:
    MessageArrayView(std::shared_ptr<const detail::Image> image, std::uint64_t id) : image_(std::move(image)), id_(id) {}
    std::shared_ptr<const detail::Image> image_;
    std::uint64_t id_;
};

class Snapshot {
public:
    Snapshot() = default;
    // Always copies into owned process memory, then validates that copy.
    [[nodiscard]] static Result<Snapshot> copy_from(std::span<const std::byte>);
    std::span<const std::byte> bytes() const noexcept;
    std::uint64_t root_count() const noexcept;
    template<class T> [[nodiscard]] Result<MessageArrayView<T>> array(std::uint64_t index) const {
        auto id = detail::image_root(image_, index, detail::type<T>());
        if (!id) return std::unexpected(id.error());
        return MessageArrayView<T>(image_, *id);
    }
private:
    friend Result<Snapshot> detail::snapshot(const std::shared_ptr<detail::State>&, std::uint64_t);
    friend class detail::FrameworkAccess;
    friend Result<Snapshot> detail::seal_binding(detail::Binding);
    friend Result<Snapshot> detail::publish_binding(detail::Binding, std::span<std::byte>, std::shared_ptr<void>);
    explicit Snapshot(std::shared_ptr<const detail::Image> image) : image_(std::move(image)) {}
    std::shared_ptr<const detail::Image> image_;
};
template<class T> Result<Snapshot> MessageArray<T>::snapshot(std::uint64_t max_bytes) const {
    auto valid = detail::info(state_, handle_);
    if (!valid) return std::unexpected(valid.error());
    return detail::snapshot(state_, max_bytes); // snapshots the entire owning message
}

class Message {
public:
    Message(); // local, growable; may throw bad_alloc during construction
    ~Message();
    Message(Message&&) noexcept;
    Message& operator=(Message&&) noexcept;
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
    template<class T> [[nodiscard]] Result<MessageArray<T>> add_array() {
        auto handle = detail::add_root(state_, detail::type<T>());
        if (!handle) return std::unexpected(handle.error());
        return MessageArray<T>(state_, *handle);
    }
    template<class T> [[nodiscard]] Result<MessageArray<T>> array(std::uint64_t index) {
        auto handle = detail::root(state_, index, detail::type<T>());
        if (!handle) return std::unexpected(handle.error());
        return MessageArray<T>(state_, *handle);
    }
    [[nodiscard]] Result<Usage> usage() const { return detail::usage(state_); }
    // Releases all reserved element capacity, including nested arrays.
    [[nodiscard]] Result<void> compact() { return detail::compact(state_); }
    // Checks packed length, not reserved capacity; never consumes the source.
    [[nodiscard]] Result<Snapshot> snapshot(std::uint64_t max_bytes = UINT64_MAX) const { return detail::snapshot(state_, max_bytes); }
private:
    friend class detail::FrameworkAccess;
    explicit Message(std::shared_ptr<detail::State> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::State> state_;
};
} // namespace vinc::message

namespace vinc {
template<class T> using MessageArray = message::MessageArray<T>;
template<class T> using MessageArrayView = message::MessageArrayView<T>;
}
