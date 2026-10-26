// Mutable message arena and validated immutable images.
// Structural edits build a replacement layout before committing; shared loans enforce one byte budget.

#include <vinc/message/detail/framework_access.hpp>
#include <vinc/message/detail/image_format.hpp>
#include <cstring>
#include <functional>
#include <utility>

namespace vinc::message::detail {
namespace {
// Growth is measured in elements; schema depth bounds recursive cost estimation.
constexpr std::uint64_t minimum_growth_capacity = 4;
constexpr std::uint64_t capacity_growth_factor = 2;
constexpr unsigned maximum_schema_depth = 64;

Result<std::uint64_t> checked_add(std::uint64_t left, std::uint64_t right) {
    if (right > UINT64_MAX - left) {
        return std::unexpected(Error{ErrorCode::SizeOverflow});
    }

    return left + right;
}

Result<std::uint64_t> checked_multiply(std::uint64_t left, std::uint64_t right) {
    if (left != 0 && right > UINT64_MAX / left) {
        return std::unexpected(Error{ErrorCode::SizeOverflow});
    }

    return left * right;
}

Result<std::uint64_t> align_image_offset(std::uint64_t byte_count) {
    auto padded_result = checked_add(byte_count, format::alignment - 1);
    if (!padded_result) {
        return std::unexpected(padded_result.error());
    }

    auto padded = std::move(*padded_result);
    return padded & ~(format::alignment - 1);
}

Result<std::size_t> host_size(std::uint64_t byte_count) {
    if (byte_count > static_cast<std::uint64_t>((std::numeric_limits<std::ptrdiff_t>::max)())) {
        return std::unexpected(Error{ErrorCode::SizeOverflow});
    }

    return static_cast<std::size_t>(byte_count);
}

Result<std::uint64_t> element_width(Type type) {
    if (type.scalar == 0) {
        if (type.hash == 0) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        return format::integer_bytes;
    }

    if (type.depth == 0) {
        return std::unexpected(Error{ErrorCode::TypeMismatch});
    }

    std::uint64_t scalar;
    switch (type.scalar) {
    case 1:
    case 2:
    case 11:
        scalar = 1;
        break;
    case 3:
    case 4:
        scalar = 2;
        break;
    case 5:
    case 6:
    case 9:
        scalar = 4;
        break;
    case 7:
    case 8:
    case 10:
        scalar = 8;
        break;
    default:
        return std::unexpected(Error{ErrorCode::TypeMismatch});
    }

    return type.depth > 1 ? format::integer_bytes : scalar;
}

bool is_scalar_array(Type type) {
    return type.depth == 1 && type.scalar != 0;
}

bool same_type(Type left, Type right) {
    return left.scalar == right.scalar && left.depth == right.depth && left.hash == right.hash;
}

Result<Type> element_type(Type type) {
    if (type.depth == 0) {
        return std::unexpected(Error{ErrorCode::TypeMismatch});
    }

    --type.depth;
    return type;
}

// Only allocation/standard length failures cross an exception boundary. Expected
// storage failures already arrive as Result; forwarding never adds a cause layer.
template <class F>
auto with_allocation_errors(ErrorOperation action, F&& operation) -> std::invoke_result_t<F> {
    try {
        auto result = operation();
        if (!result && result.error().domain == ErrorDomain::MessageStorage &&
            result.error().operation == 0) {
            result.error().operation = static_cast<std::int32_t>(action);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted, action});
    } catch (const std::length_error&) {
        return std::unexpected(Error{ErrorCode::SizeOverflow, action});
    }
}

Result<void> write_integer(std::span<std::byte> bytes, std::uint64_t position, std::uint64_t value,
                           unsigned size = format::integer_bytes) {
    // Every caller writes fixed-width fields into an extent validated before commit.
    if (position > bytes.size() || size > bytes.size() - position) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    auto offset_result = host_size(position);
    if (!offset_result) {
        return std::unexpected(offset_result.error());
    }

    auto offset = std::move(*offset_result);
    for (unsigned i = 0; i < size; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 255);
    }
    return {};
}

Result<std::uint64_t> read_integer(std::span<const std::byte> bytes, std::uint64_t position,
                                   unsigned size = format::integer_bytes) {
    if (position > bytes.size() || size > bytes.size() - position) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    auto offset_result = host_size(position);
    if (!offset_result) {
        return std::unexpected(offset_result.error());
    }

    auto offset = std::move(*offset_result);
    std::uint64_t result = 0;
    for (unsigned i = 0; i < size; ++i) {
        result |= std::uint64_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
    }
    return result;
}

struct Node {
    bool alive = false;
    std::uint64_t generation = 0;
    std::uint64_t revision = 1;
    Type type;
    std::uint64_t size = 0;
    std::uint64_t capacity = 0;
    std::uint64_t offset = 0;
    std::uint64_t descriptor = 0;
    std::vector<Handle> children;
    std::span<const std::byte> imported; // transaction-local; cleared before commit
};

// Editable descriptor tree used to prepare structural changes off to the side.
// The live State is updated only after layout, allocation and rendering all succeed.
struct Plan {
    std::vector<Node> nodes;
    std::vector<Handle> roots;
    std::uint64_t next_generation = 1;
    std::size_t first_free = 0;
};

struct Layout {
    std::uint64_t table = 0;
    std::uint64_t payload = 0;
    std::uint64_t total = 0;
    std::uint64_t count = 0;
    std::vector<std::uint64_t> ids;
    std::vector<std::uint64_t> offsets;
};

// Compute one contiguous image: header, root IDs, descriptors, then aligned payloads.
// Compact images count used elements; construction images also reserve array capacity.
Result<Layout> calculate_layout(const Plan& plan, bool compact) {
    Layout result;
    result.ids.resize(plan.nodes.size(), UINT64_MAX);
    result.offsets.resize(plan.nodes.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        if (plan.nodes[i].alive) {
            result.ids[i] = result.count++;
        }
    }

    auto root_bytes_result = checked_multiply(plan.roots.size(), format::integer_bytes);
    if (!root_bytes_result) {
        return std::unexpected(root_bytes_result.error());
    }

    auto root_bytes = std::move(*root_bytes_result);
    auto header_end_result = checked_add(format::header::size, root_bytes);
    if (!header_end_result) {
        return std::unexpected(header_end_result.error());
    }

    auto header_end = std::move(*header_end_result);
    auto table_result = align_image_offset(header_end);
    if (!table_result) {
        return std::unexpected(table_result.error());
    }

    auto table = std::move(*table_result);
    result.table = table;
    auto descriptor_bytes_result = checked_multiply(result.count, format::descriptor::size);
    if (!descriptor_bytes_result) {
        return std::unexpected(descriptor_bytes_result.error());
    }

    auto descriptor_bytes = std::move(*descriptor_bytes_result);
    auto payload_result = checked_add(table, descriptor_bytes);
    if (!payload_result) {
        return std::unexpected(payload_result.error());
    }

    auto payload = std::move(*payload_result);
    result.payload = payload;
    result.total = payload;
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& value = plan.nodes[i];
        if (!value.alive) {
            continue;
        }
        auto width_result = element_width(value.type);
        if (!width_result) {
            return std::unexpected(width_result.error());
        }

        auto width = std::move(*width_result);
        auto bytes_result = checked_multiply(compact ? value.size : value.capacity, width);
        if (!bytes_result) {
            return std::unexpected(bytes_result.error());
        }

        auto bytes = std::move(*bytes_result);
        if (bytes != 0) {
            result.offsets[i] = result.total;
            auto aligned_result = align_image_offset(bytes);
            if (!aligned_result) {
                return std::unexpected(aligned_result.error());
            }

            auto aligned = std::move(*aligned_result);
            auto end_result = checked_add(result.total, aligned);
            if (!end_result) {
                return std::unexpected(end_result.error());
            }

            auto end = std::move(*end_result);
            result.total = end;
        }
    }
    auto host_size_result = host_size(result.total);
    if (!host_size_result) {
        return std::unexpected(host_size_result.error());
    }

    return result;
}
}

// One mutable arena, either growable local bytes or a fixed framework loan.
// All nested fields share its budget. The owner keeps external storage alive; it is not a lock.
struct State {
    std::vector<std::byte> local;
    std::span<std::byte> fixed;
    std::shared_ptr<void> owner;
    bool is_fixed = false;
    bool active = true;
    std::uint64_t scratch_limit = UINT64_MAX;
    std::uint64_t epoch = 1;
    Plan plan;

    std::span<std::byte> bytes() {
        return is_fixed ? fixed : std::span<std::byte>(local);
    }

    std::uint64_t limit() const {
        return is_fixed ? fixed.size() : UINT64_MAX;
    }
};

// Validated read-only representation. Entries and roots are local lookup metadata;
// the serialized image contains IDs and offsets rather than these vectors or pointers.
struct Image {
    struct Entry {
        Type type;
        std::uint64_t offset;
        std::uint64_t size;
        std::uint64_t capacity;
    };

    std::vector<std::byte> owned;
    std::span<const std::byte> borrowed;
    std::shared_ptr<void> owner;
    std::vector<Entry> entries;
    std::vector<std::uint64_t> roots;

    std::span<const std::byte> bytes() const {
        return owner ? borrowed : std::span<const std::byte>(owned);
    }
};

namespace {
Result<std::reference_wrapper<State>> live(const std::shared_ptr<State>& state) {
    if (!state || !state->active) {
        return std::unexpected(Error{ErrorCode::Closed});
    }

    return std::ref(*state);
}

Result<std::reference_wrapper<const Node>> node(const State& state, Handle handle) {
    if (handle.index >= state.plan.nodes.size()) {
        return std::unexpected(Error{ErrorCode::StaleHandle});
    }

    auto index_result = host_size(handle.index);
    if (!index_result) {
        return std::unexpected(index_result.error());
    }

    auto index = std::move(*index_result);
    const auto& value = state.plan.nodes[index];
    if (!value.alive || value.generation != handle.generation) {
        return std::unexpected(Error{ErrorCode::StaleHandle});
    }

    return std::cref(value);
}

Result<void> before_change(const State& state) {
    if (state.epoch == UINT64_MAX) {
        return std::unexpected(Error{ErrorCode::SizeOverflow});
    }

    return {};
}

Result<Handle> new_node(Plan& plan, Type type) {
    auto element_width_result = element_width(type);
    if (!element_width_result) {
        return std::unexpected(element_width_result.error());
    }

    if (plan.next_generation == UINT64_MAX) {
        return std::unexpected(Error{ErrorCode::SizeOverflow});
    }

    std::size_t index = plan.first_free;
    while (index < plan.nodes.size() && plan.nodes[index].alive) {
        ++index;
    }
    Node value;
    value.alive = true;
    value.generation = plan.next_generation++;
    value.type = type;
    if (index == plan.nodes.size()) {
        plan.nodes.push_back(std::move(value));
    } else {
        plan.nodes[index] = std::move(value);
    }
    plan.first_free = index + 1;
    return Handle{index, plan.nodes[index].generation};
}

Result<Handle> initialized_node(Plan& plan, Type type, bool singular = false) {
    auto root_result = new_node(plan, type);
    if (!root_result) {
        return std::unexpected(root_result.error());
    }

    auto root = std::move(*root_result);
    std::vector<std::pair<Handle, bool>> pending{{root, singular}};
    while (!pending.empty()) {
        const auto [handle, one] = pending.back();
        pending.pop_back();
        auto index_result = host_size(handle.index);
        if (!index_result) {
            return std::unexpected(index_result.error());
        }

        auto index = std::move(*index_result);
        const auto current = plan.nodes[index].type;
        if (current.depth == 0) {
            if (!current.schema || current.schema->hash != current.hash) {
                return std::unexpected(Error{ErrorCode::TypeMismatch});
            }

            for (const auto& field : current.schema->fields) {
                auto child_result = new_node(plan, field.type);
                if (!child_result) {
                    return std::unexpected(child_result.error());
                }

                auto child = std::move(*child_result);
                plan.nodes[index].children.push_back(child);
                pending.emplace_back(child, field.singular);
            }
            auto& descriptor_node = plan.nodes[index];
            descriptor_node.capacity = descriptor_node.children.size();
            descriptor_node.size = descriptor_node.capacity;
        } else if (one) {
            if (!is_scalar_array(current)) {
                return std::unexpected(Error{ErrorCode::TypeMismatch});
            }

            auto& descriptor_node = plan.nodes[index];
            descriptor_node.capacity = 1;
            descriptor_node.size = descriptor_node.capacity;
        }
    }
    return root;
}

Result<std::uint64_t> initial_cost(Type type, bool singular = false, unsigned depth = 0) {
    if (depth > maximum_schema_depth) {
        return std::unexpected(Error{ErrorCode::SizeOverflow});
    }

    if (type.depth != 0) {
        std::uint64_t payload = 0;
        if (singular) {
            auto width_result = element_width(type);
            if (!width_result) {
                return std::unexpected(width_result.error());
            }

            auto width = std::move(*width_result);
            auto aligned_result = align_image_offset(width);
            if (!aligned_result) {
                return std::unexpected(aligned_result.error());
            }

            auto aligned = std::move(*aligned_result);
            payload = aligned;
        }
        return checked_add(format::descriptor::size, payload);
    }

    if (type.schema == nullptr) {
        return std::unexpected(Error{ErrorCode::TypeMismatch});
    }

    auto field_bytes_result = checked_multiply(type.schema->fields.size(), format::integer_bytes);
    if (!field_bytes_result) {
        return std::unexpected(field_bytes_result.error());
    }

    auto field_bytes = std::move(*field_bytes_result);
    auto aligned_result = align_image_offset(field_bytes);
    if (!aligned_result) {
        return std::unexpected(aligned_result.error());
    }

    auto aligned = std::move(*aligned_result);
    auto cost_result = checked_add(format::descriptor::size, aligned);
    if (!cost_result) {
        return std::unexpected(cost_result.error());
    }

    auto cost = std::move(*cost_result);
    for (const auto& field : type.schema->fields) {
        auto child_cost_result = initial_cost(field.type, field.singular, depth + 1);
        if (!child_cost_result) {
            return std::unexpected(child_cost_result.error());
        }

        auto child_cost = std::move(*child_cost_result);
        auto total_result = checked_add(cost, child_cost);
        if (!total_result) {
            return std::unexpected(total_result.error());
        }

        auto total = std::move(*total_result);
        cost = total;
    }
    return cost;
}

Result<void> remove_tree(Plan& plan, Handle root) {
    std::vector<Handle> pending{root};
    while (!pending.empty()) {
        const auto id = pending.back().index;
        pending.pop_back();
        auto index_result = host_size(id);
        if (!index_result) {
            return std::unexpected(index_result.error());
        }

        auto index = std::move(*index_result);
        auto& value = plan.nodes[index];
        pending.insert(pending.end(), value.children.begin(), value.children.end());
        value = Node{};
        plan.first_free = std::min(plan.first_free, index);
    }
    return {};
}

Result<std::span<const std::byte>> source_bytes(State& state, std::size_t index, const Node& target) {
    if (index >= state.plan.nodes.size()) {
        return std::span<const std::byte>{};
    }

    const auto& source = state.plan.nodes[index];
    if (!source.alive || source.generation != target.generation || !is_scalar_array(source.type)) {
        return std::span<const std::byte>{};
    }

    auto width_result = element_width(source.type);
    if (!width_result) {
        return std::unexpected(width_result.error());
    }

    auto width = std::move(*width_result);
    auto size_result = checked_multiply(std::min(source.size, target.size), width);
    if (!size_result) {
        return std::unexpected(size_result.error());
    }

    auto size = std::move(*size_result);
    if (size == 0) {
        return std::span<const std::byte>{};
    }

    auto offset_result = host_size(source.offset);
    if (!offset_result) {
        return std::unexpected(offset_result.error());
    }

    auto offset = std::move(*offset_result);
    auto length_result = host_size(size);
    if (!length_result) {
        return std::unexpected(length_result.error());
    }

    auto length = std::move(*length_result);
    return state.bytes().subspan(offset, length);
}

// Write the portable image explicitly. Child references become descriptor IDs,
// reserved bytes are zeroed, and scalar payloads are copied from the previous layout/imports.
// The destination has already been zeroed, including reserved header bytes.
Result<void> write_image_header(std::span<std::byte> bytes, const Plan& plan, const Layout& positions,
                                bool compact) {
    auto magic_write = write_integer(bytes, format::header::magic, format::magic);
    if (!magic_write) {
        return std::unexpected(magic_write.error());
    }

    auto version_write = write_integer(bytes, format::header::version, format::version, format::tag_bytes);
    if (!version_write) {
        return std::unexpected(version_write.error());
    }

    auto flags_write =
        write_integer(bytes, format::header::flags,
                      compact ? format::compact_flag : format::reserved_capacity_flag, format::tag_bytes);
    if (!flags_write) {
        return std::unexpected(flags_write.error());
    }

    auto total_bytes_write = write_integer(bytes, format::header::total_bytes, positions.total);
    if (!total_bytes_write) {
        return std::unexpected(total_bytes_write.error());
    }

    auto node_count_write = write_integer(bytes, format::header::node_count, positions.count);
    if (!node_count_write) {
        return std::unexpected(node_count_write.error());
    }

    auto root_count_write = write_integer(bytes, format::header::root_count, plan.roots.size());
    if (!root_count_write) {
        return std::unexpected(root_count_write.error());
    }

    auto descriptor_table_write = write_integer(bytes, format::header::descriptor_table, positions.table);
    if (!descriptor_table_write) {
        return std::unexpected(descriptor_table_write.error());
    }

    auto payload_write = write_integer(bytes, format::header::payload, positions.payload);
    if (!payload_write) {
        return std::unexpected(payload_write.error());
    }

    for (std::size_t i = 0; i < plan.roots.size(); ++i) {
        auto index_result = host_size(plan.roots[i].index);
        if (!index_result) {
            return std::unexpected(index_result.error());
        }

        auto index = std::move(*index_result);
        auto root_id_write =
            write_integer(bytes, format::header::size + i * format::integer_bytes, positions.ids[index]);
        if (!root_id_write) {
            return std::unexpected(root_id_write.error());
        }
    }
    return {};
}

Result<void> render_into(std::span<std::byte> bytes, State& state, const Plan& plan, const Layout& positions,
                         bool compact) {
    std::fill(bytes.begin(), bytes.end(), std::byte{0});
    auto write_image_header_result = write_image_header(bytes, plan, positions, compact);
    if (!write_image_header_result) {
        return std::unexpected(write_image_header_result.error());
    }

    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& value = plan.nodes[i];
        if (!value.alive) {
            continue;
        }
        const auto offset = positions.offsets[i];
        const auto descriptor = positions.table + positions.ids[i] * format::descriptor::size;
        const auto stored_capacity = compact ? value.size : value.capacity;
        auto payload_write =
            write_integer(bytes, descriptor + format::descriptor::payload, stored_capacity ? offset : 0);
        if (!payload_write) {
            return std::unexpected(payload_write.error());
        }

        auto element_count_write =
            write_integer(bytes, descriptor + format::descriptor::element_count, value.size);
        if (!element_count_write) {
            return std::unexpected(element_count_write.error());
        }

        auto scalar_write = write_integer(bytes, descriptor + format::descriptor::scalar, value.type.scalar,
                                          format::tag_bytes);
        if (!scalar_write) {
            return std::unexpected(scalar_write.error());
        }

        auto depth_write =
            write_integer(bytes, descriptor + format::descriptor::depth, value.type.depth, format::tag_bytes);
        if (!depth_write) {
            return std::unexpected(depth_write.error());
        }

        auto capacity_write =
            write_integer(bytes, descriptor + format::descriptor::capacity, stored_capacity);
        if (!capacity_write) {
            return std::unexpected(capacity_write.error());
        }

        auto schema_hash_write =
            write_integer(bytes, descriptor + format::descriptor::schema_hash, value.type.hash);
        if (!schema_hash_write) {
            return std::unexpected(schema_hash_write.error());
        }

        if (!is_scalar_array(value.type)) {
            for (std::size_t j = 0; j < value.children.size(); ++j) {
                auto index_result = host_size(value.children[j].index);
                if (!index_result) {
                    return std::unexpected(index_result.error());
                }

                auto index = std::move(*index_result);
                auto child_id_write =
                    write_integer(bytes, offset + j * format::integer_bytes, positions.ids[index]);
                if (!child_id_write) {
                    return std::unexpected(child_id_write.error());
                }
            }
        } else {
            auto source_result = source_bytes(state, i, value);
            if (!source_result) {
                return std::unexpected(source_result.error());
            }

            auto source = std::move(*source_result);
            if (!value.imported.empty()) {
                source = value.imported;
            }
            auto width_result = element_width(value.type);
            if (!width_result) {
                return std::unexpected(width_result.error());
            }

            auto width = std::move(*width_result);
            auto length_result = checked_multiply(value.size, width);
            if (!length_result) {
                return std::unexpected(length_result.error());
            }

            auto length = std::move(*length_result);
            if (source.size() > length) {
                return std::unexpected(Error{ErrorCode::InvalidArgument});
            }

            if (!source.empty()) {
                auto destination_result = host_size(offset);
                if (!destination_result) {
                    return std::unexpected(destination_result.error());
                }

                auto destination = std::move(*destination_result);
                std::memcpy(bytes.data() + destination, source.data(), source.size());
            }
        }
    }
    return {};
}

Result<std::vector<std::byte>> render(State& state, const Plan& plan, const Layout& positions, bool compact) {
    if (positions.total > state.scratch_limit) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted});
    }

    auto size_result = host_size(positions.total);
    if (!size_result) {
        return std::unexpected(size_result.error());
    }

    auto size = std::move(*size_result);
    std::vector<std::byte> bytes(size);
    auto render_into_result = render_into(bytes, state, plan, positions, compact);
    if (!render_into_result) {
        return std::unexpected(render_into_result.error());
    }

    return bytes;
}

// Updates only the staged tree. Imported spans remain valid until rendering has finished.
void prepare_committed_plan(const State& state, Plan& plan, const Layout& positions) {
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        auto& next = plan.nodes[i];
        if (next.alive && i < state.plan.nodes.size()) {
            const auto& old = state.plan.nodes[i];
            if (old.alive && old.generation == next.generation &&
                (old.size != next.size || old.capacity != next.capacity || old.children != next.children ||
                 !next.imported.empty())) {
                next.revision = old.revision + 1;
            }
        }

        plan.nodes[i].offset = positions.offsets[i];
        if (plan.nodes[i].alive) {
            plan.nodes[i].descriptor = positions.table + positions.ids[i] * format::descriptor::size;
        }

        plan.nodes[i].imported = {};
    }
}

// Strong failure boundary for structural edits: render to temporary storage first.
// A capacity/allocation failure leaves the old tree and payload available to the caller.
Result<void> rebuild(State& state, Plan& plan) {
    auto before_change_result = before_change(state);
    if (!before_change_result) {
        return std::unexpected(before_change_result.error());
    }

    auto positions_result = calculate_layout(plan, false);
    if (!positions_result) {
        return std::unexpected(positions_result.error());
    }

    auto positions = std::move(*positions_result);
    if (positions.total > state.limit()) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded});
    }

    auto bytes_result = render(state, plan, positions, false);
    if (!bytes_result) {
        return std::unexpected(bytes_result.error());
    }

    auto bytes = std::move(*bytes_result);
    prepare_committed_plan(state, plan, positions);

    // Commit begins only after every fallible check, allocation and render has succeeded.
    if (state.is_fixed) {
        std::memcpy(state.fixed.data(), bytes.data(), bytes.size());
    } else {
        state.local.swap(bytes);
    }
    state.plan.nodes.swap(plan.nodes);
    state.plan.roots.swap(plan.roots);
    state.plan.next_generation = plan.next_generation;
    state.plan.first_free = plan.first_free;
    ++state.epoch;
    return {};
}

std::uint64_t preferred_capacity(std::uint64_t previous, std::uint64_t needed) {
    if (previous >= needed) {
        return previous;
    }

    const auto growth = previous > UINT64_MAX / capacity_growth_factor
                            ? needed
                            : std::max(minimum_growth_capacity, previous * capacity_growth_factor);
    return std::max(needed, growth);
}

// Prefer geometric capacity growth, but retry with exactly the requested size when
// spare capacity alone would exceed the byte budget. A valid small edit should still fit.
Result<void> rebuild_with_growth(State& state, Plan& plan, std::uint64_t id, std::uint64_t minimum) {
    auto index_result = host_size(id);
    if (!index_result) {
        return std::unexpected(index_result.error());
    }

    auto index = std::move(*index_result);
    const auto old = plan.nodes[index].capacity;
    plan.nodes[index].capacity = preferred_capacity(old, minimum);
    auto grown = rebuild(state, plan);
    if (grown) {
        return {};
    }

    // Only explicit capacity/size/scratch-budget failures retry at the exact size.
    // Allocation exceptions still go to the enclosing API boundary, as before.
    const auto& error = grown.error();
    const bool retryable = error.is(ErrorCode::CapacityExceeded) || error.is(ErrorCode::ResourceExhausted) ||
                           error.is(ErrorCode::SizeOverflow);
    if (!retryable || plan.nodes[index].capacity == minimum) {
        return std::unexpected(error);
    }

    plan.nodes[index].capacity = minimum;
    return rebuild(state, plan);
}

// Imported spans borrow source payload until rebuild renders them. Keep the source alive
// and unchanged across this call and rebuild, including same-arena copies.
Result<Handle> clone_tree(Plan& plan, State& source, Handle root) {
    auto first_ref_result = node(source, root);
    if (!first_ref_result) {
        return std::unexpected(first_ref_result.error());
    }

    auto first_ref = std::move(*first_ref_result);
    auto root_result = new_node(plan, first_ref.get().type);
    if (!root_result) {
        return std::unexpected(root_result.error());
    }

    auto result = std::move(*root_result);
    std::vector<std::pair<Handle, Handle>> pending{{root, result}};
    while (!pending.empty()) {
        const auto [from, to] = pending.back();
        pending.pop_back();
        auto original_ref_result = node(source, from);
        if (!original_ref_result) {
            return std::unexpected(original_ref_result.error());
        }

        auto original_ref = std::move(*original_ref_result);
        const auto& original = original_ref.get();
        auto index_result = host_size(to.index);
        if (!index_result) {
            return std::unexpected(index_result.error());
        }

        auto index = std::move(*index_result);
        plan.nodes[index].size = original.size;
        plan.nodes[index].capacity = original.size;
        if (is_scalar_array(original.type)) {
            auto width_result = element_width(original.type);
            if (!width_result) {
                return std::unexpected(width_result.error());
            }

            auto width = std::move(*width_result);
            auto count_result = checked_multiply(original.size, width);
            if (!count_result) {
                return std::unexpected(count_result.error());
            }

            auto count = std::move(*count_result);
            if (count != 0) {
                auto offset_result = host_size(original.offset);
                if (!offset_result) {
                    return std::unexpected(offset_result.error());
                }

                auto offset = std::move(*offset_result);
                auto length_result = host_size(count);
                if (!length_result) {
                    return std::unexpected(length_result.error());
                }

                auto length = std::move(*length_result);
                plan.nodes[index].imported = source.bytes().subspan(offset, length);
            }
        } else {
            plan.nodes[index].children.reserve(original.children.size());
            for (auto child : original.children) {
                auto child_ref_result = node(source, child);
                if (!child_ref_result) {
                    return std::unexpected(child_ref_result.error());
                }

                auto child_ref = std::move(*child_ref_result);
                auto replacement_result = new_node(plan, child_ref.get().type);
                if (!replacement_result) {
                    return std::unexpected(replacement_result.error());
                }

                auto replacement = std::move(*replacement_result);
                plan.nodes[index].children.push_back(replacement);
                pending.emplace_back(child, replacement);
            }
        }
    }
    return result;
}

// Treat incoming bytes as untrusted layout data. Check extents, padding, element types
// and unique tree ownership before allowing any descriptor-based payload access.
// Root ownership is claimed before child links, preserving duplicate-ID rejection order.
Result<void> read_image_roots(Image& image, std::uint64_t roots, std::uint64_t count,
                              std::vector<bool>& owned) {
    const auto bytes = image.bytes();
    for (std::uint64_t i = 0; i < roots; ++i) {
        auto id_result = read_integer(bytes, format::header::size + i * format::integer_bytes);
        if (!id_result) {
            return std::unexpected(id_result.error());
        }

        auto id = std::move(*id_result);
        if (id >= count) {
            return std::unexpected(Error{ErrorCode::InvalidData});
        }

        auto index_result = host_size(id);
        if (!index_result) {
            return std::unexpected(index_result.error());
        }

        auto index = std::move(*index_result);
        if (owned[index]) {
            return std::unexpected(Error{ErrorCode::InvalidData});
        }

        owned[index] = true;
        image.roots.push_back(id);
    }
    return {};
}

Result<void> read_image_descriptors(Image& image, std::uint64_t count, std::uint64_t table,
                                    std::uint64_t payload) {
    const auto bytes = image.bytes();
    auto cursor = payload;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto at = table + i * format::descriptor::size;
        auto offset_result = read_integer(bytes, at + format::descriptor::payload);
        if (!offset_result) {
            return std::unexpected(offset_result.error());
        }

        auto offset = std::move(*offset_result);
        auto size_result = read_integer(bytes, at + format::descriptor::element_count);
        if (!size_result) {
            return std::unexpected(size_result.error());
        }

        auto size = std::move(*size_result);
        auto scalar_result = read_integer(bytes, at + format::descriptor::scalar, format::tag_bytes);
        if (!scalar_result) {
            return std::unexpected(scalar_result.error());
        }

        auto scalar = std::move(*scalar_result);
        auto depth_result = read_integer(bytes, at + format::descriptor::depth, format::tag_bytes);
        if (!depth_result) {
            return std::unexpected(depth_result.error());
        }

        auto depth = std::move(*depth_result);
        Type type{static_cast<std::uint32_t>(scalar), static_cast<std::uint32_t>(depth)};
        auto capacity_result = read_integer(bytes, at + format::descriptor::capacity);
        if (!capacity_result) {
            return std::unexpected(capacity_result.error());
        }

        auto capacity = std::move(*capacity_result);
        auto hash_result = read_integer(bytes, at + format::descriptor::schema_hash);
        if (!hash_result) {
            return std::unexpected(hash_result.error());
        }

        auto hash = std::move(*hash_result);
        type.hash = hash;
        if (!(type.scalar <= format::boolean_scalar_code &&
              (type.scalar ? type.depth > 0 && type.hash == 0 : type.hash != 0))) {
            return std::unexpected(Error{ErrorCode::InvalidData});
        }

        auto reserved_result = read_integer(bytes, at + format::descriptor::reserved);
        if (!reserved_result) {
            return std::unexpected(reserved_result.error());
        }

        auto reserved = std::move(*reserved_result);
        if (reserved != 0) {
            return std::unexpected(Error{ErrorCode::InvalidData});
        }

        if (size > capacity) {
            return std::unexpected(Error{ErrorCode::InvalidData});
        }

        auto flags_result = read_integer(bytes, format::header::flags, format::tag_bytes);
        if (!flags_result) {
            return std::unexpected(flags_result.error());
        }

        auto flags = std::move(*flags_result);
        if (flags == format::compact_flag && size != capacity) {
            return std::unexpected(Error{ErrorCode::InvalidData});
        }

        if (type.depth == 0 && size != capacity) {
            return std::unexpected(Error{ErrorCode::InvalidData});
        }

        auto width_result = element_width(type);
        if (!width_result) {
            return std::unexpected(width_result.error());
        }

        auto width = std::move(*width_result);
        auto length_result = checked_multiply(capacity, width);
        if (!length_result) {
            return std::unexpected(length_result.error());
        }

        auto length = std::move(*length_result);
        auto used_result = checked_multiply(size, width);
        if (!used_result) {
            return std::unexpected(used_result.error());
        }

        auto used = std::move(*used_result);
        if (offset != (capacity != 0 ? cursor : 0)) {
            return std::unexpected(Error{ErrorCode::InvalidData});
        }

        if (length != 0) {
            if (cursor > bytes.size() || length > bytes.size() - cursor) {
                return std::unexpected(Error{ErrorCode::InvalidData});
            }

            auto aligned_result = align_image_offset(length);
            if (!aligned_result) {
                return std::unexpected(aligned_result.error());
            }

            auto aligned = std::move(*aligned_result);
            auto end_result = checked_add(cursor, aligned);
            if (!end_result) {
                return std::unexpected(end_result.error());
            }

            auto end = std::move(*end_result);
            if (end > bytes.size()) {
                return std::unexpected(Error{ErrorCode::InvalidData});
            }

            for (auto padding = cursor + used; padding < end; ++padding) {
                auto index_result = host_size(padding);
                if (!index_result) {
                    return std::unexpected(index_result.error());
                }

                auto index = std::move(*index_result);
                if (bytes[index] != std::byte{0}) {
                    return std::unexpected(Error{ErrorCode::InvalidData});
                }
            }
            cursor = end;
        }
        if (type.depth == 1 && type.scalar == format::boolean_scalar_code) {
            for (std::uint64_t j = 0; j < size; ++j) {
                auto index_result = host_size(offset + j);
                if (!index_result) {
                    return std::unexpected(index_result.error());
                }

                auto index = std::move(*index_result);
                if (std::to_integer<unsigned>(bytes[index]) > 1) {
                    return std::unexpected(Error{ErrorCode::InvalidData});
                }
            }
        }
        image.entries.push_back({type, offset, size, capacity});
    }
    if (cursor != bytes.size()) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    return {};
}

Result<void> validate_child_ownership(const Image& image, std::uint64_t count, std::vector<bool>& owned) {
    const auto bytes = image.bytes();
    for (const auto& value : image.entries) {
        if (is_scalar_array(value.type)) {
            continue;
        }
        for (std::uint64_t j = 0; j < value.size; ++j) {
            auto id_result = read_integer(bytes, value.offset + j * format::integer_bytes);
            if (!id_result) {
                return std::unexpected(id_result.error());
            }

            auto id = std::move(*id_result);
            if (id >= count) {
                return std::unexpected(Error{ErrorCode::InvalidData});
            }

            auto index_result = host_size(id);
            if (!index_result) {
                return std::unexpected(index_result.error());
            }

            auto index = std::move(*index_result);
            if (owned[index]) {
                return std::unexpected(Error{ErrorCode::InvalidData});
            }

            const auto child_type = image.entries[index].type;
            if (value.type.depth != 0) {
                auto expected_type_result = element_type(value.type);
                if (!expected_type_result) {
                    return std::unexpected(expected_type_result.error());
                }

                auto expected_type = std::move(*expected_type_result);
                if (!same_type(child_type, expected_type)) {
                    return std::unexpected(Error{ErrorCode::InvalidData});
                }
            }
            owned[index] = true;
        }
    }
    return {};
}

Result<void> validate_reachability(const Image& image, std::uint64_t count, const std::vector<bool>& owned) {
    const auto bytes = image.bytes();
    const bool all_owned = std::all_of(owned.begin(), owned.end(), [](bool value) {
        return value;
    });
    if (!all_owned) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    auto pending = image.roots;
    std::uint64_t reached = 0;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        ++reached;
        if (reached > count) {
            return std::unexpected(Error{ErrorCode::InvalidData});
        }

        auto index_result = host_size(id);
        if (!index_result) {
            return std::unexpected(index_result.error());
        }

        auto index = std::move(*index_result);
        const auto& entry = image.entries[index];
        if (!is_scalar_array(entry.type)) {
            for (std::uint64_t i = 0; i < entry.size; ++i) {
                auto child_result = read_integer(bytes, entry.offset + i * format::integer_bytes);
                if (!child_result) {
                    return std::unexpected(child_result.error());
                }

                auto child = std::move(*child_result);
                pending.push_back(child);
            }
        }
    }
    if (reached != count) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    return {};
}

Result<void> validate_image_header(std::span<const std::byte> bytes) {
    // Bounds and each format check precede all later field reads.
    if (bytes.size() < format::header::size) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    auto magic_result = read_integer(bytes, format::header::magic);
    if (!magic_result) {
        return std::unexpected(magic_result.error());
    }

    auto magic = std::move(*magic_result);
    if (magic != format::magic) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    auto version_result = read_integer(bytes, format::header::version, format::tag_bytes);
    if (!version_result) {
        return std::unexpected(version_result.error());
    }

    auto version = std::move(*version_result);
    if (version != format::version) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    auto flags_result = read_integer(bytes, format::header::flags, format::tag_bytes);
    if (!flags_result) {
        return std::unexpected(flags_result.error());
    }

    auto flags = std::move(*flags_result);
    if (flags != format::compact_flag && flags != format::reserved_capacity_flag) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    auto total_result = read_integer(bytes, format::header::total_bytes);
    if (!total_result) {
        return std::unexpected(total_result.error());
    }

    auto total = std::move(*total_result);
    if (total != bytes.size()) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    auto reserved_result = read_integer(bytes, format::header::reserved);
    if (!reserved_result) {
        return std::unexpected(reserved_result.error());
    }

    auto reserved = std::move(*reserved_result);
    if (reserved != 0) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    return {};
}

Result<void> validate(Image& image) {
    const auto bytes = image.bytes();
    auto validate_image_header_result = validate_image_header(bytes);
    if (!validate_image_header_result) {
        return std::unexpected(validate_image_header_result.error());
    }

    auto count_result = read_integer(bytes, format::header::node_count);
    if (!count_result) {
        return std::unexpected(count_result.error());
    }

    auto count = std::move(*count_result);
    auto roots_result = read_integer(bytes, format::header::root_count);
    if (!roots_result) {
        return std::unexpected(roots_result.error());
    }

    auto roots = std::move(*roots_result);
    auto table_result = read_integer(bytes, format::header::descriptor_table);
    if (!table_result) {
        return std::unexpected(table_result.error());
    }

    auto table = std::move(*table_result);
    auto payload_result = read_integer(bytes, format::header::payload);
    if (!payload_result) {
        return std::unexpected(payload_result.error());
    }

    auto payload = std::move(*payload_result);
    if (roots > count || count > bytes.size() / format::descriptor::size) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    auto root_bytes_result = checked_multiply(roots, format::integer_bytes);
    if (!root_bytes_result) {
        return std::unexpected(root_bytes_result.error());
    }

    auto root_bytes = std::move(*root_bytes_result);
    auto root_end_result = checked_add(format::header::size, root_bytes);
    if (!root_end_result) {
        return std::unexpected(root_end_result.error());
    }

    auto root_end = std::move(*root_end_result);
    auto expected_table_result = align_image_offset(root_end);
    if (!expected_table_result) {
        return std::unexpected(expected_table_result.error());
    }

    auto expected_table = std::move(*expected_table_result);
    if (table != expected_table) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    auto descriptor_bytes_result = checked_multiply(count, format::descriptor::size);
    if (!descriptor_bytes_result) {
        return std::unexpected(descriptor_bytes_result.error());
    }

    auto descriptor_bytes = std::move(*descriptor_bytes_result);
    auto expected_payload_result = checked_add(table, descriptor_bytes);
    if (!expected_payload_result) {
        return std::unexpected(expected_payload_result.error());
    }

    auto expected_payload = std::move(*expected_payload_result);
    if (payload != expected_payload || payload > bytes.size()) {
        return std::unexpected(Error{ErrorCode::InvalidData});
    }

    auto node_count_result = host_size(count);
    if (!node_count_result) {
        return std::unexpected(node_count_result.error());
    }

    auto node_count = std::move(*node_count_result);
    auto root_count_result = host_size(roots);
    if (!root_count_result) {
        return std::unexpected(root_count_result.error());
    }

    auto root_count = std::move(*root_count_result);
    image.entries.reserve(node_count);
    image.roots.reserve(root_count);
    std::vector<bool> owned(node_count, false);
    auto read_image_roots_result = read_image_roots(image, roots, count, owned);
    if (!read_image_roots_result) {
        return std::unexpected(read_image_roots_result.error());
    }

    auto read_image_descriptors_result = read_image_descriptors(image, count, table, payload);
    if (!read_image_descriptors_result) {
        return std::unexpected(read_image_descriptors_result.error());
    }

    auto validate_child_ownership_result = validate_child_ownership(image, count, owned);
    if (!validate_child_ownership_result) {
        return std::unexpected(validate_child_ownership_result.error());
    }

    return validate_reachability(image, count, owned);
}

// Layout validation alone is insufficient: match every nested field to the expected
// generated schema before exposing a typed message facade.
// Mutable receive adapters also bind local schema pointers. Immutable snapshots
// run the identical checks without annotating entries or installing a shared cache.
template <class ImageType>
Result<void> validate_schema(ImageType& image, std::uint64_t id, Type expected) {
    static_assert(std::is_same_v<std::remove_const_t<ImageType>, Image>);
    struct Check {
        std::uint64_t id;
        Type type;
        bool singular;
    };

    std::vector<Check> pending{{id, expected, false}};
    while (!pending.empty()) {
        const auto check = pending.back();
        pending.pop_back();
        auto index_result = host_size(check.id);
        if (!index_result) {
            return std::unexpected(index_result.error());
        }

        auto index = std::move(*index_result);
        auto& value = image.entries[index];
        if (!same_type(value.type, check.type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        if constexpr (!std::is_const_v<ImageType>) {
            value.type.schema = check.type.schema;
        }
        if (check.singular) {
            if (value.size != 1) {
                return std::unexpected(Error{ErrorCode::InvalidData});
            }
        }
        if (value.type.depth == 0) {
            if (!check.type.schema || value.size != check.type.schema->fields.size()) {
                return std::unexpected(Error{ErrorCode::InvalidData});
            }

            for (std::uint64_t i = 0; i < value.size; ++i) {
                auto field_index_result = host_size(i);
                if (!field_index_result) {
                    return std::unexpected(field_index_result.error());
                }

                auto field_index = std::move(*field_index_result);
                const auto& field = check.type.schema->fields[field_index];
                auto child_result = read_integer(image.bytes(), value.offset + i * format::integer_bytes);
                if (!child_result) {
                    return std::unexpected(child_result.error());
                }

                auto child = std::move(*child_result);
                pending.push_back({child, field.type, field.singular});
            }
        } else if (!is_scalar_array(value.type)) {
            for (std::uint64_t i = 0; i < value.size; ++i) {
                auto child_result = read_integer(image.bytes(), value.offset + i * format::integer_bytes);
                if (!child_result) {
                    return std::unexpected(child_result.error());
                }

                auto child = std::move(*child_result);
                auto child_type_result = element_type(check.type);
                if (!child_type_result) {
                    return std::unexpected(child_type_result.error());
                }

                auto child_type = std::move(*child_type_result);
                pending.push_back({child, child_type, false});
            }
        }
    }
    return {};
}

Result<std::shared_ptr<Image>> make_image(State& state, std::uint64_t maximum) {
    auto positions_result = calculate_layout(state.plan, true);
    if (!positions_result) {
        return std::unexpected(positions_result.error());
    }

    auto positions = std::move(*positions_result);
    if (positions.total > maximum) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded});
    }

    auto image = std::make_shared<Image>();
    auto bytes_result = render(state, state.plan, positions, true);
    if (!bytes_result) {
        return std::unexpected(bytes_result.error());
    }

    auto bytes = std::move(*bytes_result);
    image->owned = std::move(bytes);
    auto validate_result = validate(*image);
    if (!validate_result) {
        return std::unexpected(validate_result.error());
    }

    return image;
}
}

Result<std::shared_ptr<State>> create_local_state() {
    auto state = std::make_shared<State>();
    auto positions_result = calculate_layout(state->plan, false);
    if (!positions_result) {
        return std::unexpected(positions_result.error());
    }

    auto positions = std::move(*positions_result);
    auto bytes_result = render(*state, state->plan, positions, false);
    if (!bytes_result) {
        return std::unexpected(bytes_result.error());
    }

    auto bytes = std::move(*bytes_result);
    state->local = std::move(bytes);
    return state;
}

// Owning constructors preserve native allocation exceptions. Empty local-state
// layout cannot normally fail; a failed invariant uses the contract handler.
// Recoverable Result entry points call create_local_state inside their own boundary.
std::shared_ptr<State> local_state() {
    return contract_value(create_local_state());
}

void release_message(const std::shared_ptr<State>& state) noexcept {
    if (state && state->is_fixed) {
        state->active = false;
        state->fixed = {};
        state->owner.reset();
    }
}

Result<Handle> add_root_core(const std::shared_ptr<State>& state_ptr, Type type) {
    auto state_ref_result = live(state_ptr);
    if (!state_ref_result) {
        return std::unexpected(state_ref_result.error());
    }

    auto state_ref = std::move(*state_ref_result);
    auto& state = state_ref.get();
    auto cost_result = initial_cost(type);
    if (!cost_result) {
        return std::unexpected(cost_result.error());
    }

    auto cost = std::move(*cost_result);
    auto minimum_result = checked_add(format::header::size + format::integer_bytes, cost);
    if (!minimum_result) {
        return std::unexpected(minimum_result.error());
    }

    auto minimum = std::move(*minimum_result);
    if (minimum > state.limit()) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded});
    }

    Plan plan = state.plan;
    auto handle_result = initialized_node(plan, type);
    if (!handle_result) {
        return std::unexpected(handle_result.error());
    }

    auto handle = std::move(*handle_result);
    plan.roots.push_back(handle);
    auto rebuild_result = rebuild(state, plan);
    if (!rebuild_result) {
        return std::unexpected(rebuild_result.error());
    }

    return handle;
}

Result<Handle> add_root(const std::shared_ptr<State>& state_ptr, Type type) {
    return with_allocation_errors(ErrorOperation::AddRoot, [&]() -> Result<Handle> {
        return add_root_core(state_ptr, type);
    });
}

Result<Handle> root(const std::shared_ptr<State>& state_ptr, std::uint64_t index, Type type) {
    return with_allocation_errors(ErrorOperation::AccessRoot, [&]() -> Result<Handle> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        if (index >= state.plan.roots.size()) {
            return std::unexpected(Error{ErrorCode::OutOfRange});
        }

        auto position_result = host_size(index);
        if (!position_result) {
            return std::unexpected(position_result.error());
        }

        auto position = std::move(*position_result);
        auto handle = state.plan.roots[position];
        auto value_ref_result = node(state, handle);
        if (!value_ref_result) {
            return std::unexpected(value_ref_result.error());
        }

        auto value_ref = std::move(*value_ref_result);
        if (!same_type(value_ref.get().type, type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        return handle;
    });
}

Result<Info> info(const std::shared_ptr<State>& state_ptr, Handle handle) {
    return with_allocation_errors(ErrorOperation::Inspect, [&]() -> Result<Info> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto value_ref_result = node(state_ref.get(), handle);
        if (!value_ref_result) {
            return std::unexpected(value_ref_result.error());
        }

        auto value_ref = std::move(*value_ref_result);
        const auto& value = value_ref.get();
        return Info{value.size, value.capacity};
    });
}

Result<Handle> child(const std::shared_ptr<State>& state_ptr, Handle handle, std::uint64_t index) {
    return with_allocation_errors(ErrorOperation::AccessChild, [&]() -> Result<Handle> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto value_ref_result = node(state_ref.get(), handle);
        if (!value_ref_result) {
            return std::unexpected(value_ref_result.error());
        }

        auto value_ref = std::move(*value_ref_result);
        const auto& value = value_ref.get();
        if (is_scalar_array(value.type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        if (index >= value.size) {
            return std::unexpected(Error{ErrorCode::OutOfRange});
        }

        auto position_result = host_size(index);
        if (!position_result) {
            return std::unexpected(position_result.error());
        }

        auto position = std::move(*position_result);
        return value.children[position];
    });
}

Result<std::array<std::byte, 8>> read(const std::shared_ptr<State>& state_ptr, Handle handle,
                                      std::uint64_t index) {
    return with_allocation_errors(ErrorOperation::Read, [&]() -> Result<std::array<std::byte, 8>> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        auto value_ref_result = node(state, handle);
        if (!value_ref_result) {
            return std::unexpected(value_ref_result.error());
        }

        auto value_ref = std::move(*value_ref_result);
        const auto& value = value_ref.get();
        if (!is_scalar_array(value.type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        if (index >= value.size) {
            return std::unexpected(Error{ErrorCode::OutOfRange});
        }

        auto count_result = element_width(value.type);
        if (!count_result) {
            return std::unexpected(count_result.error());
        }

        auto count = std::move(*count_result);
        auto offset_result = host_size(value.offset + index * count);
        if (!offset_result) {
            return std::unexpected(offset_result.error());
        }

        auto offset = std::move(*offset_result);
        auto length_result = host_size(count);
        if (!length_result) {
            return std::unexpected(length_result.error());
        }

        auto length = std::move(*length_result);
        std::array<std::byte, 8> output{};
        std::memcpy(output.data(), state.bytes().data() + offset, length);
        return output;
    });
}

Result<void> write(const std::shared_ptr<State>& state_ptr, Handle handle, std::uint64_t index,
                   std::span<const std::byte> bytes) {
    return with_allocation_errors(ErrorOperation::Write, [&]() -> Result<void> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        auto value_ref_result = node(state, handle);
        if (!value_ref_result) {
            return std::unexpected(value_ref_result.error());
        }

        auto value_ref = std::move(*value_ref_result);
        const auto& value = value_ref.get();
        if (!is_scalar_array(value.type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        auto width_result = element_width(value.type);
        if (!width_result) {
            return std::unexpected(width_result.error());
        }

        auto width = std::move(*width_result);
        if (bytes.size() != width) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        if (index >= value.size) {
            return std::unexpected(Error{ErrorCode::OutOfRange});
        }

        auto offset_result = host_size(value.offset + index * bytes.size());
        if (!offset_result) {
            return std::unexpected(offset_result.error());
        }

        auto offset = std::move(*offset_result);
        std::memcpy(state.bytes().data() + offset, bytes.data(), bytes.size());
        return {};
    });
}

namespace {
// Caller has checked the epoch and byte count. This path must stay allocation-free.
Result<void> resize_scalar_in_place(State& state, Handle handle, const Node& original, std::uint64_t size,
                                    std::uint64_t stride) {
    const auto first_changed = std::min(size, original.size);
    const auto changed_count = std::max(size, original.size) - first_changed;
    auto offset_result = host_size(original.offset + first_changed * stride);
    if (!offset_result) {
        return std::unexpected(offset_result.error());
    }

    auto offset = std::move(*offset_result);
    auto length_result = host_size(changed_count * stride);
    if (!length_result) {
        return std::unexpected(length_result.error());
    }

    auto length = std::move(*length_result);
    auto position_result = host_size(handle.index);
    if (!position_result) {
        return std::unexpected(position_result.error());
    }

    auto position = std::move(*position_result);
    // Validate both header destinations before touching live bytes. The owning
    // state is exclusively accessed; these extents cannot change during this edit.
    auto element_count_check =
        read_integer(state.bytes(), original.descriptor + format::descriptor::element_count);
    if (!element_count_check) {
        return std::unexpected(element_count_check.error());
    }

    auto flags_check = read_integer(state.bytes(), format::header::flags, format::tag_bytes);
    if (!flags_check) {
        return std::unexpected(flags_check.error());
    }

    auto element_count_write =
        write_integer(state.bytes(), original.descriptor + format::descriptor::element_count, size);
    if (!element_count_write) {
        return std::unexpected(element_count_write.error());
    }

    auto flags_write = write_integer(state.bytes(), format::header::flags, format::reserved_capacity_flag,
                                     format::tag_bytes);
    if (!flags_write) {
        return std::unexpected(flags_write.error());
    }

    if (length != 0) {
        std::memset(state.bytes().data() + offset, 0, length);
    }
    state.plan.nodes[position].size = size;
    ++state.plan.nodes[position].revision;
    ++state.epoch;
    return {};
}
}

// Use an in-place fast path for scalar arrays that already have enough capacity.
// Nested/relocating edits instead stage a new tree and rebuild the owning image.
Result<void> resize(const std::shared_ptr<State>& state_ptr, Handle handle, std::uint64_t size,
                    bool reserve_only) {
    const auto operation = reserve_only ? ErrorOperation::Reserve : ErrorOperation::Resize;
    return with_allocation_errors(operation, [&]() -> Result<void> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        auto original_ref_result = node(state, handle);
        if (!original_ref_result) {
            return std::unexpected(original_ref_result.error());
        }

        auto original_ref = std::move(*original_ref_result);
        const auto& original = original_ref.get();
        if (original.type.depth == 0) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        auto stride_result = element_width(original.type);
        if (!stride_result) {
            return std::unexpected(stride_result.error());
        }

        auto stride = std::move(*stride_result);
        auto byte_count_result = checked_multiply(size, stride);
        if (!byte_count_result) {
            return std::unexpected(byte_count_result.error());
        }

        auto byte_count = std::move(*byte_count_result);
        auto host_size_result = host_size(byte_count);
        if (!host_size_result) {
            return std::unexpected(host_size_result.error());
        }

        if (reserve_only && size <= original.capacity) {
            return {};
        }

        if (!reserve_only && size == original.size) {
            return {};
        }

        auto before_change_result = before_change(state);
        if (!before_change_result) {
            return std::unexpected(before_change_result.error());
        }

        if (!reserve_only && is_scalar_array(original.type) && size <= original.capacity) {
            return resize_scalar_in_place(state, handle, original, size, stride);
        }

        // Reject impossible fixed requests before allocating per-child metadata.
        auto cost_per_element = stride;
        if (!is_scalar_array(original.type) && !reserve_only) {
            auto type_result = element_type(original.type);
            if (!type_result) {
                return std::unexpected(type_result.error());
            }

            auto type = std::move(*type_result);
            auto cost_result = initial_cost(type);
            if (!cost_result) {
                return std::unexpected(cost_result.error());
            }

            auto cost = std::move(*cost_result);
            auto child_cost_result = checked_add(cost, format::integer_bytes);
            if (!child_cost_result) {
                return std::unexpected(child_cost_result.error());
            }

            auto child_cost = std::move(*child_cost_result);
            cost_per_element = child_cost;
        }
        auto payload_cost_result = checked_multiply(size, cost_per_element);
        if (!payload_cost_result) {
            return std::unexpected(payload_cost_result.error());
        }

        auto payload_cost = std::move(*payload_cost_result);
        auto lower_bound_result = checked_add(format::header::size, payload_cost);
        if (!lower_bound_result) {
            return std::unexpected(lower_bound_result.error());
        }

        auto lower_bound = std::move(*lower_bound_result);
        if (lower_bound > state.limit()) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded});
        }

        auto position_result = host_size(handle.index);
        if (!position_result) {
            return std::unexpected(position_result.error());
        }

        auto position = std::move(*position_result);
        auto count_result = host_size(size);
        if (!count_result) {
            return std::unexpected(count_result.error());
        }

        auto count = std::move(*count_result);
        Plan plan = state.plan;
        if (!reserve_only) {
            if (!is_scalar_array(original.type)) {
                if (size < original.size) {
                    for (std::uint64_t i = size; i < original.size; ++i) {
                        auto child_index_result = host_size(i);
                        if (!child_index_result) {
                            return std::unexpected(child_index_result.error());
                        }

                        auto child_index = std::move(*child_index_result);
                        auto remove_tree_result = remove_tree(plan, original.children[child_index]);
                        if (!remove_tree_result) {
                            return std::unexpected(remove_tree_result.error());
                        }
                    }
                    plan.nodes[position].children.resize(count);
                } else {
                    plan.nodes[position].children.reserve(count);
                    auto type_result = element_type(original.type);
                    if (!type_result) {
                        return std::unexpected(type_result.error());
                    }

                    auto type = std::move(*type_result);
                    for (auto i = original.size; i < size; ++i) {
                        auto next_result = initialized_node(plan, type);
                        if (!next_result) {
                            return std::unexpected(next_result.error());
                        }

                        auto next = std::move(*next_result);
                        plan.nodes[position].children.push_back(next);
                    }
                }
            }
            plan.nodes[position].size = size;
        }
        if (reserve_only) {
            plan.nodes[position].capacity = size;
            return rebuild(state, plan);
        }

        return rebuild_with_growth(state, plan, handle.index, size);
    });
}

Result<void> append(const std::shared_ptr<State>& state_ptr, Handle handle,
                    std::span<const std::byte> bytes) {
    return with_allocation_errors(ErrorOperation::Append, [&]() -> Result<void> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        auto original_ref_result = node(state, handle);
        if (!original_ref_result) {
            return std::unexpected(original_ref_result.error());
        }

        auto original_ref = std::move(*original_ref_result);
        const auto& original = original_ref.get();
        if (!is_scalar_array(original.type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        auto width_result = element_width(original.type);
        if (!width_result) {
            return std::unexpected(width_result.error());
        }

        auto width = std::move(*width_result);
        if (bytes.size() != width) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        const auto index = original.size;
        auto size_result = checked_add(index, 1);
        if (!size_result) {
            return std::unexpected(size_result.error());
        }

        auto size = std::move(*size_result);
        auto resize_result = resize(state_ptr, handle, size, false);
        if (!resize_result) {
            return std::unexpected(resize_result.error());
        }

        // Bytes passed here are an owned scalar encoding; no alias to the arena.
        auto value_ref_result = node(state, handle);
        if (!value_ref_result) {
            return std::unexpected(value_ref_result.error());
        }

        auto value_ref = std::move(*value_ref_result);
        auto offset_result = host_size(value_ref.get().offset + index * bytes.size());
        if (!offset_result) {
            return std::unexpected(offset_result.error());
        }

        auto offset = std::move(*offset_result);
        std::memcpy(state.bytes().data() + offset, bytes.data(), bytes.size());
        return {};
    });
}

Result<void> append_child(const std::shared_ptr<State>& state_ptr, Handle handle,
                          const std::shared_ptr<State>& from, Handle source) {
    return with_allocation_errors(ErrorOperation::Append, [&]() -> Result<void> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        auto source_ref_result = live(from);
        if (!source_ref_result) {
            return std::unexpected(source_ref_result.error());
        }

        auto source_ref = std::move(*source_ref_result);
        auto& source_state = source_ref.get();
        auto value_ref_result = node(state, handle);
        if (!value_ref_result) {
            return std::unexpected(value_ref_result.error());
        }

        auto value_ref = std::move(*value_ref_result);
        const auto& value = value_ref.get();
        auto original_ref_result = node(source_state, source);
        if (!original_ref_result) {
            return std::unexpected(original_ref_result.error());
        }

        auto original_ref = std::move(*original_ref_result);
        if (value.type.depth == 0 || is_scalar_array(value.type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        auto type_result = element_type(value.type);
        if (!type_result) {
            return std::unexpected(type_result.error());
        }

        auto type = std::move(*type_result);
        if (!same_type(original_ref.get().type, type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        auto size_result = checked_add(value.size, 1);
        if (!size_result) {
            return std::unexpected(size_result.error());
        }

        auto size = std::move(*size_result);
        auto position_result = host_size(handle.index);
        if (!position_result) {
            return std::unexpected(position_result.error());
        }

        auto position = std::move(*position_result);
        Plan plan = state.plan;
        auto copied_result = clone_tree(plan, source_state, source);
        if (!copied_result) {
            return std::unexpected(copied_result.error());
        }

        auto copied = std::move(*copied_result);
        plan.nodes[position].children.push_back(copied);
        plan.nodes[position].size = size;
        return rebuild_with_growth(state, plan, handle.index, size);
    });
}

Result<void> assign(const std::shared_ptr<State>& state_ptr, Handle handle, std::span<const std::byte> bytes,
                    std::uint64_t size) {
    return with_allocation_errors(ErrorOperation::Assign, [&]() -> Result<void> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        auto value_ref_result = node(state, handle);
        if (!value_ref_result) {
            return std::unexpected(value_ref_result.error());
        }

        auto value_ref = std::move(*value_ref_result);
        const auto& value = value_ref.get();
        if (!is_scalar_array(value.type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        auto width_result = element_width(value.type);
        if (!width_result) {
            return std::unexpected(width_result.error());
        }

        auto width = std::move(*width_result);
        auto length_result = checked_multiply(size, width);
        if (!length_result) {
            return std::unexpected(length_result.error());
        }

        auto length = std::move(*length_result);
        if (length != bytes.size()) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        auto before_change_result = before_change(state);
        if (!before_change_result) {
            return std::unexpected(before_change_result.error());
        }

        auto position_result = host_size(handle.index);
        if (!position_result) {
            return std::unexpected(position_result.error());
        }

        auto position = std::move(*position_result);
        if (size <= value.capacity) {
            auto offset_result = host_size(value.offset);
            if (!offset_result) {
                return std::unexpected(offset_result.error());
            }

            auto offset = std::move(*offset_result);
            // Validate the entire in-place edit before modifying the live image.
            const auto tail_count = size < value.size ? (value.size - size) * width : 0;
            auto tail_length_result = host_size(tail_count);
            if (!tail_length_result) {
                return std::unexpected(tail_length_result.error());
            }

            auto tail_length = std::move(*tail_length_result);
            auto element_count_check =
                read_integer(state.bytes(), value.descriptor + format::descriptor::element_count);
            if (!element_count_check) {
                return std::unexpected(element_count_check.error());
            }

            auto flags_check = read_integer(state.bytes(), format::header::flags, format::tag_bytes);
            if (!flags_check) {
                return std::unexpected(flags_check.error());
            }

            auto element_count_write =
                write_integer(state.bytes(), value.descriptor + format::descriptor::element_count, size);
            if (!element_count_write) {
                return std::unexpected(element_count_write.error());
            }

            auto flags_write = write_integer(state.bytes(), format::header::flags,
                                             format::reserved_capacity_flag, format::tag_bytes);
            if (!flags_write) {
                return std::unexpected(flags_write.error());
            }

            if (!bytes.empty()) {
                std::memcpy(state.bytes().data() + offset, bytes.data(), bytes.size());
            }
            if (tail_length != 0) {
                std::memset(state.bytes().data() + offset + bytes.size(), 0, tail_length);
            }
            state.plan.nodes[position].size = size;
            ++state.plan.nodes[position].revision;
            ++state.epoch;
            return {};
        }

        Plan plan = state.plan;
        plan.nodes[position].size = size;
        plan.nodes[position].imported = bytes;
        return rebuild_with_growth(state, plan, handle.index, size);
    });
}

Result<void> preflight_assign(const std::shared_ptr<State>& state_ptr, Handle handle, std::uint64_t size) {
    return with_allocation_errors(ErrorOperation::Import, [&]() -> Result<void> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        auto value_ref_result = node(state, handle);
        if (!value_ref_result) {
            return std::unexpected(value_ref_result.error());
        }

        auto value_ref = std::move(*value_ref_result);
        const auto& value = value_ref.get();
        if (!is_scalar_array(value.type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        auto width_result = element_width(value.type);
        if (!width_result) {
            return std::unexpected(width_result.error());
        }

        auto width = std::move(*width_result);
        auto length_result = checked_multiply(size, width);
        if (!length_result) {
            return std::unexpected(length_result.error());
        }

        auto length = std::move(*length_result);
        auto host_size_result = host_size(length);
        if (!host_size_result) {
            return std::unexpected(host_size_result.error());
        }

        if (size > value.capacity) {
            auto position_result = host_size(handle.index);
            if (!position_result) {
                return std::unexpected(position_result.error());
            }

            auto position = std::move(*position_result);
            Plan plan = state.plan;
            plan.nodes[position].size = size;
            plan.nodes[position].capacity = size;
            auto positions_result = calculate_layout(plan, false);
            if (!positions_result) {
                return std::unexpected(positions_result.error());
            }

            auto positions = std::move(*positions_result);
            if (positions.total > state.limit()) {
                return std::unexpected(Error{ErrorCode::CapacityExceeded});
            }

            if (positions.total > state.scratch_limit) {
                return std::unexpected(Error{ErrorCode::ResourceExhausted});
            }
        }
        if (length > state.scratch_limit) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted});
        }

        return {};
    });
}

Result<void> shrink(const std::shared_ptr<State>& state_ptr, Handle handle) {
    return with_allocation_errors(ErrorOperation::Shrink, [&]() -> Result<void> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        auto value_ref_result = node(state, handle);
        if (!value_ref_result) {
            return std::unexpected(value_ref_result.error());
        }

        auto value_ref = std::move(*value_ref_result);
        const auto& value = value_ref.get();
        if (value.size == value.capacity) {
            return {};
        }

        auto position_result = host_size(handle.index);
        if (!position_result) {
            return std::unexpected(position_result.error());
        }

        auto position = std::move(*position_result);
        Plan plan = state.plan;
        plan.nodes[position].capacity = value.size;
        return rebuild(state, plan);
    });
}

Result<void> compact(const std::shared_ptr<State>& state_ptr) {
    return with_allocation_errors(ErrorOperation::Compact, [&]() -> Result<void> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        Plan plan = state.plan;
        bool changed = false;
        for (auto& value : plan.nodes) {
            changed |= value.alive && value.capacity != value.size;
            value.capacity = value.size;
        }
        if (changed) {
            return rebuild(state, plan);
        }

        return {};
    });
}

Result<Usage> usage(const std::shared_ptr<State>& state_ptr) {
    return with_allocation_errors(ErrorOperation::Usage, [&]() -> Result<Usage> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        auto packed_result = calculate_layout(state.plan, true);
        if (!packed_result) {
            return std::unexpected(packed_result.error());
        }

        auto packed = std::move(*packed_result);
        auto reserved_result = calculate_layout(state.plan, false);
        if (!reserved_result) {
            return std::unexpected(reserved_result.error());
        }

        auto reserved = std::move(*reserved_result);
        return Usage{packed.total, reserved.total, state.is_fixed ? state.fixed.size() : 0};
    });
}

Result<Binding> clone_core(Binding source) {
    auto source_ref_result = live(source.state);
    if (!source_ref_result) {
        return std::unexpected(source_ref_result.error());
    }

    auto source_ref = std::move(*source_ref_result);
    auto target_result = create_local_state();
    if (!target_result) {
        return std::unexpected(target_result.error());
    }

    auto target = std::move(*target_result);
    Plan plan;
    auto handle_result = clone_tree(plan, source_ref.get(), source.handle);
    if (!handle_result) {
        return std::unexpected(handle_result.error());
    }

    auto handle = std::move(*handle_result);
    plan.roots.push_back(handle);
    auto rebuild_result = rebuild(*target, plan);
    if (!rebuild_result) {
        return std::unexpected(rebuild_result.error());
    }

    return Binding{std::move(target), handle, false};
}

Result<Binding> clone(Binding source) {
    return with_allocation_errors(ErrorOperation::Copy, [&]() -> Result<Binding> {
        return clone_core(source);
    });
}

Binding copy_local(Binding source) {
    (void)node_epoch(source.state, source.handle);
    return contract_value(clone_core(std::move(source)));
}

Handle construct_root(const std::shared_ptr<State>& state, Type type) {
    return contract_value(add_root_core(state, type));
}

Binding construct_record(const Schema& schema) {
    auto state = contract_value(create_local_state());
    auto handle = construct_root(state, {0, 0, &schema, schema.hash});
    return {std::move(state), handle, false};
}

Result<void> replace(Binding target, Binding source) {
    return with_allocation_errors(ErrorOperation::Assign, [&]() -> Result<void> {
        auto target_ref_result = live(target.state);
        if (!target_ref_result) {
            return std::unexpected(target_ref_result.error());
        }

        auto target_ref = std::move(*target_ref_result);
        auto& to = target_ref.get();
        auto source_ref_result = live(source.state);
        if (!source_ref_result) {
            return std::unexpected(source_ref_result.error());
        }

        auto source_ref = std::move(*source_ref_result);
        auto& source_state = source_ref.get();
        auto target_node_result = node(to, target.handle);
        if (!target_node_result) {
            return std::unexpected(target_node_result.error());
        }

        auto target_node = std::move(*target_node_result);
        auto source_node_result = node(source_state, source.handle);
        if (!source_node_result) {
            return std::unexpected(source_node_result.error());
        }

        auto source_node = std::move(*source_node_result);
        if (!same_type(target_node.get().type, source_node.get().type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        if (target.state == source.state && target.handle.index == source.handle.index) {
            return {};
        }

        Plan plan = to.plan;
        std::vector<std::pair<Handle, Handle>> pending{{target.handle, source.handle}};
        while (!pending.empty()) {
            auto [destination, origin] = pending.back();
            pending.pop_back();
            auto original_ref_result = node(source_state, origin);
            if (!original_ref_result) {
                return std::unexpected(original_ref_result.error());
            }

            auto original_ref = std::move(*original_ref_result);
            const auto& original = original_ref.get();
            auto position_result = host_size(destination.index);
            if (!position_result) {
                return std::unexpected(position_result.error());
            }

            auto position = std::move(*position_result);
            auto& current = plan.nodes[position];
            if (original.type.depth == 0) {
                if (current.children.size() != original.children.size()) {
                    return std::unexpected(Error{ErrorCode::TypeMismatch});
                }

                for (std::size_t i = 0; i < current.children.size(); ++i) {
                    pending.emplace_back(current.children[i], original.children[i]);
                }
            } else if (is_scalar_array(original.type)) {
                current.size = original.size;
                current.capacity = std::max(current.capacity, original.size);
                if (original.size != 0) {
                    auto width_result = element_width(original.type);
                    if (!width_result) {
                        return std::unexpected(width_result.error());
                    }

                    auto width = std::move(*width_result);
                    auto byte_count_result = checked_multiply(original.size, width);
                    if (!byte_count_result) {
                        return std::unexpected(byte_count_result.error());
                    }

                    auto byte_count = std::move(*byte_count_result);
                    auto length_result = host_size(byte_count);
                    if (!length_result) {
                        return std::unexpected(length_result.error());
                    }

                    auto length = std::move(*length_result);
                    auto offset_result = host_size(original.offset);
                    if (!offset_result) {
                        return std::unexpected(offset_result.error());
                    }

                    auto offset = std::move(*offset_result);
                    current.imported = source_state.bytes().subspan(offset, length);
                }
            } else {
                auto children = current.children;
                for (auto handle : children) {
                    auto remove_tree_result = remove_tree(plan, handle);
                    if (!remove_tree_result) {
                        return std::unexpected(remove_tree_result.error());
                    }
                }
                plan.nodes[position].children.clear();
                for (auto handle : original.children) {
                    auto copied_result = clone_tree(plan, source_state, handle);
                    if (!copied_result) {
                        return std::unexpected(copied_result.error());
                    }

                    auto copied = std::move(*copied_result);
                    plan.nodes[position].children.push_back(copied);
                }
                auto& updated = plan.nodes[position];
                updated.size = original.size;
                updated.capacity = std::max(updated.capacity, original.size);
            }
        }
        return rebuild(to, plan);
    });
}

Result<void> assign_children(Binding target, std::span<const Binding> sources) {
    return with_allocation_errors(ErrorOperation::Assign, [&]() -> Result<void> {
        auto state_ref_result = live(target.state);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        auto original_ref_result = node(state, target.handle);
        if (!original_ref_result) {
            return std::unexpected(original_ref_result.error());
        }

        auto original_ref = std::move(*original_ref_result);
        const auto& original = original_ref.get();
        if (original.type.depth == 0 || is_scalar_array(original.type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        auto type_result = element_type(original.type);
        if (!type_result) {
            return std::unexpected(type_result.error());
        }

        auto type = std::move(*type_result);
        auto cost_result = initial_cost(type);
        if (!cost_result) {
            return std::unexpected(cost_result.error());
        }

        auto cost = std::move(*cost_result);
        auto child_cost_result = checked_add(cost, format::integer_bytes);
        if (!child_cost_result) {
            return std::unexpected(child_cost_result.error());
        }

        auto child_cost = std::move(*child_cost_result);
        auto payload_cost_result = checked_multiply(sources.size(), child_cost);
        if (!payload_cost_result) {
            return std::unexpected(payload_cost_result.error());
        }

        auto payload_cost = std::move(*payload_cost_result);
        auto minimum_result = checked_add(format::header::size, payload_cost);
        if (!minimum_result) {
            return std::unexpected(minimum_result.error());
        }

        auto minimum = std::move(*minimum_result);
        if (minimum > state.limit()) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded});
        }

        auto position_result = host_size(target.handle.index);
        if (!position_result) {
            return std::unexpected(position_result.error());
        }

        auto position = std::move(*position_result);
        Plan plan = state.plan;
        for (auto child : original.children) {
            auto remove_tree_result = remove_tree(plan, child);
            if (!remove_tree_result) {
                return std::unexpected(remove_tree_result.error());
            }
        }
        plan.nodes[position].children.clear();
        for (const auto& source : sources) {
            auto source_ref_result = live(source.state);
            if (!source_ref_result) {
                return std::unexpected(source_ref_result.error());
            }

            auto source_ref = std::move(*source_ref_result);
            auto& source_state = source_ref.get();
            auto source_node_result = node(source_state, source.handle);
            if (!source_node_result) {
                return std::unexpected(source_node_result.error());
            }

            auto source_node = std::move(*source_node_result);
            if (!same_type(source_node.get().type, type)) {
                return std::unexpected(Error{ErrorCode::TypeMismatch});
            }

            auto copied_result = clone_tree(plan, source_state, source.handle);
            if (!copied_result) {
                return std::unexpected(copied_result.error());
            }

            auto copied = std::move(*copied_result);
            plan.nodes[position].children.push_back(copied);
        }
        auto& next = plan.nodes[position];
        next.size = sources.size();
        next.capacity = std::max(next.capacity, next.size);
        return rebuild(state, plan);
    });
}

Result<Binding> create_record(const Schema& schema) {
    return with_allocation_errors(ErrorOperation::CreateRecord, [&]() -> Result<Binding> {
        auto state_result = create_local_state();
        if (!state_result) {
            return std::unexpected(state_result.error());
        }

        auto state = std::move(*state_result);
        auto handle_result = add_root(state, {0, 0, &schema, schema.hash});
        if (!handle_result) {
            return std::unexpected(handle_result.error());
        }

        auto handle = std::move(*handle_result);
        return Binding{std::move(state), handle, false};
    });
}

Binding field(Binding parent, std::uint64_t index) {
    auto result = child(parent.state, parent.handle, index);
    if (!result) {
        vinc::contract_violation("invalid message field");
    }

    return {std::move(parent.state), *result, true};
}

Result<Binding> receive_record(std::span<const std::byte> bytes, const Schema& schema) {
    try {
        return adopt_record(std::vector<std::byte>(bytes.begin(), bytes.end()), schema);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Copy});
    }
}

namespace {
// image is validated against its schema. Until the final move, it owns all payload bytes;
// local State/Node owners unwind automatically if rebuilding lookup metadata fails.
Result<Binding> adopt_validated_record(Image& image) {
    auto state = std::make_shared<State>();
    state->plan.nodes.reserve(image.entries.size());
    auto table_result = read_integer(image.bytes(), format::header::descriptor_table);
    if (!table_result) {
        return std::unexpected(table_result.error());
    }

    auto table = std::move(*table_result);
    for (std::size_t i = 0; i < image.entries.size(); ++i) {
        const auto& entry = image.entries[i];
        Node descriptor_node;
        descriptor_node.alive = true;
        descriptor_node.generation = i + 1;
        descriptor_node.type = entry.type;
        descriptor_node.size = entry.size;
        descriptor_node.capacity = entry.capacity;
        descriptor_node.offset = entry.offset;
        descriptor_node.descriptor = table + i * format::descriptor::size;
        if (!is_scalar_array(entry.type)) {
            for (std::uint64_t j = 0; j < entry.size; ++j) {
                auto id_result = read_integer(image.bytes(), entry.offset + j * format::integer_bytes);
                if (!id_result) {
                    return std::unexpected(id_result.error());
                }

                auto id = std::move(*id_result);
                descriptor_node.children.push_back({id, id + 1});
            }
        }
        state->plan.nodes.push_back(std::move(descriptor_node));
    }
    const Handle handle{image.roots[0], image.roots[0] + 1};
    state->plan.roots.push_back(handle);
    state->plan.next_generation = image.entries.size() + 1;
    state->plan.first_free = image.entries.size();
    state->local = std::move(image.owned);
    return Binding{std::move(state), handle, false};
}
}

Result<Binding> adopt_record(std::vector<std::byte> bytes, const Schema& schema) {
    auto result = with_allocation_errors(ErrorOperation::ValidateImage, [&]() -> Result<Binding> {
        Image image;
        image.owned = std::move(bytes);
        auto validate_result = validate(image);
        if (!validate_result) {
            return std::unexpected(validate_result.error());
        }

        if (image.roots.size() != 1) {
            return std::unexpected(Error{ErrorCode::InvalidData});
        }

        auto validate_schema_result = validate_schema(image, image.roots[0], {0, 0, &schema, schema.hash});
        if (!validate_schema_result) {
            return std::unexpected(validate_schema_result.error());
        }

        return adopt_validated_record(image);
    });
    if (!result && result.error().is(ErrorCode::SizeOverflow)) {
        result.error().code = static_cast<std::int32_t>(ErrorCode::InvalidData);
    }
    return result;
}

namespace {
// Only lookup metadata is allocated here. Child handles refer to the retained Image;
// clone_tree consumes their bytes synchronously before this temporary plan is destroyed.
Result<void> populate_readonly_plan(Plan& plan, const Image& image) {
    plan.nodes.reserve(image.entries.size());
    for (std::size_t i = 0; i < image.entries.size(); ++i) {
        const auto& entry = image.entries[i];
        Node descriptor_node;
        descriptor_node.alive = true;
        descriptor_node.generation = i + 1;
        descriptor_node.type = entry.type;
        descriptor_node.size = entry.size;
        descriptor_node.capacity = entry.capacity;
        descriptor_node.offset = entry.offset;
        if (!is_scalar_array(entry.type)) {
            for (std::uint64_t j = 0; j < entry.size; ++j) {
                auto child_result = read_integer(image.bytes(), entry.offset + j * format::integer_bytes);
                if (!child_result) {
                    return std::unexpected(child_result.error());
                }

                auto child = std::move(*child_result);
                descriptor_node.children.push_back({child, child + 1});
            }
        }
        plan.nodes.push_back(std::move(descriptor_node));
    }
    return {};
}
}

Result<Binding> copy_image_record(const std::shared_ptr<const Image>& image, std::uint64_t id, Type type) {
    return with_allocation_errors(ErrorOperation::Copy, [&]() -> Result<Binding> {
        if (!image || id >= image->entries.size()) {
            return std::unexpected(Error{ErrorCode::Closed});
        }

        Image inspected;
        inspected.owner = std::shared_ptr<void>(image, const_cast<Image*>(image.get()));
        inspected.borrowed = image->bytes();
        inspected.entries = image->entries;
        auto validate_schema_result = validate_schema(inspected, id, type);
        if (!validate_schema_result) {
            return std::unexpected(validate_schema_result.error());
        }

        State source;
        // Read-only adapter, used solely as clone_tree's source; never returned or mutated.
        source.is_fixed = true;
        source.fixed = {const_cast<std::byte*>(inspected.bytes().data()), inspected.bytes().size()};
        auto populate_readonly_plan_result = populate_readonly_plan(source.plan, inspected);
        if (!populate_readonly_plan_result) {
            return std::unexpected(populate_readonly_plan_result.error());
        }

        auto target_result = create_local_state();
        if (!target_result) {
            return std::unexpected(target_result.error());
        }

        auto target = std::move(*target_result);
        Plan plan;
        auto handle_result = clone_tree(plan, source, {id, id + 1});
        if (!handle_result) {
            return std::unexpected(handle_result.error());
        }

        auto handle = std::move(*handle_result);
        plan.roots.push_back(handle);
        auto rebuild_result = rebuild(*target, plan);
        if (!rebuild_result) {
            return std::unexpected(rebuild_result.error());
        }

        return Binding{std::move(target), handle, false};
    });
}

std::uint64_t epoch(const std::shared_ptr<State>& state_ptr) {
    if (!state_ptr || !state_ptr->active) {
        vinc::contract_violation("invalid or expired message access");
    }

    return state_ptr->epoch;
}

void check_epoch(const std::shared_ptr<State>& state_ptr, std::uint64_t expected) {
    if (epoch(state_ptr) != expected) {
        vinc::contract_violation("invalid or expired message access");
    }
}

std::uint64_t node_epoch(const std::shared_ptr<State>& state_ptr, Handle handle) {
    // Public access requires a live handle. This path only checks metadata and
    // performs no allocation; internal recoverable operations use node() directly.
    return contract_value(with_allocation_errors(ErrorOperation::CheckHandle, [&]() -> Result<std::uint64_t> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto value_ref_result = node(state_ref.get(), handle);
        if (!value_ref_result) {
            return std::unexpected(value_ref_result.error());
        }

        auto value_ref = std::move(*value_ref_result);
        return value_ref.get().revision;
    }));
}

void check_node_epoch(const std::shared_ptr<State>& state_ptr, Handle handle, std::uint64_t expected) {
    if (node_epoch(state_ptr, handle) != expected) {
        vinc::contract_violation("invalid or expired message access");
    }
}

Result<Snapshot> snapshot(const std::shared_ptr<State>& state_ptr, std::uint64_t maximum) {
    return with_allocation_errors(ErrorOperation::Snapshot, [&]() -> Result<Snapshot> {
        auto state_ref_result = live(state_ptr);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto image_result = make_image(state_ref.get(), maximum);
        if (!image_result) {
            return std::unexpected(image_result.error());
        }

        auto image = std::move(*image_result);
        return Snapshot(std::move(image));
    });
}

Result<std::uint64_t> image_root(const std::shared_ptr<const Image>& image, std::uint64_t index, Type type) {
    return with_allocation_errors(ErrorOperation::AccessRoot, [&]() -> Result<std::uint64_t> {
        if (!image) {
            return std::unexpected(Error{ErrorCode::Closed});
        }

        if (index >= image->roots.size()) {
            return std::unexpected(Error{ErrorCode::OutOfRange});
        }

        auto position_result = host_size(index);
        if (!position_result) {
            return std::unexpected(position_result.error());
        }

        auto position = std::move(*position_result);
        const auto id = image->roots[position];
        auto entry_index_result = host_size(id);
        if (!entry_index_result) {
            return std::unexpected(entry_index_result.error());
        }

        auto entry_index = std::move(*entry_index_result);
        if (!same_type(image->entries[entry_index].type, type)) {
            return std::unexpected(Error{ErrorCode::TypeMismatch});
        }

        // Generic image validation already checks scalar/nested-scalar types.
        // Records additionally need field order, field types and singular sizes
        // checked before any allocation-free typed proxy can be exposed.
        if (type.scalar == 0) {
            auto validated = validate_schema(*image, id, type);
            if (!validated) {
                return std::unexpected(validated.error());
            }
        }

        return id;
    });
}

std::uint64_t image_size(const std::shared_ptr<const Image>& image, std::uint64_t id) {
    // Images are validated before exposure; invalid handles are contract violations.
    if (!image) {
        vinc::contract_violation("invalid or expired message access");
    }

    if (id >= image->entries.size()) {
        vinc::contract_violation("invalid image descriptor");
    }

    return image->entries[contract_value(host_size(id))].size;
}

std::uint64_t image_capacity(const std::shared_ptr<const Image>& image, std::uint64_t id) {
    (void)image_size(image, id);
    return image->entries[contract_value(host_size(id))].capacity;
}

std::uint64_t image_child(const std::shared_ptr<const Image>& image, std::uint64_t id, std::uint64_t index) {
    if (!image) {
        vinc::contract_violation("invalid or expired message access");
    }

    if (id >= image->entries.size()) {
        vinc::contract_violation("invalid image descriptor");
    }

    const auto& value = image->entries[contract_value(host_size(id))];
    if (index >= value.size) {
        vinc::contract_violation("invalid or expired message access");
    }

    return contract_value(read_integer(image->bytes(), value.offset + index * format::integer_bytes));
}

std::array<std::byte, 8> image_read(const std::shared_ptr<const Image>& image, std::uint64_t id,
                                    std::uint64_t index) {
    if (!image) {
        vinc::contract_violation("invalid or expired message access");
    }

    if (id >= image->entries.size()) {
        vinc::contract_violation("invalid image descriptor");
    }

    const auto& value = image->entries[contract_value(host_size(id))];
    if (index >= value.size) {
        vinc::contract_violation("invalid or expired message access");
    }

    std::array<std::byte, 8> bytes{};
    const auto stride = contract_value(element_width(value.type));
    const auto offset = contract_value(host_size(value.offset + index * stride));
    const auto length = contract_value(host_size(stride));
    std::memcpy(bytes.data(), image->bytes().data() + offset, length);
    return bytes;
}

// Access handles are process-local metadata. Validation and child traversal never
// construct schema objects, copy payloads or allocate an element wrapper.
void validate_access(const Access& access) {
    if (access.image) {
        (void)image_size(access.image, access.id);
    } else {
        (void)node_epoch(access.state, access.handle);
    }
}

std::uint64_t access_size(const Access& access) {
    validate_access(access);
    if (access.image) {
        return image_size(access.image, access.id);
    }

    return contract_value(info(access.state, access.handle)).size;
}

Access access_child(const Access& access, std::uint64_t index) {
    if (index >= access_size(access)) {
        vinc::contract_index_violation("message element index out of range", index, access_size(access));
    }

    if (access.image) {
        return {{}, {}, access.image, image_child(access.image, access.id, index)};
    }

    return {access.state, contract_value(child(access.state, access.handle, index))};
}

std::array<std::byte, 8> access_read(const Access& access, std::uint64_t index) {
    validate_access(access);
    if (access.image) {
        return image_read(access.image, access.id, index);
    }

    return contract_value(read(access.state, access.handle, index));
}

std::uint64_t access_revision(const Access& access) {
    validate_access(access);
    return access.image ? 0 : node_epoch(access.state, access.handle);
}

void check_revision(const Access& access, std::uint64_t revision) {
    if (access_revision(access) != revision) {
        vinc::contract_violation("invalidated message element or iterator");
    }
}

Result<Message> FrameworkAccess::borrow(std::span<std::byte> bytes, std::shared_ptr<void> owner,
                                        std::uint64_t scratch_limit) {
    return with_allocation_errors(ErrorOperation::Borrow, [&]() -> Result<Message> {
        if (!owner || bytes.size() < format::header::size || scratch_limit < format::header::size) {
            return std::unexpected(Error{ErrorCode::InvalidArgument});
        }

        auto state = std::make_shared<State>();
        state->is_fixed = true;
        state->fixed = bytes;
        state->owner = std::move(owner);
        state->scratch_limit = scratch_limit;
        auto positions_result = calculate_layout(state->plan, false);
        if (!positions_result) {
            return std::unexpected(positions_result.error());
        }

        auto positions = std::move(*positions_result);
        auto render_into_result =
            render_into(bytes.first(format::header::size), *state, state->plan, positions, false);
        if (!render_into_result) {
            return std::unexpected(render_into_result.error());
        }

        return Message(std::move(state));
    });
}

Result<Snapshot> FrameworkAccess::seal(Message& message) {
    return seal_binding({message.state_, {}, false});
}

// End exclusive construction without moving payload bytes. Revoking the writable
// binding invalidates saved field wrappers; Message Bus publishes the retained image afterward.
Result<Snapshot> seal_binding(Binding binding) {
    return with_allocation_errors(ErrorOperation::Seal, [&]() -> Result<Snapshot> {
        auto state_ref_result = live(binding.state);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        if (!state.is_fixed) {
            return std::unexpected(Error{ErrorCode::InvalidArgument});
        }

        auto total_result = read_integer(state.fixed, format::header::total_bytes);
        if (!total_result) {
            return std::unexpected(total_result.error());
        }

        auto total = std::move(*total_result);
        // Check the borrowed extent before forming a subspan from header data.
        if (total > state.fixed.size()) {
            return std::unexpected(Error{ErrorCode::InvalidData});
        }

        auto length_result = host_size(total);
        if (!length_result) {
            return std::unexpected(length_result.error());
        }

        auto length = std::move(*length_result);
        auto image = std::make_shared<Image>();
        image->borrowed = state.fixed.first(length);
        image->owner = state.owner;
        // Metadata validation only; no payload copy or movement.
        auto validate_result = validate(*image);
        if (!validate_result) {
            return std::unexpected(validate_result.error());
        }

        release_message(binding.state);
        return Snapshot(std::move(image));
    });
}

Result<Binding> FrameworkAccess::borrow_record(const Schema& schema, std::span<std::byte> bytes,
                                               std::shared_ptr<void> owner, std::uint64_t scratch_limit) {
    return with_allocation_errors(ErrorOperation::Borrow, [&]() -> Result<Binding> {
        if (!owner || bytes.size() < format::header::size || scratch_limit < format::header::size) {
            return std::unexpected(Error{ErrorCode::InvalidArgument});
        }

        auto state = std::make_shared<State>();
        state->is_fixed = true;
        state->fixed = bytes;
        state->owner = std::move(owner);
        state->scratch_limit = scratch_limit;
        auto handle_result = add_root(state, {0, 0, &schema, schema.hash});
        if (!handle_result) {
            return std::unexpected(handle_result.error());
        }

        auto handle = std::move(*handle_result);
        return Binding{std::move(state), handle, false};
    });
}

namespace {
// Retains the destination lease and builds lookup metadata without touching its bytes.
// Failure destroys this temporary image; publication has not started at that point.
Result<std::shared_ptr<Image>> prepare_published_image(const State& state, const Layout& positions,
                                                       std::span<std::byte> bytes,
                                                       std::shared_ptr<void> owner) {
    auto length_result = host_size(positions.total);
    if (!length_result) {
        return std::unexpected(length_result.error());
    }

    auto length = std::move(*length_result);
    auto count_result = host_size(positions.count);
    if (!count_result) {
        return std::unexpected(count_result.error());
    }

    auto count = std::move(*count_result);
    auto image = std::make_shared<Image>();
    image->owner = std::move(owner);
    image->borrowed = bytes.first(length);
    image->entries.reserve(count);
    image->roots.reserve(state.plan.roots.size());
    for (const auto& root : state.plan.roots) {
        auto position_result = host_size(root.index);
        if (!position_result) {
            return std::unexpected(position_result.error());
        }

        auto position = std::move(*position_result);
        image->roots.push_back(positions.ids[position]);
    }
    for (std::size_t i = 0; i < state.plan.nodes.size(); ++i) {
        const auto& descriptor_node = state.plan.nodes[i];
        if (descriptor_node.alive) {
            image->entries.push_back(
                {descriptor_node.type, positions.offsets[i], descriptor_node.size, descriptor_node.size});
        }
    }
    return image;
}
}

// Local push path: check packed size and reject overlap, then render straight into
// the reserved shared slot. The source remains local and usable after publication.
Result<Snapshot> publish_binding(Binding binding, std::span<std::byte> bytes, std::shared_ptr<void> owner) {
    return with_allocation_errors(ErrorOperation::Publish, [&]() -> Result<Snapshot> {
        auto state_ref_result = live(binding.state);
        if (!state_ref_result) {
            return std::unexpected(state_ref_result.error());
        }

        auto state_ref = std::move(*state_ref_result);
        auto& state = state_ref.get();
        if (!owner || state.plan.roots.size() != 1 || state.plan.roots[0].index != binding.handle.index) {
            return std::unexpected(Error{ErrorCode::InvalidArgument});
        }

        auto positions_result = calculate_layout(state.plan, true);
        if (!positions_result) {
            return std::unexpected(positions_result.error());
        }

        auto positions = std::move(*positions_result);
        if (positions.total > bytes.size()) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded});
        }

        const auto input = state.bytes();
        // Supported 64-bit targets use flat byte addresses. uintptr_t preserves these
        // addresses; valid spans occupy non-wrapping ranges. No integer is dereferenced.
        const auto input_address = reinterpret_cast<std::uintptr_t>(input.data());
        const auto output_address = reinterpret_cast<std::uintptr_t>(bytes.data());
        const bool disjoint = output_address >= input_address
                                  ? output_address - input_address >= input.size()
                                  : input_address - output_address >= bytes.size();
        if (!disjoint) {
            return std::unexpected(Error{ErrorCode::InvalidArgument});
        }

        auto image_result = prepare_published_image(state, positions, bytes, std::move(owner));
        if (!image_result) {
            return std::unexpected(image_result.error());
        }

        auto image = std::move(*image_result);
        auto length_result = host_size(positions.total);
        if (!length_result) {
            return std::unexpected(length_result.error());
        }

        auto length = std::move(*length_result);
        // All allocations/budget checks precede the first destination write.
        auto render_into_result = render_into(bytes.first(length), state, state.plan, positions, true);
        if (!render_into_result) {
            return std::unexpected(render_into_result.error());
        }

        return Snapshot(std::move(image));
    });
}

void FrameworkAccess::revoke(Message& message) noexcept {
    release_message(message.state_);
}
} // namespace vinc::message::detail

namespace vinc::message {
Message::Message() : state_(detail::local_state()) {
}

Message::~Message() {
    detail::release_message(state_);
}

Message::Message(Message&&) noexcept = default;

Message& Message::operator=(Message&& other) noexcept {
    if (this != &other) {
        detail::release_message(state_);
        state_ = std::move(other.state_);
    }

    return *this;
}

std::span<const std::byte> Snapshot::bytes() const noexcept {
    return image_ ? image_->bytes() : std::span<const std::byte>{};
}

std::uint64_t Snapshot::root_count() const noexcept {
    return image_ ? image_->roots.size() : 0;
}

Result<Snapshot> Snapshot::copy_from(std::span<const std::byte> bytes) {
    auto result = detail::with_allocation_errors(ErrorOperation::ValidateImage, [&]() -> Result<Snapshot> {
        auto image = std::make_shared<detail::Image>();
        image->owned.assign(bytes.begin(), bytes.end());
        auto validate_result = detail::validate(*image);
        if (!validate_result) {
            return std::unexpected(validate_result.error());
        }

        return Snapshot(std::move(image));
    });
    // Arithmetic overflow while inspecting an external image is invalid input.
    if (!result && result.error().is(ErrorCode::SizeOverflow)) {
        result.error().code = static_cast<std::int32_t>(ErrorCode::InvalidData);
    }
    return result;
}
} // namespace vinc::message
