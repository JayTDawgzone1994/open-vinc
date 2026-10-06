#include "runtime_services_plugin.hpp"
#include <vinc/contract.hpp>
#include <vinc/module/detail/contract_report.hpp>
#include <vinc/module/detail/runtime_services.hpp>
#include <vinc/module/error.hpp>
#include <cstdlib>
#include <exception>
#include <string_view>

namespace {
constexpr vinc::MID fixture_mid = 456;
constexpr int unexpected_termination = 90;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        return 1;
    }

    std::set_terminate([] {
        std::_Exit(unexpected_termination);
    });
    auto storage = vinc::detail::ContractReportStorage::open(argv[1], fixture_mid);
    if (!storage) {
        return 2;
    }

    const std::string_view mode = argv[2];
    const auto* plugin_a = bridge_plugin_a();
    const auto* plugin_b = bridge_plugin_b();
    {
        vinc::detail::HostRuntimeServices host(**storage);
        if (!host.bind_host()) {
            return 3;
        }

        if (mode == "unbound") {
            plugin_a->fail();
        }

        if (!plugin_a->bind(host.services())) {
            return 4;
        }

        if (mode == "second-unbound") {
            plugin_b->fail();
        }

        if (!plugin_b->bind(host.services())) {
            return 5;
        }

        if (mode == "plugin-a") {
            plugin_a->fail();
        } else if (mode == "plugin-b") {
            plugin_b->fail();
        } else if (mode == "clear-a-then-b") {
            plugin_a->clear();
            plugin_b->fail();
        } else if (mode == "cleared") {
            plugin_a->clear();
            plugin_a->fail();
        } else if (mode == "host") {
            plugin_a->clear();
            vinc::contract_index_violation("host", 23, 7);
        } else if (mode == "validation") {
            auto duplicate = host.bind_host();
            if (duplicate || !duplicate.error().is(vinc::module_system::ErrorCode::ServiceFailed) ||
                duplicate.error().causes().empty() ||
                duplicate.error().causes().front().domain != vinc::ErrorDomain::Common) {
                return 6;
            }
        } else if (mode != "teardown") {
            return 7;
        }

        // Both images withdraw before the Host table and storage can be destroyed.
        plugin_a->clear();
        plugin_b->clear();
    }

    if (mode == "teardown") {
        plugin_a->fail();
    }
    return 0;
}
