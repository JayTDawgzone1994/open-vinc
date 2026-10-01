#include "framework_access.hpp"
#include <cstring>
#include <functional>
#include <utility>

namespace vinc::message::detail {
namespace {
constexpr std::uint64_t header_size = 64, descriptor_size = 48;
constexpr std::uint64_t magic = 0x00525241434e4956ULL; // "VINCARR\0", little endian
struct Failure { Error error; };
[[noreturn]] void fail(ErrorCode code) { throw Failure{{code}}; }
void require(bool value, ErrorCode code) { if (!value) fail(code); }
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    if (b > UINT64_MAX - a) fail(ErrorCode::SizeOverflow);
    return a + b;
}
std::uint64_t mul(std::uint64_t a, std::uint64_t b) {
    if (a && b > UINT64_MAX / a) fail(ErrorCode::SizeOverflow);
    return a * b;
}
std::uint64_t aligned(std::uint64_t n) { return add(n, 7) & ~std::uint64_t{7}; }
std::size_t host_size(std::uint64_t n) {
    require(n <= static_cast<std::uint64_t>((std::numeric_limits<std::ptrdiff_t>::max)()), ErrorCode::SizeOverflow);
    return static_cast<std::size_t>(n);
}
std::uint64_t width(Type type) {
    if (type.scalar == 0) { require(type.hash != 0, ErrorCode::TypeMismatch); return 8; }
    require(type.depth != 0, ErrorCode::TypeMismatch);
    std::uint64_t scalar;
    switch (type.scalar) {
    case 1: case 2: case 11: scalar = 1; break;
    case 3: case 4: scalar = 2; break;
    case 5: case 6: case 9: scalar = 4; break;
    case 7: case 8: case 10: scalar = 8; break;
    default: fail(ErrorCode::TypeMismatch);
    }
    return type.depth > 1 ? 8 : scalar;
}
bool primitive(Type t) { return t.depth == 1 && t.scalar != 0; }
bool same(Type a, Type b) { return a.scalar == b.scalar && a.depth == b.depth && a.hash == b.hash; }
Type element_type(Type t) { require(t.depth > 0, ErrorCode::TypeMismatch); --t.depth; return t; }
template<class F> auto guarded(F&& f) -> Result<std::invoke_result_t<F>> {
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<F>>) { f(); return {}; }
        else return f();
    } catch (const Failure& e) { return std::unexpected(e.error); }
      catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::ResourceExhausted}); }
      catch (const std::length_error&) { return std::unexpected(Error{ErrorCode::SizeOverflow}); }
}
void put(std::span<std::byte> bytes, std::uint64_t position, std::uint64_t value, unsigned size = 8) {
    for (unsigned i = 0; i < size; ++i) bytes[host_size(position + i)] = static_cast<std::byte>((value >> (8 * i)) & 255);
}
std::uint64_t get(std::span<const std::byte> bytes, std::uint64_t position, unsigned size = 8) {
    require(position <= bytes.size() && size <= bytes.size() - position, ErrorCode::InvalidData);
    std::uint64_t result = 0;
    for (unsigned i = 0; i < size; ++i) result |= std::uint64_t(std::to_integer<unsigned>(bytes[host_size(position + i)])) << (8 * i);
    return result;
}
struct Node {
    bool alive = false;
    std::uint64_t generation = 0, revision = 1;
    Type type;
    std::uint64_t size = 0, capacity = 0, offset = 0, descriptor = 0;
    std::vector<Handle> children;
    std::span<const std::byte> imported; // transaction-local; cleared before commit
};
struct Plan {
    std::vector<Node> nodes;
    std::vector<Handle> roots;
    std::uint64_t next_generation = 1;
    std::size_t first_free = 0;
};
struct Layout {
    std::uint64_t table = 0, payload = 0, total = 0, count = 0;
    std::vector<std::uint64_t> ids, offsets;
};
Layout layout(const Plan& plan, bool compact) {
    Layout result;
    result.ids.resize(plan.nodes.size(), UINT64_MAX);
    result.offsets.resize(plan.nodes.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i)
        if (plan.nodes[i].alive) result.ids[i] = result.count++;
    result.table = aligned(add(header_size, mul(plan.roots.size(), 8)));
    result.payload = add(result.table, mul(result.count, descriptor_size));
    result.total = result.payload;
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& node = plan.nodes[i];
        if (!node.alive) continue;
        const auto bytes = mul(compact ? node.size : node.capacity, width(node.type));
        if (bytes) {
            result.offsets[i] = result.total;
            result.total = add(result.total, aligned(bytes));
        }
    }
    (void)host_size(result.total);
    return result;
}
}

struct State {
    std::vector<std::byte> local;
    std::span<std::byte> fixed;
    std::shared_ptr<void> owner;
    bool is_fixed = false, active = true;
    std::uint64_t scratch_limit = UINT64_MAX, epoch = 1;
    Plan plan;
    std::span<std::byte> bytes() { return is_fixed ? fixed : std::span<std::byte>(local); }
    std::uint64_t limit() const { return is_fixed ? fixed.size() : UINT64_MAX; }
};
struct Image {
    struct Entry { Type type; std::uint64_t offset, size, capacity; };
    std::vector<std::byte> owned;
    std::span<const std::byte> borrowed;
    std::shared_ptr<void> owner;
    std::vector<Entry> entries;
    std::vector<std::uint64_t> roots;
    std::span<const std::byte> bytes() const { return owner ? borrowed : std::span<const std::byte>(owned); }
};

namespace {
State& live(const std::shared_ptr<State>& state) {
    require(state && state->active, ErrorCode::Closed);
    return *state;
}
const Node& node(const State& state, Handle handle) {
    require(handle.index < state.plan.nodes.size(), ErrorCode::StaleHandle);
    const auto& value = state.plan.nodes[host_size(handle.index)];
    require(value.alive && value.generation == handle.generation, ErrorCode::StaleHandle);
    return value;
}
void before_change(const State& state) { require(state.epoch != UINT64_MAX, ErrorCode::SizeOverflow); }
Handle new_node(Plan& plan, Type type) {
    (void)width(type);
    require(plan.next_generation != UINT64_MAX, ErrorCode::SizeOverflow);
    std::size_t index = plan.first_free;
    while (index < plan.nodes.size() && plan.nodes[index].alive) ++index;
    Node value; value.alive = true; value.generation = plan.next_generation++; value.type = type;
    if (index == plan.nodes.size()) plan.nodes.push_back(std::move(value));
    else plan.nodes[index] = std::move(value);
    plan.first_free = index + 1;
    return {index, plan.nodes[index].generation};
}
Handle initialized_node(Plan& plan, Type type, bool singular = false) {
    const auto root = new_node(plan, type);
    std::vector<std::pair<Handle, bool>> pending{{root, singular}};
    while (!pending.empty()) {
        const auto [handle, one] = pending.back(); pending.pop_back();
        const auto current = plan.nodes[host_size(handle.index)].type;
        if (current.depth == 0) {
            require(current.schema && current.schema->hash == current.hash, ErrorCode::TypeMismatch);
            for (const auto& field : current.schema->fields) {
                auto child = new_node(plan, field.type);
                plan.nodes[host_size(handle.index)].children.push_back(child);
                pending.emplace_back(child, field.singular);
            }
            auto& n = plan.nodes[host_size(handle.index)];
            n.size = n.capacity = n.children.size();
        } else if (one) {
            require(primitive(current), ErrorCode::TypeMismatch);
            auto& n = plan.nodes[host_size(handle.index)]; n.size = n.capacity = 1;
        }
    }
    return root;
}
std::uint64_t initial_cost(Type type, bool singular = false, unsigned depth = 0) {
    require(depth <= 64, ErrorCode::SizeOverflow);
    if (type.depth != 0) return add(descriptor_size, singular ? aligned(width(type)) : 0);
    require(type.schema != nullptr, ErrorCode::TypeMismatch);
    auto cost = add(descriptor_size, aligned(mul(type.schema->fields.size(), 8)));
    for (const auto& field : type.schema->fields) cost = add(cost, initial_cost(field.type, field.singular, depth + 1));
    return cost;
}
void remove_tree(Plan& plan, Handle root) {
    std::vector<Handle> pending{root};
    while (!pending.empty()) {
        const auto id = pending.back().index; pending.pop_back();
        auto& value = plan.nodes[host_size(id)];
        pending.insert(pending.end(), value.children.begin(), value.children.end());
        value = Node{}; // old views are invalid even if this descriptor is reused
        plan.first_free = std::min(plan.first_free, host_size(id));
    }
}
std::span<const std::byte> source_bytes(State& state, std::size_t index, const Node& target) {
    if (index >= state.plan.nodes.size()) return {};
    const auto& source = state.plan.nodes[index];
    if (!source.alive || source.generation != target.generation || !primitive(source.type)) return {};
    const auto size = mul(std::min(source.size, target.size), width(source.type));
    if (!size) return {};
    return state.bytes().subspan(host_size(source.offset), host_size(size));
}
void render_into(std::span<std::byte> bytes, State& state, const Plan& plan, const Layout& positions, bool compact) {
    std::fill(bytes.begin(), bytes.end(), std::byte{0});
    put(bytes, 0, magic); put(bytes, 8, 2, 4); put(bytes, 12, compact ? 1 : 2, 4);
    put(bytes, 16, positions.total); put(bytes, 24, positions.count); put(bytes, 32, plan.roots.size());
    put(bytes, 40, positions.table); put(bytes, 48, positions.payload);
    for (std::size_t i = 0; i < plan.roots.size(); ++i) put(bytes, header_size + i * 8, positions.ids[host_size(plan.roots[i].index)]);
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& value = plan.nodes[i];
        if (!value.alive) continue;
        const auto offset = positions.offsets[i];
        const auto descriptor = positions.table + positions.ids[i] * descriptor_size;
        put(bytes, descriptor, (compact ? value.size : value.capacity) ? offset : 0);
        put(bytes, descriptor + 8, value.size);
        put(bytes, descriptor + 16, value.type.scalar, 4);
        put(bytes, descriptor + 20, value.type.depth, 4);
        put(bytes, descriptor + 24, compact ? value.size : value.capacity);
        put(bytes, descriptor + 32, value.type.hash);
        if (!primitive(value.type)) {
            for (std::size_t j = 0; j < value.children.size(); ++j)
                put(bytes, offset + j * 8, positions.ids[host_size(value.children[j].index)]);
        } else {
            auto source = source_bytes(state, i, value);
            if (!value.imported.empty()) source = value.imported;
            require(source.size() <= mul(value.size, width(value.type)), ErrorCode::InvalidArgument);
            if (!source.empty()) std::memcpy(bytes.data() + host_size(offset), source.data(), source.size());
        }
    }
}
std::vector<std::byte> render(State& state, const Plan& plan, const Layout& positions, bool compact) {
    require(positions.total <= state.scratch_limit, ErrorCode::ResourceExhausted);
    std::vector<std::byte> bytes(host_size(positions.total));
    render_into(bytes, state, plan, positions, compact);
    return bytes;
}
void rebuild(State& state, Plan& plan) {
    before_change(state);
    auto positions = layout(plan, false);
    require(positions.total <= state.limit(), ErrorCode::CapacityExceeded);
    auto bytes = render(state, plan, positions, false);
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        auto& next = plan.nodes[i];
        if (next.alive && i < state.plan.nodes.size()) {
            const auto& old = state.plan.nodes[i];
            if (old.alive && old.generation == next.generation &&
                (old.size != next.size || old.capacity != next.capacity || old.children != next.children || !next.imported.empty()))
                next.revision = old.revision + 1;
        }
        plan.nodes[i].offset = positions.offsets[i];
        if (plan.nodes[i].alive) plan.nodes[i].descriptor = positions.table + positions.ids[i] * descriptor_size;
        plan.nodes[i].imported = {};
    }
    // Everything that can allocate/fail has completed. Commit is non-throwing.
    if (state.is_fixed) std::memcpy(state.fixed.data(), bytes.data(), bytes.size());
    else state.local.swap(bytes);
    state.plan.nodes.swap(plan.nodes); state.plan.roots.swap(plan.roots);
    state.plan.next_generation = plan.next_generation;
    state.plan.first_free = plan.first_free;
    ++state.epoch;
}
std::uint64_t preferred_capacity(std::uint64_t previous, std::uint64_t needed) {
    if (previous >= needed) return previous;
    const auto growth = previous > UINT64_MAX / 2 ? needed : std::max<std::uint64_t>(4, previous * 2);
    return std::max(needed, growth);
}
void rebuild_with_growth(State& state, Plan& plan, std::uint64_t id, std::uint64_t minimum) {
    const auto old = plan.nodes[host_size(id)].capacity;
    plan.nodes[host_size(id)].capacity = preferred_capacity(old, minimum);
    try { rebuild(state, plan); }
    catch (const Failure& failure) {
        if ((failure.error.code != ErrorCode::CapacityExceeded && failure.error.code != ErrorCode::ResourceExhausted &&
             failure.error.code != ErrorCode::SizeOverflow) || plan.nodes[host_size(id)].capacity == minimum) throw;
        plan.nodes[host_size(id)].capacity = minimum;
        rebuild(state, plan);
    }
}
Handle clone_tree(Plan& plan, State& source, Handle root) {
    const auto& first = node(source, root);
    auto result = new_node(plan, first.type);
    std::vector<std::pair<Handle, Handle>> pending{{root, result}};
    while (!pending.empty()) {
        const auto [from, to] = pending.back(); pending.pop_back();
        const auto& original = node(source, from);
        plan.nodes[host_size(to.index)].size = original.size;
        plan.nodes[host_size(to.index)].capacity = original.size;
        if (primitive(original.type)) {
            const auto count = mul(original.size, width(original.type));
            if (count) plan.nodes[host_size(to.index)].imported = source.bytes().subspan(host_size(original.offset), host_size(count));
        } else {
            plan.nodes[host_size(to.index)].children.reserve(original.children.size());
            for (auto child : original.children) {
                auto replacement = new_node(plan, node(source, child).type);
                plan.nodes[host_size(to.index)].children.push_back(replacement);
                pending.emplace_back(child, replacement);
            }
        }
    }
    return result;
}
void validate(Image& image) {
    const auto bytes = image.bytes();
    require(bytes.size() >= header_size && get(bytes, 0) == magic && get(bytes, 8, 4) == 2 &&
        (get(bytes, 12, 4) == 1 || get(bytes, 12, 4) == 2) && get(bytes, 16) == bytes.size() && get(bytes, 56) == 0, ErrorCode::InvalidData);
    const auto count = get(bytes, 24), roots = get(bytes, 32), table = get(bytes, 40), payload = get(bytes, 48);
    require(roots <= count && count <= bytes.size() / descriptor_size, ErrorCode::InvalidData);
    require(table == aligned(add(header_size, mul(roots, 8))) && payload == add(table, mul(count, descriptor_size)) && payload <= bytes.size(), ErrorCode::InvalidData);
    image.entries.reserve(host_size(count)); image.roots.reserve(host_size(roots));
    std::vector<bool> owned(host_size(count), false);
    for (std::uint64_t i = 0; i < roots; ++i) {
        const auto id = get(bytes, header_size + i * 8);
        require(id < count && !owned[host_size(id)], ErrorCode::InvalidData);
        owned[host_size(id)] = true; image.roots.push_back(id);
    }
    auto cursor = payload;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto at = table + i * descriptor_size;
        const auto offset = get(bytes, at), size = get(bytes, at + 8);
        Type type{static_cast<std::uint32_t>(get(bytes, at + 16, 4)), static_cast<std::uint32_t>(get(bytes, at + 20, 4))};
        const auto capacity = get(bytes, at + 24);
        type.hash = get(bytes, at + 32);
        require(type.scalar <= 11 && (type.scalar ? type.depth > 0 && type.hash == 0 : type.hash != 0) && get(bytes, at + 40) == 0, ErrorCode::InvalidData);
        require(size <= capacity && (get(bytes, 12, 4) != 1 || size == capacity), ErrorCode::InvalidData);
        require(type.depth != 0 || size == capacity, ErrorCode::InvalidData);
        const auto length = mul(capacity, width(type));
        const auto used = mul(size, width(type));
        require(capacity ? offset == cursor : offset == 0, ErrorCode::InvalidData);
        if (length) {
            require(cursor <= bytes.size() && length <= bytes.size() - cursor, ErrorCode::InvalidData);
            const auto end = add(cursor, aligned(length));
            require(end <= bytes.size(), ErrorCode::InvalidData);
            for (auto padding = cursor + used; padding < end; ++padding)
                require(bytes[host_size(padding)] == std::byte{0}, ErrorCode::InvalidData);
            cursor = end;
        }
        if (type.depth == 1 && type.scalar == 11)
            for (std::uint64_t j = 0; j < size; ++j) require(std::to_integer<unsigned>(bytes[host_size(offset + j)]) <= 1, ErrorCode::InvalidData);
        image.entries.push_back({type, offset, size, capacity});
    }
    require(cursor == bytes.size(), ErrorCode::InvalidData);
    for (const auto& value : image.entries) {
        if (primitive(value.type)) continue;
        for (std::uint64_t j = 0; j < value.size; ++j) {
            const auto id = get(bytes, value.offset + j * 8);
            require(id < count && !owned[host_size(id)], ErrorCode::InvalidData);
            const auto child_type = image.entries[host_size(id)].type;
            if (value.type.depth != 0) require(same(child_type, element_type(value.type)), ErrorCode::InvalidData);
            owned[host_size(id)] = true;
        }
    }
    // Unique ownership plus reachability excludes disconnected cycles, including records.
    require(std::all_of(owned.begin(), owned.end(), [](bool value) { return value; }), ErrorCode::InvalidData);
    auto pending = image.roots;
    std::uint64_t reached = 0;
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back(); ++reached;
        require(reached <= count, ErrorCode::InvalidData);
        const auto& e = image.entries[host_size(id)];
        if (!primitive(e.type)) for (std::uint64_t i = 0; i < e.size; ++i) pending.push_back(get(bytes, e.offset + i * 8));
    }
    require(reached == count, ErrorCode::InvalidData);
}
void validate_schema(Image& image, std::uint64_t id, Type expected) {
    struct Check { std::uint64_t id; Type type; bool singular; };
    std::vector<Check> pending{{id, expected, false}};
    while (!pending.empty()) {
        const auto check = pending.back(); pending.pop_back();
        auto& value = image.entries[host_size(check.id)];
        require(same(value.type, check.type), ErrorCode::TypeMismatch);
        value.type.schema = check.type.schema;
        if (check.singular) require(value.size == 1, ErrorCode::InvalidData);
        if (value.type.depth == 0) {
            require(check.type.schema && value.size == check.type.schema->fields.size(), ErrorCode::InvalidData);
            for (std::uint64_t i = 0; i < value.size; ++i) {
                const auto& field = check.type.schema->fields[host_size(i)];
                pending.push_back({get(image.bytes(), value.offset + i * 8), field.type, field.singular});
            }
        } else if (!primitive(value.type)) {
            for (std::uint64_t i = 0; i < value.size; ++i)
                pending.push_back({get(image.bytes(), value.offset + i * 8), element_type(check.type), false});
        }
    }
}
std::shared_ptr<Image> make_image(State& state, std::uint64_t maximum) {
    auto positions = layout(state.plan, true);
    require(positions.total <= maximum, ErrorCode::CapacityExceeded);
    auto image = std::make_shared<Image>();
    image->owned = render(state, state.plan, positions, true);
    validate(*image);
    return image;
}
}

std::shared_ptr<State> local_state() {
    auto state = std::make_shared<State>();
    state->local = render(*state, state->plan, layout(state->plan, false), false);
    return state;
}
void release_message(const std::shared_ptr<State>& state) noexcept {
    if (state && state->is_fixed) {
        state->active = false; state->fixed = {}; state->owner.reset();
    }
}
Result<Handle> add_root(const std::shared_ptr<State>& ptr, Type type) {
    return guarded([&] {
        auto& state = live(ptr);
        require(add(header_size + 8, initial_cost(type)) <= state.limit(), ErrorCode::CapacityExceeded);
        Plan plan = state.plan;
        auto result = initialized_node(plan, type);
        plan.roots.push_back(result);
        rebuild(state, plan);
        return result;
    });
}
Result<Handle> root(const std::shared_ptr<State>& ptr, std::uint64_t index, Type type) {
    return guarded([&] {
        auto& state = live(ptr);
        require(index < state.plan.roots.size(), ErrorCode::OutOfRange);
        auto handle = state.plan.roots[host_size(index)];
        require(same(node(state, handle).type, type), ErrorCode::TypeMismatch);
        return handle;
    });
}
Result<Info> info(const std::shared_ptr<State>& ptr, Handle handle) {
    return guarded([&] { const auto& value = node(live(ptr), handle); return Info{value.size, value.capacity}; });
}
Result<Handle> child(const std::shared_ptr<State>& ptr, Handle handle, std::uint64_t index) {
    return guarded([&] {
        const auto& value = node(live(ptr), handle);
        require(!primitive(value.type), ErrorCode::TypeMismatch);
        require(index < value.size, ErrorCode::OutOfRange);
        return value.children[host_size(index)];
    });
}
Result<std::array<std::byte, 8>> read(const std::shared_ptr<State>& ptr, Handle handle, std::uint64_t index) {
    return guarded([&] {
        auto& state = live(ptr); const auto& value = node(state, handle);
        require(primitive(value.type), ErrorCode::TypeMismatch); require(index < value.size, ErrorCode::OutOfRange);
        std::array<std::byte, 8> output{};
        const auto count = width(value.type);
        std::memcpy(output.data(), state.bytes().data() + host_size(value.offset + index * count), host_size(count));
        return output;
    });
}
Result<void> write(const std::shared_ptr<State>& ptr, Handle handle, std::uint64_t index, std::span<const std::byte> bytes) {
    return guarded([&] {
        auto& state = live(ptr); const auto& value = node(state, handle);
        require(primitive(value.type) && bytes.size() == width(value.type), ErrorCode::TypeMismatch);
        require(index < value.size, ErrorCode::OutOfRange);
        std::memcpy(state.bytes().data() + host_size(value.offset + index * bytes.size()), bytes.data(), bytes.size());
    });
}
Result<void> resize(const std::shared_ptr<State>& ptr, Handle handle, std::uint64_t size, bool reserve_only) {
    return guarded([&] {
        auto& state = live(ptr); const auto& original = node(state, handle);
        require(original.type.depth != 0, ErrorCode::TypeMismatch);
        const auto stride = width(original.type);
        (void)host_size(mul(size, stride));
        if (reserve_only && size <= original.capacity) return;
        if (!reserve_only && size == original.size) return;
        before_change(state);
        if (!reserve_only && primitive(original.type) && size <= original.capacity) {
            if (size > original.size)
                std::memset(state.bytes().data() + host_size(original.offset + original.size * stride), 0, host_size((size - original.size) * stride));
            if (size < original.size)
                std::memset(state.bytes().data() + host_size(original.offset + size * stride), 0, host_size((original.size - size) * stride));
            put(state.bytes(), original.descriptor + 8, size);
            put(state.bytes(), 12, 2, 4);
            state.plan.nodes[host_size(handle.index)].size = size;
            ++state.plan.nodes[host_size(handle.index)].revision;
            ++state.epoch;
            return;
        }
        // Reject impossible fixed requests before allocating per-child metadata.
        const auto lower_bound = add(header_size, mul(size, !primitive(original.type) && !reserve_only ? add(initial_cost(element_type(original.type)), 8) : stride));
        require(lower_bound <= state.limit(), ErrorCode::CapacityExceeded);
        Plan plan = state.plan;
        if (!reserve_only) {
            if (!primitive(original.type)) {
                if (size < original.size) {
                    for (std::uint64_t i = size; i < original.size; ++i) remove_tree(plan, original.children[host_size(i)]);
                    plan.nodes[host_size(handle.index)].children.resize(host_size(size));
                } else {
                    plan.nodes[host_size(handle.index)].children.reserve(host_size(size));
                    for (auto i = original.size; i < size; ++i) {
                        auto next = initialized_node(plan, element_type(original.type));
                        plan.nodes[host_size(handle.index)].children.push_back(next);
                    }
                }
            }
            plan.nodes[host_size(handle.index)].size = size;
        }
        if (reserve_only) {
            plan.nodes[host_size(handle.index)].capacity = size;
            rebuild(state, plan);
        } else rebuild_with_growth(state, plan, handle.index, size);
    });
}
Result<void> append(const std::shared_ptr<State>& ptr, Handle handle, std::span<const std::byte> bytes) {
    return guarded([&] {
        auto& state = live(ptr); const auto& original = node(state, handle);
        require(primitive(original.type) && bytes.size() == width(original.type), ErrorCode::TypeMismatch);
        const auto index = original.size;
        const auto size = add(index, 1);
        auto resized = resize(ptr, handle, size, false);
        if (!resized) throw Failure{resized.error()};
        // Bytes passed here are an owned scalar encoding; no alias to the arena.
        const auto& value = node(state, handle);
        std::memcpy(state.bytes().data() + host_size(value.offset + index * bytes.size()), bytes.data(), bytes.size());
    });
}
Result<void> append_child(const std::shared_ptr<State>& ptr, Handle handle, const std::shared_ptr<State>& from, Handle source) {
    return guarded([&] {
        auto& state = live(ptr); auto& source_state = live(from);
        const auto& value = node(state, handle); const auto& original = node(source_state, source);
        require(value.type.depth > 0 && !primitive(value.type) && same(original.type, element_type(value.type)), ErrorCode::TypeMismatch);
        Plan plan = state.plan;
        const auto copied = clone_tree(plan, source_state, source);
        plan.nodes[host_size(handle.index)].children.push_back(copied);
        plan.nodes[host_size(handle.index)].size = add(value.size, 1);
        rebuild_with_growth(state, plan, handle.index, value.size + 1);
    });
}
Result<void> assign(const std::shared_ptr<State>& ptr, Handle handle, std::span<const std::byte> bytes, std::uint64_t size) {
    return guarded([&] {
        auto& state = live(ptr); const auto& value = node(state, handle);
        require(primitive(value.type) && mul(size, width(value.type)) == bytes.size(), ErrorCode::TypeMismatch);
        before_change(state);
        if (size <= value.capacity) {
            if (!bytes.empty()) std::memcpy(state.bytes().data() + host_size(value.offset), bytes.data(), bytes.size());
            if (size < value.size) std::memset(state.bytes().data() + host_size(value.offset + bytes.size()), 0, host_size((value.size - size) * width(value.type)));
            put(state.bytes(), value.descriptor + 8, size);
            put(state.bytes(), 12, 2, 4);
            state.plan.nodes[host_size(handle.index)].size = size;
            ++state.plan.nodes[host_size(handle.index)].revision; ++state.epoch; return;
        }
        Plan plan = state.plan;
        plan.nodes[host_size(handle.index)].size = size;
        plan.nodes[host_size(handle.index)].imported = bytes;
        rebuild_with_growth(state, plan, handle.index, size);
    });
}
Result<void> preflight_assign(const std::shared_ptr<State>& ptr, Handle handle, std::uint64_t size) {
    return guarded([&] {
        auto& state = live(ptr); const auto& value = node(state, handle);
        require(primitive(value.type), ErrorCode::TypeMismatch);
        const auto length = mul(size, width(value.type));
        (void)host_size(length);
        if (size > value.capacity) {
            Plan plan = state.plan;
            plan.nodes[host_size(handle.index)].size = size;
            plan.nodes[host_size(handle.index)].capacity = size;
            const auto needed = layout(plan, false).total;
            require(needed <= state.limit(), ErrorCode::CapacityExceeded);
            require(needed <= state.scratch_limit, ErrorCode::ResourceExhausted);
        }
        require(length <= state.scratch_limit, ErrorCode::ResourceExhausted);
    });
}
Result<void> shrink(const std::shared_ptr<State>& ptr, Handle handle) {
    return guarded([&] {
        auto& state = live(ptr); const auto& value = node(state, handle);
        if (value.size == value.capacity) return;
        Plan plan = state.plan;
        plan.nodes[host_size(handle.index)].capacity = value.size;
        rebuild(state, plan);
    });
}
Result<void> compact(const std::shared_ptr<State>& ptr) {
    return guarded([&] {
        auto& state = live(ptr);
        Plan plan = state.plan;
        bool changed = false;
        for (auto& value : plan.nodes) {
            changed |= value.alive && value.capacity != value.size;
            value.capacity = value.size;
        }
        if (changed) rebuild(state, plan);
    });
}
Result<Usage> usage(const std::shared_ptr<State>& ptr) {
    return guarded([&] {
        auto& state = live(ptr);
        return Usage{layout(state.plan, true).total, layout(state.plan, false).total, state.is_fixed ? state.fixed.size() : 0};
    });
}
Result<Binding> clone(Binding source) {
    return guarded([&] {
        auto& from = live(source.state);
        auto target = local_state();
        Plan plan;
        auto handle = clone_tree(plan, from, source.handle);
        plan.roots.push_back(handle);
        rebuild(*target, plan);
        return Binding{std::move(target), handle, false};
    });
}
Result<void> replace(Binding target, Binding source) {
    return guarded([&] {
        auto& to = live(target.state); auto& from = live(source.state);
        require(same(node(to, target.handle).type, node(from, source.handle).type), ErrorCode::TypeMismatch);
        if (target.state == source.state && target.handle.index == source.handle.index) return;
        Plan plan = to.plan;
        std::vector<std::pair<Handle, Handle>> pending{{target.handle, source.handle}};
        while (!pending.empty()) {
            auto [destination, origin] = pending.back(); pending.pop_back();
            const auto& original = node(from, origin);
            auto& current = plan.nodes[host_size(destination.index)];
            if (original.type.depth == 0) {
                require(current.children.size() == original.children.size(), ErrorCode::TypeMismatch);
                for (std::size_t i = 0; i < current.children.size(); ++i) pending.emplace_back(current.children[i], original.children[i]);
            } else if (primitive(original.type)) {
                current.size = original.size;
                current.capacity = std::max(current.capacity, original.size);
                if (original.size) current.imported = from.bytes().subspan(host_size(original.offset), host_size(mul(original.size, width(original.type))));
            } else {
                auto children = current.children;
                for (auto h : children) remove_tree(plan, h);
                plan.nodes[host_size(destination.index)].children.clear();
                for (auto h : original.children) {
                    auto copied = clone_tree(plan, from, h);
                    plan.nodes[host_size(destination.index)].children.push_back(copied);
                }
                auto& updated = plan.nodes[host_size(destination.index)];
                updated.size = original.size; updated.capacity = std::max(updated.capacity, original.size);
            }
        }
        rebuild(to, plan);
    });
}
Result<void> assign_children(Binding target, std::span<const Binding> sources) {
    return guarded([&] {
        auto& state = live(target.state);
        const auto& original = node(state, target.handle);
        require(original.type.depth > 0 && !primitive(original.type), ErrorCode::TypeMismatch);
        const auto type = element_type(original.type);
        require(add(header_size, mul(sources.size(), add(initial_cost(type), 8))) <= state.limit(), ErrorCode::CapacityExceeded);
        Plan plan = state.plan;
        for (auto child : original.children) remove_tree(plan, child);
        plan.nodes[host_size(target.handle.index)].children.clear();
        for (const auto& source : sources) {
            auto& from = live(source.state);
            require(same(node(from, source.handle).type, type), ErrorCode::TypeMismatch);
            const auto copied = clone_tree(plan, from, source.handle);
            plan.nodes[host_size(target.handle.index)].children.push_back(copied);
        }
        auto& next = plan.nodes[host_size(target.handle.index)];
        next.size = sources.size(); next.capacity = std::max(next.capacity, next.size);
        rebuild(state, plan);
    });
}
Result<Binding> create_record(const Schema& schema) {
    return guarded([&] {
        auto state = local_state();
        auto handle = add_root(state, {0, 0, &schema, schema.hash});
        if (!handle) throw Failure{handle.error()};
        return Binding{std::move(state), *handle, false};
    });
}
Binding field(Binding parent, std::uint64_t index) {
    auto result = child(parent.state, parent.handle, index);
    if (!result) throw AccessError(result.error());
    return {std::move(parent.state), *result, true};
}
Result<Binding> receive_record(std::span<const std::byte> bytes, const Schema& schema) {
    try { return adopt_record(std::vector<std::byte>(bytes.begin(), bytes.end()), schema); }
    catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::ResourceExhausted}); }
}
Result<Binding> adopt_record(std::vector<std::byte> bytes, const Schema& schema) {
    auto result = guarded([&] {
        Image image;
        image.owned = std::move(bytes);
        validate(image);
        require(image.roots.size() == 1, ErrorCode::InvalidData);
        validate_schema(image, image.roots[0], {0, 0, &schema, schema.hash});
        auto state = std::make_shared<State>();
        state->plan.nodes.reserve(image.entries.size());
        const auto table = get(image.bytes(), 40);
        for (std::size_t i = 0; i < image.entries.size(); ++i) {
            const auto& e = image.entries[i];
            Node n; n.alive = true; n.generation = i + 1; n.type = e.type;
            n.size = e.size; n.capacity = e.capacity; n.offset = e.offset; n.descriptor = table + i * descriptor_size;
            if (!primitive(e.type)) for (std::uint64_t j = 0; j < e.size; ++j) {
                const auto id = get(image.bytes(), e.offset + j * 8); n.children.push_back({id, id + 1});
            }
            state->plan.nodes.push_back(std::move(n));
        }
        const Handle handle{image.roots[0], image.roots[0] + 1};
        state->plan.roots.push_back(handle);
        state->plan.next_generation = image.entries.size() + 1;
        state->plan.first_free = image.entries.size();
        state->local = std::move(image.owned);
        return Binding{std::move(state), handle, false};
    });
    if (!result && result.error().code == ErrorCode::SizeOverflow) result.error().code = ErrorCode::InvalidData;
    return result;
}
Result<Binding> copy_image_record(const std::shared_ptr<const Image>& image, std::uint64_t id, Type type) {
    return guarded([&] {
        require(image != nullptr && id < image->entries.size(), ErrorCode::Closed);
        Image inspected;
        inspected.owner = std::shared_ptr<void>(image, const_cast<Image*>(image.get()));
        inspected.borrowed = image->bytes(); inspected.entries = image->entries;
        validate_schema(inspected, id, type);
        State source;
        // Read-only adapter, used solely as clone_tree's source; never returned or mutated.
        source.is_fixed = true;
        source.fixed = {const_cast<std::byte*>(inspected.bytes().data()), inspected.bytes().size()};
        source.plan.nodes.reserve(inspected.entries.size());
        for (std::size_t i = 0; i < inspected.entries.size(); ++i) {
            const auto& e = inspected.entries[i];
            Node n; n.alive = true; n.generation = i + 1; n.type = e.type;
            n.size = e.size; n.capacity = e.capacity; n.offset = e.offset;
            if (!primitive(e.type)) for (std::uint64_t j = 0; j < e.size; ++j) {
                const auto child = get(inspected.bytes(), e.offset + j * 8); n.children.push_back({child, child + 1});
            }
            source.plan.nodes.push_back(std::move(n));
        }
        auto target = local_state(); Plan plan;
        const auto handle = clone_tree(plan, source, {id, id + 1}); plan.roots.push_back(handle);
        rebuild(*target, plan);
        return Binding{std::move(target), handle, false};
    });
}
std::uint64_t epoch(const std::shared_ptr<State>& ptr) {
    if (!ptr || !ptr->active) throw AccessError({ErrorCode::Closed});
    return ptr->epoch;
}
void check_epoch(const std::shared_ptr<State>& ptr, std::uint64_t expected) {
    if (epoch(ptr) != expected) throw AccessError({ErrorCode::StaleHandle});
}
std::uint64_t node_epoch(const std::shared_ptr<State>& ptr, Handle h) {
    return checked(guarded([&] { return node(live(ptr), h).revision; }));
}
void check_node_epoch(const std::shared_ptr<State>& ptr, Handle h, std::uint64_t expected) {
    if (node_epoch(ptr, h) != expected) throw AccessError({ErrorCode::StaleHandle});
}
Result<Snapshot> snapshot(const std::shared_ptr<State>& ptr, std::uint64_t maximum) {
    return guarded([&] { return Snapshot(make_image(live(ptr), maximum)); });
}
Result<std::uint64_t> image_root(const std::shared_ptr<const Image>& image, std::uint64_t index, Type type) {
    return guarded([&] {
        require(image != nullptr, ErrorCode::Closed);
        require(index < image->roots.size(), ErrorCode::OutOfRange);
        const auto id = image->roots[host_size(index)];
        require(same(image->entries[host_size(id)].type, type), ErrorCode::TypeMismatch);
        return id;
    });
}
std::uint64_t image_size(const std::shared_ptr<const Image>& image, std::uint64_t id) {
    if (!image) throw AccessError({ErrorCode::Closed});
    return image->entries.at(host_size(id)).size;
}
std::uint64_t image_child(const std::shared_ptr<const Image>& image, std::uint64_t id, std::uint64_t index) {
    if (!image) throw AccessError({ErrorCode::Closed});
    const auto& value = image->entries.at(host_size(id));
    if (index >= value.size) throw AccessError({ErrorCode::OutOfRange});
    return get(image->bytes(), value.offset + index * 8);
}
std::array<std::byte, 8> image_read(const std::shared_ptr<const Image>& image, std::uint64_t id, std::uint64_t index) {
    if (!image) throw AccessError({ErrorCode::Closed});
    const auto& value = image->entries.at(host_size(id));
    if (index >= value.size) throw AccessError({ErrorCode::OutOfRange});
    std::array<std::byte, 8> bytes{};
    const auto stride = width(value.type);
    std::memcpy(bytes.data(), image->bytes().data() + host_size(value.offset + index * stride), host_size(stride));
    return bytes;
}
Result<Message> FrameworkAccess::borrow(std::span<std::byte> bytes, std::shared_ptr<void> owner, std::uint64_t scratch_limit) {
    return guarded([&] {
        require(owner && bytes.size() >= header_size && scratch_limit >= header_size, ErrorCode::InvalidArgument);
        auto state = std::make_shared<State>();
        state->is_fixed = true; state->fixed = bytes; state->owner = std::move(owner); state->scratch_limit = scratch_limit;
        render_into(bytes.first(header_size), *state, state->plan, layout(state->plan, false), false);
        return Message(std::move(state));
    });
}
Result<Snapshot> FrameworkAccess::seal(Message& message) {
    return seal_binding({message.state_, {}, false});
}
Result<Snapshot> seal_binding(Binding binding) {
    return guarded([&] {
        auto& state = live(binding.state);
        require(state.is_fixed, ErrorCode::InvalidArgument);
        auto image = std::make_shared<Image>();
        image->borrowed = state.fixed.first(host_size(get(state.fixed, 16))); image->owner = state.owner;
        validate(*image); // metadata allocations and validation; no payload copy or movement
        release_message(binding.state);
        return Snapshot(std::move(image));
    });
}
Result<Binding> FrameworkAccess::borrow_record(const Schema& schema, std::span<std::byte> bytes, std::shared_ptr<void> owner, std::uint64_t scratch_limit) {
    return guarded([&] {
        require(owner && bytes.size() >= header_size && scratch_limit >= header_size, ErrorCode::InvalidArgument);
        auto state = std::make_shared<State>();
        state->is_fixed = true; state->fixed = bytes; state->owner = std::move(owner); state->scratch_limit = scratch_limit;
        auto handle = add_root(state, {0, 0, &schema, schema.hash});
        if (!handle) throw Failure{handle.error()};
        return Binding{std::move(state), *handle, false};
    });
}
Result<Snapshot> publish_binding(Binding binding, std::span<std::byte> bytes, std::shared_ptr<void> owner) {
    return guarded([&] {
        auto& state = live(binding.state);
        require(owner && state.plan.roots.size() == 1 && state.plan.roots[0].index == binding.handle.index, ErrorCode::InvalidArgument);
        const auto positions = layout(state.plan, true);
        require(positions.total <= bytes.size(), ErrorCode::CapacityExceeded);
        const auto input = state.bytes();
        const auto in = reinterpret_cast<std::uintptr_t>(input.data()), out = reinterpret_cast<std::uintptr_t>(bytes.data());
        require(out >= in ? out - in >= input.size() : in - out >= bytes.size(), ErrorCode::InvalidArgument);
        auto image = std::make_shared<Image>();
        image->owner = std::move(owner); image->borrowed = bytes.first(host_size(positions.total));
        image->entries.reserve(host_size(positions.count)); image->roots.reserve(state.plan.roots.size());
        for (const auto& root : state.plan.roots) image->roots.push_back(positions.ids[host_size(root.index)]);
        for (std::size_t i = 0; i < state.plan.nodes.size(); ++i) {
            const auto& n = state.plan.nodes[i];
            if (n.alive) image->entries.push_back({n.type, positions.offsets[i], n.size, n.size});
        }
        // All allocations/budget checks precede the first destination write.
        render_into(bytes.first(host_size(positions.total)), state, state.plan, positions, true);
        return Snapshot(std::move(image));
    });
}
void FrameworkAccess::revoke(Message& message) noexcept { release_message(message.state_); }
} // namespace vinc::message::detail

namespace vinc::message {
Message::Message() : state_(detail::local_state()) {}
Message::~Message() { detail::release_message(state_); }
Message::Message(Message&&) noexcept = default;
Message& Message::operator=(Message&& other) noexcept {
    if (this != &other) { detail::release_message(state_); state_ = std::move(other.state_); }
    return *this;
}
std::span<const std::byte> Snapshot::bytes() const noexcept { return image_ ? image_->bytes() : std::span<const std::byte>{}; }
std::uint64_t Snapshot::root_count() const noexcept { return image_ ? image_->roots.size() : 0; }
Result<Snapshot> Snapshot::copy_from(std::span<const std::byte> bytes) {
    auto result = detail::guarded([&] {
        auto image = std::make_shared<detail::Image>();
        image->owned.assign(bytes.begin(), bytes.end());
        detail::validate(*image);
        return Snapshot(std::move(image));
    });
    // Arithmetic overflow while inspecting an external image is invalid input.
    if (!result && result.error().code == ErrorCode::SizeOverflow) result.error().code = ErrorCode::InvalidData;
    return result;
}
} // namespace vinc::message
