#pragma once

// Framework-only access to externally owned, fixed-capacity message storage.
// Public constructors create local memory; Message Bus alone supplies and synchronizes a shared-memory loan.

// Framework-only detail contract; visibility does not make it an application API.
// Message Bus supplies a lease owner retaining the slot/mapping; synchronization lives in Message Bus.
#include <vinc/message/storage.hpp>

namespace vinc::message::detail {
class FrameworkAccess {
public:
    // owner must keep the exclusively borrowed bytes valid until release.
    // scratch_limit bounds any one payload rebuild buffer (metadata is additional).
    [[nodiscard]] static Result<Message> borrow(std::span<std::byte> bytes, std::shared_ptr<void> owner,
                                                std::uint64_t scratch_limit);
    // Validates the existing image without copying/moving payload, then consumes
    // write access. Snapshot retains owner; Message Bus controls publication.
    [[nodiscard]] static Result<Snapshot> seal(Message& message);
    static void revoke(Message& message) noexcept;
    static Result<Binding> borrow_record(const Schema& schema, std::span<std::byte> bytes,
                                         std::shared_ptr<void> owner, std::uint64_t scratch_limit);

    template <Record T>
    static Result<std::unique_ptr<T>> borrow_message(std::span<std::byte> bytes, std::shared_ptr<void> owner,
                                                     std::uint64_t scratch_limit) {
        auto binding = borrow_record(T::schema(), bytes, std::move(owner), scratch_limit);
        if (!binding) {
            return std::unexpected(binding.error());
        }

        try {
            return std::unique_ptr<T>(new T(RecordAccess::bind<T>(std::move(*binding))));
        } catch (const std::bad_alloc&) {
            release_message(binding->state);
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Bind});
        }
    }

    template <Record T> static Result<Snapshot> seal(T& message) {
        return seal_binding(RecordAccess::binding(message));
    }

    template <Record T> static void revoke(T& message) noexcept {
        release_message(RecordAccess::binding(message).state);
    }

    template <Record T>
    static Result<Snapshot> push(const T& message, std::span<std::byte> bytes, std::shared_ptr<void> owner) {
        return publish_binding(RecordAccess::binding(message), bytes, std::move(owner));
    }
};
}
