#include <vinc/shell/detail/script_service.hpp>
#include <algorithm>

namespace vinc::shell {
// Caller holds mutex_ throughout collection. Only ready futures are consumed;
// a live owner keeps its mailbox even after the current output batch is taken.
void ScriptService::poll() {
    if (!runtime_.active()) {
        return;
    }

    auto text = runtime_.take_output();
    auto mailbox = mail_.find(owner_);
    auto append = [&](std::string_view value) {
        if (mailbox == mail_.end()) {
            return;
        }

        auto& delivery = mailbox->second;
        constexpr auto limit = config::shell_script_delivery_bytes;
        const auto count = (std::min)(value.size(), limit - delivery.output.size());
        delivery.output.append(value.substr(0, count));
        delivery.truncated |= count != value.size();
    };
    append(text);
    if (!runtime_.ready()) {
        return;
    }

    auto result = runtime_.finish();

    // The worker can append a final chunk between the first drain and completion.
    append(runtime_.take_output());
    if (result.outcome && result.outcome->shutdown) {
        shutdown_requested_.store(true);
    }

    if (mailbox != mail_.end()) {
        mailbox->second.result = std::move(result);
    }

    owner_ = 0;
}

Result<void> ScriptService::start(std::uint64_t owner, std::filesystem::path path,
                                  std::vector<std::string> arguments) {
    std::lock_guard lock(mutex_);
    poll();
    if (stopping_) {
        return std::unexpected(Error{ErrorCode::Closing, ErrorOperation::StartScript});
    }

    if (runtime_.active()) {
        return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::StartScript});
    }

    if (mail_.contains(owner)) {
        return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::StartScript});
    }

    if (mail_.size() >= config::shell_script_mailboxes) {
        return std::unexpected(Error{ErrorCode::Busy, ErrorOperation::StartScript});
    }

    mail_.try_emplace(owner);
    try {
        auto started = runtime_.start(std::move(path), std::move(arguments),
                                      [this](const auto& command, std::stop_token stop) {
                                          return router_.execute(command, stop);
                                      });
        if (!started) {
            mail_.erase(owner);
            return std::unexpected(started.error());
        }

        owner_ = owner;
        return {};
    } catch (const std::exception&) {
        mail_.erase(owner);
        return std::unexpected(Error{ErrorCode::WorkerFailed, ErrorOperation::StartScript});
    }
}

bool ScriptService::cancel(std::uint64_t owner) {
    std::lock_guard lock(mutex_);
    poll();
    return (!owner || owner == owner_) && runtime_.cancel();
}

bool ScriptService::active(std::uint64_t owner) {
    std::lock_guard lock(mutex_);
    poll();
    return runtime_.active() && (!owner || owner == owner_);
}

ScriptService::Delivery ScriptService::take(std::uint64_t owner) {
    std::lock_guard lock(mutex_);
    poll();
    const auto mailbox = mail_.find(owner);
    if (mailbox == mail_.end()) {
        return {};
    }

    auto result = std::move(mailbox->second);
    if (owner != owner_) {
        mail_.erase(mailbox);
    } else {
        mailbox->second = {};
    }

    return result;
}

void ScriptService::forget(std::uint64_t owner) {
    std::lock_guard lock(mutex_);
    mail_.erase(owner);
}

void ScriptService::shutdown() {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    runtime_.cancel();
}
}
