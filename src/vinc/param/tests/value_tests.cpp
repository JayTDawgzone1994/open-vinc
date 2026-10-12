#include "support.hpp"
#include <vinc/param/detail/protocol.hpp>
#include <vinc/param/detail/persistence.hpp>
#include <vinc/osal/error.hpp>
#include <fstream>
#include <iostream>
#include <limits>

namespace {
using namespace vinc;
using namespace param_test;
using namespace param;
const std::vector<Value> values{true,
                                std::int8_t{-128},
                                std::uint8_t{255},
                                std::int16_t{-32768},
                                std::uint16_t{65535},
                                (std::numeric_limits<std::int32_t>::min)(),
                                (std::numeric_limits<std::uint32_t>::max)(),
                                (std::numeric_limits<std::int64_t>::min)(),
                                (std::numeric_limits<std::uint64_t>::max)(),
                                1.2345678f,
                                1.2345678901234567,
                                std::string("UTF-8 \xe4\xb8\xad\xe6\x96\x87\n\"\\")};

void value_and_wire() {
    Snapshot source;
    source.revision = 9;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto& value = values[index];
        check(take(parse_value(type_of(value), value_text(value))) == value, "numeric/string roundtrip");
        source.entries.emplace("camera.p" + std::to_string(index),
                               Entry{value, value, Permission::Open, 9, 1234});
    }
    auto encoded = take(param::detail::encode_snapshot(source));
    auto decoded = take(param::detail::decode_snapshot(encoded));
    check(decoded.revision == source.revision && decoded.entries == source.entries, "snapshot roundtrip");
    auto malformed = encoded;
    malformed[3] = std::byte{99};
    check(!param::detail::decode_snapshot(malformed), "reject wire version");
    malformed = encoded;
    malformed.pop_back();
    check(!param::detail::decode_snapshot(malformed), "reject truncation");
    malformed = encoded;
    malformed.push_back(std::byte{0});
    check(!param::detail::decode_snapshot(malformed), "reject trailing bytes");
    check(!parse_value(Type::UInt8, "256"), "uint8 range");
    check(!parse_value(Type::UInt64, "-1"), "unsigned sign");
    check(!parse_value(Type::Bool, "1"), "bool syntax");
    check(!validate_value(std::string("\xc0\xaf")), "UTF-8 overlong");
    check(!validate_value(std::string(config::param_string_bytes + 1, 'x')), "string bound");
    take(validate_value(std::string(config::param_string_bytes, 'x')));
    check(valid_name("m." + std::string(config::param_name_bytes - 2, 'x')), "full name exact bound");
    check(!valid_name("m." + std::string(config::param_name_bytes - 1, 'x')), "full name over bound");
    for (const auto* name : {"m.x.y", "m/x", ".x", "m.", "m.\xe4\xb8\xad", "m. space"}) {
        check(!valid_name(name), "invalid qualified name");
    }
    const auto cause = Error{osal::ErrorCode::PermissionDenied, osal::ErrorOperation::OpenSharedMemory};
    const auto failure = wrap_error(ErrorCode::StorageFailed, ErrorOperation::Pump, cause);
    auto reply = take(param::detail::decode_response(param::detail::encode_failure(failure)));
    check(reply.failure && reply.failure->causes().size() == 1 &&
              reply.failure->causes()[0].domain == ErrorDomain::Osal,
          "cross-process cause chain");
    auto unknown = Error::from_raw(ErrorDomain::Param, 9999, 8888);
    const auto restored = take(param::detail::decode_error(param::detail::encode_error(unknown)));
    check(restored.code == 9999 && restored.operation == 8888, "unknown codes retained");
}

void persistence() {
    Directory temporary;
    const auto file = temporary.path / "params.toml";
    param::detail::Persistence store;
    check(take(store.load(file)).empty() && std::filesystem::exists(file), "first run creates file");
    param::detail::SavedMap saved;
    for (std::size_t index = 0; index < values.size(); ++index) {
        saved.emplace("camera.p" + std::to_string(index),
                      param::detail::SavedEntry{values[index], values[index]});
    }
    take(store.write(saved));
    param::detail::Persistence reader;
    const auto loaded = take(reader.load(file));
    check(loaded.size() == saved.size(), "all TOML types persisted");
    for (const auto& [name, item] : saved) {
        check(loaded.at(name).value == item.value && loaded.at(name).default_value == item.default_value,
              "TOML value/default precision and uint64 range");
    }
    // Replacing an existing file must work on Windows as well as POSIX.
    saved.begin()->second.value = false;
    take(store.write(saved));
    check(take(reader.load(file)).begin()->second.value == Value(false), "atomic file replacement");
    {
        std::ofstream stream(file, std::ios::trunc);
        stream << "format = 1\n[params.\"camera.gain\"]\ntype = \"int32\"\ndefault = 2\nvalue = 8\n";
    }
    check(std::get<std::int32_t>(take(reader.load(file)).at("camera.gain").value) == 8, "manual TOML edit");
    for (const auto* text : {"[ broken", "format = 2\n[params]",
                             "format=1\n[params.\"m.x\"]\ntype=\"uint8\"\ndefault=0\nvalue=256"}) {
        {
            std::ofstream stream(file, std::ios::trunc);
            stream << text;
        }
        auto invalid = reader.load(file);
        check(!invalid && invalid.error().is(ErrorCode::InvalidFile), "invalid TOML/schema rejected");
        check(!std::filesystem::exists(file), "invalid whole file deleted");
    }
    take(reader.write(saved));
    // A destination directory is an I/O failure, never proof of corrupt TOML.
    const auto blocked = temporary.path / "blocked";
    std::filesystem::create_directory(blocked);
    param::detail::Persistence unavailable;
    check(!unavailable.load(blocked) && !unavailable.enabled(), "disable inaccessible persistence");
    check(std::filesystem::is_directory(blocked), "I/O failure must not delete destination");
    // Failing temp-file creation preserves the last complete saved snapshot.
    std::filesystem::create_directory(file.string() + ".tmp");
    check(!reader.write({}) && !reader.enabled(), "write failure disables persistence");
    param::detail::Persistence verify;
    check(take(verify.load(file)).size() == saved.size(), "failed save preserves old file");
}
}

int main() {
    try {
        value_and_wire();
        persistence();
        std::cout << "PASS: Param values, wire, TOML persistence and failure preservation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
