#include <vinc/contract.hpp>
#include <vinc/detail/runtime_services.hpp>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>
#include <string_view>

namespace {
bool reject_allocations = false;
}

// The isolated child arms this only after initialization; fatal dispatch must
// reach the callback even when ordinary C++ allocation is unavailable.
void* operator new(std::size_t size) {
    if (reject_allocations) {
        std::_Exit(93);
    }
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {
using namespace vinc;
using namespace vinc::detail;

void returning_report(void*, const ContractViolation&) noexcept {}

void allocation_free_report(void* context, const ContractViolation& violation) noexcept {
    const auto valid = context == &reject_allocations && violation.has_index == 1 &&
                       violation.index == 10 && violation.size == 3 && violation.line > 0 &&
                       std::string_view(violation.reason) == "fallback probe" &&
                       std::string_view(violation.file).find("runtime_services_tests.cpp") != std::string_view::npos;
    std::_Exit(valid ? 80 : 1);
}

bool validation() {
    RuntimeServices table;
    auto invalid = bind_runtime_services(table);
    if (invalid || !invalid.error().is(common::ErrorCode::InvalidRuntimeServices)) {
        return false;
    }

    table.report_contract = returning_report;
    table.version = runtime_services_version + 1;
    auto incompatible = bind_runtime_services(table);
    if (incompatible || !incompatible.error().is(common::ErrorCode::IncompatibleRuntimeServices)) {
        return false;
    }

    if (error_to_string(incompatible.error()).find("Incompatible runtime services") == std::string::npos) {
        return false;
    }

    table.version = runtime_services_version;
    table.size = sizeof(RuntimeServices) - 1;
    auto short_table = bind_runtime_services(table);
    if (short_table || !short_table.error().is(common::ErrorCode::InvalidRuntimeServices)) {
        return false;
    }

    table.size = sizeof(RuntimeServices);
    if (!bind_runtime_services(table)) {
        return false;
    }

    auto duplicate = bind_runtime_services(table);
    if (duplicate || !duplicate.error().is(common::ErrorCode::RuntimeServicesAlreadyBound)) {
        return false;
    }

    clear_runtime_services();
    auto rebound = bind_runtime_services(table);
    return !rebound && rebound.error().is(common::ErrorCode::RuntimeServicesAlreadyBound);
}

struct ExitSentinel {
    ~ExitSentinel() {
        if (reject_allocations) {
            std::_Exit(92);
        }
    }
};
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }

    const std::string_view mode = argv[1];
    if (mode == "validation") {
        return validation() ? 0 : 1;
    }

    // Distinct sentinels prove direct fallback bypasses user termination and
    // exit cleanup rather than treating an unrelated crash as a passing probe.
    std::set_terminate([] {
        std::_Exit(90);
    });
    std::atexit([] {
        std::_Exit(91);
    });
    ExitSentinel sentinel;
    RuntimeServices table;
    table.report_contract = returning_report;
    if (mode == "bound-no-allocation") {
        table.context = &reject_allocations;
        table.report_contract = allocation_free_report;
        if (!bind_runtime_services(table)) {
            std::_Exit(1);
        }
    } else if (mode == "returned" || mode == "cleared") {
        if (!bind_runtime_services(table)) {
            std::_Exit(1);
        }
        if (mode == "cleared") {
            clear_runtime_services();
        }
    } else if (mode != "unbound") {
        std::_Exit(1);
    }

    reject_allocations = true;
    contract_index_violation("fallback probe", 10, 3);
}
