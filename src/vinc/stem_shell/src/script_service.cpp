#include <stem_shell/script_service.hpp>
#include <algorithm>

namespace vinc::shell {
void ScriptService::poll() {
    if (!runtime_.active()) return;
    auto text = runtime_.take_output();
    auto box = mail_.find(owner_);
    auto append = [&](std::string_view value) {
        if (box == mail_.end()) return;
        auto& d = box->second;
        constexpr std::size_t limit = 256 * 1024;
        const auto count = (std::min)(value.size(), limit - d.output.size());
        d.output.append(value.substr(0, count));
        d.truncated |= count != value.size();
    };
    append(text);
    if (!runtime_.ready()) return;
    auto result = runtime_.finish();
    if (result.message.size() > 256 * 1024) { result.message.resize(256 * 1024 - 32); result.message += "\n[Script result truncated]"; }
    append(runtime_.take_output());
    if (result.shutdown) shutdown_requested_.store(true);
    if (box != mail_.end()) box->second.result = std::move(result);
    owner_ = 0;
}
CommandResult ScriptService::start(std::uint64_t owner, std::filesystem::path path, std::vector<std::string> args) {
    std::lock_guard lock(mutex_); poll();
    if (stopping_) return {false, false, "Framework is shutting down"};
    if (runtime_.active()) return {false, false, "Busy: a script is already running"};
    if (mail_.contains(owner)) return {false, false, "Busy: collect the previous script result first"};
    if (mail_.size() >= 17) return {false, false, "Busy: script result mailboxes are full"};
    mail_.try_emplace(owner);
    try {
        runtime_.start(std::move(path), std::move(args), [this](const auto& command, std::stop_token stop) {
            return router_.execute(command, stop);
        });
        owner_ = owner;
        return {true, false, "Script running. scancel cancels the global script."};
    } catch (const std::exception& e) { mail_.erase(owner); return {false, false, std::string("Cannot start script: ") + e.what()}; }
}
bool ScriptService::cancel(std::uint64_t owner) {
    std::lock_guard lock(mutex_); poll();
    return (!owner || owner == owner_) && runtime_.cancel();
}
bool ScriptService::active(std::uint64_t owner) {
    std::lock_guard lock(mutex_); poll();
    return runtime_.active() && (!owner || owner == owner_);
}
ScriptService::Delivery ScriptService::take(std::uint64_t owner) {
    std::lock_guard lock(mutex_); poll();
    const auto it = mail_.find(owner);
    if (it == mail_.end()) return {};
    auto result = std::move(it->second);
    if (owner != owner_) mail_.erase(it); else it->second = {};
    return result;
}
void ScriptService::forget(std::uint64_t owner) {
    std::lock_guard lock(mutex_); mail_.erase(owner);
}
void ScriptService::shutdown() {
    std::lock_guard lock(mutex_); stopping_ = true; runtime_.cancel();
}
}
