#include <vinc/rpc/detail/manager.hpp>
#include <algorithm>
#include <limits>

namespace vinc::rpc::detail {
namespace {
Error invalid() {
    return {ErrorCode::Protocol, ErrorOperation::Receive};
}

control::Bytes success(control::Bytes body) {
    control::Bytes bytes;
    control::put_u32(bytes, result_success);
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}
}

RpcManager::RpcManager(std::shared_ptr<vinc::detail::ResourceNames> names) : names_(std::move(names)) {
    worker_ = std::jthread([this](std::stop_token stop) {
        run(stop);
    });
}

RpcManager::~RpcManager() {
    worker_.request_stop();
    changed_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }

    // Manager is destroyed only after child shutdown. Instance journal survives
    // cleanup failures; no new central generation ever reuses these pool names.
}

void RpcManager::admit(std::uint64_t mid, std::string name, std::shared_ptr<control::Session> session) {
    auto peer = std::make_shared<Peer>();
    peer->mid = mid;
    peer->name = std::move(name);
    peer->session = std::move(session);
    std::lock_guard lock(mutex_);
    peers_.emplace(mid, std::move(peer));
}

void RpcManager::exited(std::uint64_t mid) {
    std::lock_guard lock(mutex_);
    if (auto found = peers_.find(mid); found != peers_.end()) {
        found->second->alive.store(false);
    }

    changed_.notify_all();
}

void RpcManager::stopping(std::uint64_t mid) {
    std::lock_guard lock(mutex_);
    if (auto found = peers_.find(mid); found != peers_.end()) {
        found->second->accepting.store(false);
    }
}

Result<void> RpcManager::submit(std::uint64_t mid, control::Frame value) {
    std::lock_guard lock(mutex_);
    auto peer = peers_.find(mid);
    if (peer == peers_.end() || !peer->second->alive.load()) {
        return std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Receive});
    }

    if (input_.size() >= config::rpc_control_limit) {
        return std::unexpected(Error{ErrorCode::QueueFull, ErrorOperation::Receive});
    }

    input_.push_back({peer->second, std::move(value)});
    changed_.notify_all();
    return {};
}

void RpcManager::reply(const std::shared_ptr<Peer>& peer, Operation operation, std::uint64_t request,
                       Result<control::Bytes> result) {
    if (!peer->alive.load()) {
        return;
    }

    if (peer->output.size() >= config::rpc_control_limit) {
        peer->session->close();
        return;
    }

    auto bytes = result ? success(std::move(*result)) : encode_failure(result.error());
    peer->output.push_back({control::Kind::Response, domain, operation, request, std::move(bytes)});
}

Transfer RpcManager::descriptor(const Service& service, std::size_t index) const {
    const auto stride = slot_bytes(service.options);
    return {service.slots[index].token,
            service.id,
            service.pool->name(),
            service.pool->extent(),
            index * *stride,
            service.options.request_bytes,
            service.options.response_bytes,
            0};
}

Result<control::Bytes> RpcManager::register_service(const std::shared_ptr<Peer>& peer,
                                                    const control::Frame& request) {
    control::Reader reader(request.payload);
    const auto wire_version = reader.u32();
    auto name = reader.string();
    const auto request_hash = reader.u64();
    const auto response_hash = reader.u64();
    ServiceOptions options{reader.u64(), reader.u64(), reader.u32()};
    if (!reader.done() || wire_version != version) {
        return std::unexpected(invalid());
    }

    if (!valid_name(name)) {
        return std::unexpected(Error{ErrorCode::InvalidName, ErrorOperation::Register});
    }

    for (const auto& [id, service] : services_) {
        if (service.owner == peer && service.name == name) {
            return std::unexpected(Error{ErrorCode::AlreadyRegistered, ErrorOperation::Register});
        }
    }

    auto extent = pool_bytes(options);
    if (!extent) {
        return std::unexpected(extent.error());
    }

    if (services_.size() >= config::rpc_service_limit || *extent > config::rpc_shared_bytes - bytes_) {
        return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Register});
    }

    if (next_service_ == UINT64_MAX) {
        return std::unexpected(Error{ErrorCode::IdExhausted, ErrorOperation::Register});
    }

    auto pool = Pool::create(names_, options);
    if (!pool) {
        return std::unexpected(pool.error());
    }

    Service service;
    service.id = next_service_++;
    service.owner = peer;
    service.name = std::move(name);
    service.request_hash = request_hash;
    service.response_hash = response_hash;
    service.options = options;
    service.pool = std::move(*pool);
    service.slots.resize(options.slots);
    control::Bytes result;
    control::put_u64(result, service.id);
    services_.emplace(service.id, std::move(service));
    bytes_ += *extent;
    return result;
}

Result<control::Bytes> RpcManager::acquire(const std::shared_ptr<Peer>& peer, const control::Frame& request) {
    control::Reader reader(request.payload);
    const auto wire_version = reader.u32();
    const auto target = reader.string();
    const auto name = reader.string();
    const auto request_hash = reader.u64();
    const auto response_hash = reader.u64();
    const auto size = reader.u64();
    if (!reader.done() || wire_version != version || !valid_name(target) || !valid_name(name)) {
        return std::unexpected(invalid());
    }

    for (auto& [id, service] : services_) {
        if (!service.owner->alive.load() || !service.owner->accepting.load() ||
            service.owner->name != target || service.name != name) {
            continue;
        }

        if (request_hash != service.request_hash || response_hash != service.response_hash) {
            return std::unexpected(Error{ErrorCode::TypeMismatch, ErrorOperation::Call});
        }

        if (size > service.options.request_bytes) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Call});
        }

        auto slot = std::find_if(service.slots.begin(), service.slots.end(), [](const Slot& value) {
            return value.token == 0;
        });
        if (slot == service.slots.end() || occupied_ >= config::rpc_active_limit) {
            return std::unexpected(Error{ErrorCode::QueueFull, ErrorOperation::Call});
        }

        if (next_token_ == UINT64_MAX) {
            return std::unexpected(Error{ErrorCode::IdExhausted, ErrorOperation::Call});
        }

        *slot = {next_token_++, peer, request.request, Phase::Granted};
        ++occupied_;
        auto transfer = descriptor(service, static_cast<std::size_t>(slot - service.slots.begin()));
        transfer.size = size;
        return encode_transfer(transfer);
    }

    return std::unexpected(Error{ErrorCode::NotFound, ErrorOperation::Call});
}

void RpcManager::free_slot(Slot& slot) {
    slot = {};
    --occupied_;
}

Result<void> RpcManager::transition(const std::shared_ptr<Peer>& peer, const control::Frame& request) {
    for (auto& [id, service] : services_) {
        for (std::size_t index = 0; index < service.slots.size(); ++index) {
            auto& slot = service.slots[index];
            if (!slot.token || slot.token != request.request) {
                continue;
            }

            if (request.operation == Release) {
                if (peer != slot.caller || !request.payload.empty() || slot.phase == Phase::Queued) {
                    return std::unexpected(invalid());
                }

                free_slot(slot);
                return {};
            }

            if (request.operation == Submit) {
                if (peer != slot.caller) {
                    return std::unexpected(invalid());
                }

                control::Reader reader(request.payload);
                const auto size = reader.u64();
                if (!reader.done() || size > service.options.request_bytes) {
                    return std::unexpected(invalid());
                }

                if (slot.phase != Phase::Granted) {
                    return {}; // A repeated notification cannot enqueue the call again.
                }

                if (!service.owner->alive.load()) {
                    slot.phase = Phase::Reply;
                    reply(slot.caller, Reply, slot.request,
                          std::unexpected(Error{ErrorCode::TargetExited, ErrorOperation::Call}));
                    return {};
                }

                auto transfer = descriptor(service, index);
                transfer.size = size;
                reply(service.owner, Invoke, slot.token, encode_transfer(transfer));
                // Queue construction may allocate. Keep the caller's write lease
                // until notification is retained, so failure can reclaim Granted.
                slot.phase = Phase::Queued;
                return {};
            }

            if (request.operation == Complete) {
                if (peer != service.owner) {
                    return std::unexpected(invalid());
                }

                if (slot.phase == Phase::Reply) {
                    return {}; // Duplicate completion cannot deliver a second result.
                }

                if (slot.phase != Phase::Queued) {
                    return std::unexpected(invalid());
                }

                auto result = decode_response(request.payload);
                if (!result) {
                    return std::unexpected(result.error());
                }

                if (!result->failure) {
                    control::Reader reader(result->body);
                    (void)reader.u32();
                    const auto reason = reader.string();
                    const auto size = reader.u64();
                    if (!reader.done() || reason.size() > config::rpc_reason_bytes ||
                        size > service.options.response_bytes) {
                        return std::unexpected(invalid());
                    }
                }

                if (!slot.caller->alive.load()) {
                    free_slot(slot);
                } else if (result->failure) {
                    reply(slot.caller, Reply, slot.request, std::unexpected(*result->failure));
                } else {
                    reply(slot.caller, Reply, slot.request,
                          control::Bytes(result->body.begin(), result->body.end()));
                }

                // A failed enqueue must remain Queued: confirmed callee exit can
                // still deliver TargetExited instead of stranding a silent Reply.
                if (slot.token) {
                    slot.phase = Phase::Reply;
                }

                return {};
            }

            return std::unexpected(invalid());
        }
    }

    // Tokens never repeat. A release/completion for an already retired token is harmless.
    if (request.request < next_token_) {
        return {};
    }

    return std::unexpected(invalid());
}

void RpcManager::execute(Input input) {
    auto& request = input.frame;
    auto& peer = input.peer;
    if (!peer->alive.load()) {
        return;
    }

    if (request.kind != control::Kind::Request || request.domain != domain || !request.request) {
        peer->session->close();
        return;
    }

    if (request.operation == Register || request.operation == Acquire) {
        if (request.request <= peer->last_request) {
            peer->session->close();
            return;
        }

        peer->last_request = request.request;
        if (!peer->accepting.load()) {
            reply(peer, request.operation == Register ? Register : Grant, request.request,
                  std::unexpected(Error{ErrorCode::Closed, ErrorOperation::Call}));
            return;
        }

        if (request.operation == Register) {
            reply(peer, Register, request.request, register_service(peer, request));
        } else {
            reply(peer, Grant, request.request, acquire(peer, request));
        }

        return;
    }

    if (request.operation != Submit && request.operation != Complete && request.operation != Release) {
        peer->session->close();
        return;
    }

    auto result = transition(peer, request);
    if (!result) {
        peer->session->close();
    }
}

void RpcManager::flush(Peer& peer) {
    while (!peer.output.empty() && peer.alive.load()) {
        auto sent = peer.session->send(peer.output.front());
        if (!sent) {
            if (!sent.error().is(control::ErrorCode::Backpressure)) {
                peer.output.clear();
            }

            return;
        }

        peer.output.pop_front();
    }
}

void RpcManager::recover(Peer& peer) {
    peer.output.clear();
    for (auto& [id, service] : services_) {
        for (auto& slot : service.slots) {
            if (!slot.token) {
                continue;
            }

            if (slot.caller->mid == peer.mid && slot.phase != Phase::Queued) {
                free_slot(slot);
            } else if (service.owner->mid == peer.mid && slot.phase == Phase::Queued) {
                slot.phase = Phase::Reply;
                if (!slot.caller->alive.load()) {
                    free_slot(slot);
                } else {
                    reply(slot.caller, Reply, slot.request,
                          std::unexpected(Error{ErrorCode::TargetExited, ErrorOperation::Call}));
                }
            }
        }
    }

    peer.recovered = true;
}

void RpcManager::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::vector<std::shared_ptr<Peer>> peers;
        try {
            std::deque<Input> input;
            {
                std::unique_lock lock(mutex_);
                changed_.wait_for(lock, config::rpc_control_retry, [&] {
                    return stop.stop_requested() || !input_.empty();
                });
                input.swap(input_);
                for (auto& [mid, peer] : peers_) {
                    peers.push_back(peer);
                }
            }

            for (auto& peer : peers) {
                if (!peer->alive.load() && !peer->recovered) {
                    recover(*peer);
                }
            }

            for (auto& request : input) {
                auto peer = request.peer;
                try {
                    execute(std::move(request));
                } catch (...) {
                    // Allocation failure during one admission does not invalidate
                    // unrelated services. Its confirmed exit completes rollback.
                    peer->session->close();
                }
            }

            for (auto& peer : peers) {
                flush(*peer);
            }

            std::erase_if(services_, [&](const auto& entry) {
                const auto& service = entry.second;
                const bool idle =
                    std::all_of(service.slots.begin(), service.slots.end(), [](const Slot& slot) {
                        return slot.token == 0;
                    });
                if (!service.owner->alive.load() && idle) {
                    bytes_ -= service.pool->extent();
                    return true;
                }

                return false;
            });
            std::lock_guard lock(mutex_);
            std::erase_if(peers_, [](const auto& entry) {
                return entry.second->recovered;
            });
        } catch (...) {
            // Existing worker-boundary policy: isolate affected sessions rather than
            // letting an exception escape a background thread and kill the central.
            for (auto& peer : peers) {
                peer->session->close();
            }
        }
    }
}
}
