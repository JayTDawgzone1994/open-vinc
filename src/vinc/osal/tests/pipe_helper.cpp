// Child endpoint-adoption fixture for Pipe tests. Mode arguments select transfer,
// closure or failure behavior; the framework transport is not mocked.

#include <vinc/osal/process_pipe.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace vinc::osal;
using namespace std::chrono_literals;

namespace {
constexpr auto transfer_timeout = 10s;
constexpr auto endpoint_wait_timeout = 5s;
// Keep the helper alive while its parent tests cancellation, closure or termination.
constexpr auto parent_control_window = 30s;
constexpr auto delayed_echo_start = 250ms;
constexpr std::size_t echo_chunk_bytes = 1024;
} // namespace

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}

IoCompletion completion(PipeService& service) {
    std::array<IoCompletion, 1> output;
    take(service.wait_completions(output, std::chrono::steady_clock::now() + transfer_timeout));
    return std::move(output[0]);
}

void send(PipeService& service, PipeWriter& pipe, std::span<const std::byte> data) {
    while (!data.empty()) {
        IoBuffer buffer(data.size());
        std::copy(data.begin(), data.end(), buffer.bytes().begin());
        take(service.submit_write(pipe, std::move(buffer),
                                  std::chrono::steady_clock::now() + transfer_timeout));
        auto result = completion(service);
        if (result.status != IoStatus::Completed || !result.transferred) {
            throw std::runtime_error("write failed");
        }
        data = data.subspan(result.transferred);
    }
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            return 80;
        }

        const std::string mode = argv[1];
        PipeServiceOptions service_options;
        if (mode == "reject-capacity") {
            service_options.max_endpoints = 1;
        }

        auto service = take(PipeService::create(service_options));
        if (mode == "reject-capacity") {
            auto inherited = take_inherited_pipes(service);
            if (inherited || !inherited.error().is(vinc::osal::ErrorCode::ResourceExhausted)) {
                return 104;
            }

            const auto& failure = inherited.error();
            if (!failure.is_operation(ErrorOperation::AdoptPipe) || failure.causes().size() != 1 ||
                failure.causes().front().domain != vinc::ErrorDomain::Osal ||
                failure.causes().front().operation != static_cast<std::int32_t>(ErrorOperation::CreatePipe)) {
                return 106;
            }

            auto again = take_inherited_pipes(service);
            if (again || !again.error().is(vinc::osal::ErrorCode::Busy)) {
                return 105;
            }

            std::this_thread::sleep_for(parent_control_window);
            return 0;
        }

        if (mode == "no-bootstrap") {
            auto missing = take_inherited_pipe(service);
            return !missing && missing.error().is(vinc::osal::ErrorCode::NotFound) ? 0 : 81;
        }

        if (mode == "many" || mode == "single-source") {
            auto inherited = take(take_inherited_pipes(service));
            if (inherited.size() != (mode == "many" ? 64u : 1u)) {
                return 91;
            }

            if (take_inherited_pipes(service)) {
                return 92;
            }

            for (auto& item : inherited) {
                if (!std::holds_alternative<PipeWriter>(item.endpoint)) {
                    return 93;
                }
                auto& writer = std::get<PipeWriter>(item.endpoint);
                const std::array data{static_cast<std::byte>(item.slot & 255)};
                send(service, writer, data);
                writer.close();
            }

            return service.shutdown_until(std::chrono::steady_clock::now() + endpoint_wait_timeout) ? 0 : 94;
        }

        if (mode == "single-sink") {
            auto inherited = take(take_inherited_pipes(service));
            if (inherited.size() != 1 || inherited[0].slot != 42 ||
                !std::holds_alternative<PipeReader>(inherited[0].endpoint)) {
                return 95;
            }
            auto& reader = std::get<PipeReader>(inherited[0].endpoint);
            take(service.submit_read(reader, IoBuffer(1),
                                     std::chrono::steady_clock::now() + endpoint_wait_timeout));
            auto value = completion(service);
            if (value.status != IoStatus::Completed || value.transferred != 1 ||
                value.buffer.bytes()[0] != std::byte{42}) {
                return 96;
            }
            take(service.submit_read(reader, IoBuffer(1),
                                     std::chrono::steady_clock::now() + endpoint_wait_timeout));
            if (completion(service).status != IoStatus::EndOfStream) {
                return 97;
            }

            return service.shutdown_until(std::chrono::steady_clock::now() + endpoint_wait_timeout) ? 0 : 98;
        }

        if (mode == "multi") {
            auto inherited = take(take_inherited_pipes(service));
            if (inherited.size() != 4) {
                return 99;
            }

            for (const auto slots : {std::array<std::uint32_t, 2>{17, 33}, {71, 99}}) {
                PipeReader* reader = nullptr;
                PipeWriter* writer = nullptr;
                for (auto& item : inherited) {
                    if (item.slot == slots[0]) {
                        reader = std::get_if<PipeReader>(&item.endpoint);
                    }

                    if (item.slot == slots[1]) {
                        writer = std::get_if<PipeWriter>(&item.endpoint);
                    }
                }

                if (!reader || !writer) {
                    return 100;
                }
                take(service.submit_read(*reader, IoBuffer(1),
                                         std::chrono::steady_clock::now() + endpoint_wait_timeout));
                auto value = completion(service);
                if (value.status != IoStatus::Completed || value.transferred != 1) {
                    return 101;
                }
                send(service, *writer, value.buffer.bytes().first(1));
                take(service.submit_read(*reader, IoBuffer(1),
                                         std::chrono::steady_clock::now() + endpoint_wait_timeout));
                if (completion(service).status != IoStatus::EndOfStream) {
                    return 102;
                }

                const std::array trailer{std::byte{0xee}};
                send(service, *writer, trailer); // reverse still usable after EOF
                writer->close();
            }

            return service.shutdown_until(std::chrono::steady_clock::now() + endpoint_wait_timeout) ? 0 : 103;
        }

        auto pipe = take(take_inherited_pipe(service));
        if (take_inherited_pipe(service)) {
            return 82;
        }

        if (mode == "hold") {
            std::this_thread::sleep_for(parent_control_window);
            return 0;
        }

        if (mode == "close") {
            pipe.close();
            auto stopped = service.shutdown_until(std::chrono::steady_clock::now() + endpoint_wait_timeout);
            if (!stopped) {
                return 83;
            }

            std::this_thread::sleep_for(parent_control_window);
            return 0;
        }

        if (mode == "descendant") {
            auto processes = take(ProcessService::create());
            SpawnOptions options;
            options.executable = std::filesystem::absolute(argv[0]);
            options.arguments = {"no-bootstrap"};
            options.stderr_mode = StdioMode::Null;
            options.stdout_mode = StdioMode::Null;
            auto child = take(processes.spawn(options));
            if (take(child.wait_until(std::chrono::steady_clock::now() + endpoint_wait_timeout)).value != 0) {
                return 84;
            }

            auto stopped = processes.shutdown_until(std::chrono::steady_clock::now() + endpoint_wait_timeout);
            if (!stopped) {
                return 85;
            }
        }

        if (mode == "delayed-echo") {
            std::this_thread::sleep_for(delayed_echo_start);
        }

        for (;;) {
            IoBuffer buffer(echo_chunk_bytes);
            take(service.submit_read(pipe.reader, std::move(buffer),
                                     std::chrono::steady_clock::now() + transfer_timeout));
            auto received = completion(service);
            if (received.status == IoStatus::EndOfStream) {
                break;
            }

            if (received.status != IoStatus::Completed || !received.transferred) {
                return 86;
            }
            send(service, pipe.writer, received.buffer.bytes().first(received.transferred));
        }
        pipe.close();
        return service.shutdown_until(std::chrono::steady_clock::now() + endpoint_wait_timeout) ? 0 : 87;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 90;
    }
}
