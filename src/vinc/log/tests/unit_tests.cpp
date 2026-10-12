#include "support.hpp"
#include <vinc/log/detail/files.hpp>
#include <vinc/log/detail/transport.hpp>
#include <fstream>
#include <iostream>
using namespace vinc;
using namespace log_test;

namespace {
struct Capture final : log::detail::Access {
    std::string text;

    bool enabled(log::Level level) const noexcept override {
        return level <= log::Level::Info;
    }

    Result<void> write(log::Level, std::string_view value) override {
        text = value;
        return {};
    }
};

std::uint64_t size(const std::filesystem::path& root) {
    std::uint64_t bytes = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            // Windows directory metadata can lag a still-open writer. Read flushed bytes.
            std::ifstream input(entry.path(), std::ios::binary);
            bytes += std::string(std::istreambuf_iterator<char>(input), {}).size();
        }
    }
    return bytes;
}

void test_api() {
    log::Logger logger;
    check(!logger.info("unbound"), "unbound returns Closed");
    Capture capture;
    log::detail::ClientAccess::bind(logger, capture);
    take(logger.info("answer {}", 42));
    check(capture.text == "answer 42", "fmt call-time formatting");
    take(logger.debug("ignored"));
    check(capture.text == "answer 42", "source filtering precedes formatting");
    auto too_large = logger.info("{}", std::string(config::log_message_bytes + 1, 'x'));
    check(!too_large && too_large.error().is(log::ErrorCode::TooLarge), "oversized message rejected");
    log::detail::Message original{log::Level::Warn, -1, 3, 2, "text"};
    auto wire = take(log::detail::encode_message(original));
    auto decoded = take(log::detail::decode_message(wire));
    check(decoded.text == original.text && decoded.timestamp_us == -1 && decoded.sequence == 3,
          "wire round trip");
    wire.payload[3] = std::byte{99};
    check(!log::detail::decode_message(wire), "version mismatch");
    wire = take(log::detail::encode_message(original));
    wire.payload.pop_back();
    check(!log::detail::decode_message(wire), "truncated payload");
    wire = take(log::detail::encode_message(original));
    wire.payload.push_back(std::byte{});
    check(!log::detail::decode_message(wire), "trailing bytes");
    check(error_to_string(Error{log::ErrorCode::Backpressure, log::ErrorOperation::Write})
                  .find("Backpressure") != std::string::npos,
          "static catalog");
}

void test_files() {
    Directory directory;
    constexpr std::uint64_t file_limit = 16;
    constexpr std::uint64_t total_limit = 32;
    log::detail::Files files;
    take(files.configure(directory.path, {file_limit, total_limit}));
    take(files.append("2026-10-08", "123456789012345"));
    take(files.flush());
    check(size(directory.path) == file_limit, "encoded newline counted exactly");
    take(files.append("2026-10-09", "abcdefghijklmno"));
    take(files.append("2026-10-07", "backward-clock"));
    take(files.flush());
    check(size(directory.path) <= total_limit, "global retention across dates and clock rollback");
    check(!std::filesystem::exists(directory.path / "2026-10-08"),
          "oldest sequence removed, empty date cleaned");
    auto large = files.append("2026-10-07", std::string(file_limit, 'x'));
    check(!large && large.error().is(log::ErrorCode::TooLarge), "no infinite rotation on oversized record");
    take(files.configure(directory.path, {file_limit, total_limit}));
    take(files.append("2026-10-10", "restart"));
    take(files.flush());
    check(size(directory.path) <= total_limit, "reopen rescans actual files without overwrite");
    auto invalid = files.configure(directory.path, {total_limit, file_limit});
    check(!invalid, "invalid capacity rejected");
    const auto blocker = directory.path / "blocked";
    {
        std::ofstream stream(blocker);
        stream << 'x';
    }
    check(!files.configure(blocker, {}), "filesystem failure is Result");
    Directory deletion;
    take(files.configure(deletion.path, {file_limit, file_limit}));
    take(files.append("2026-10-08", "123456789012345"));
    take(files.flush());
    // A foreign replacement is never recursively deleted by retention.
    take(files.configure(deletion.path, {file_limit, file_limit}));
    auto old = deletion.path / "2026-10-08" / "vinc-00000000000000000001.log";
    std::filesystem::remove(old);
    std::filesystem::create_directory(old);
    auto rejected = files.append("2026-10-09", "replacement");
    check(!rejected && std::filesystem::is_directory(old), "retention failure preserves foreign replacement");
    Directory equal;
    take(files.configure(equal.path, {file_limit, file_limit}));
    take(files.append("2026-10-08", "123456789012345"));
    take(files.append("2026-10-08", "another"));
    take(files.flush());
    check(size(equal.path) <= file_limit, "equal total and file limits rotate and retire active file");
}
}

int main() {
    try {
        test_api();
        test_files();
        std::cout << "Log unit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
