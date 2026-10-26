#include <vinc/module/detail/mid_allocator.hpp>
#include <algorithm>
#include <array>
#include <future>
#include <iostream>

int main() {
    using namespace vinc;
    static_assert(sizeof(MID) == 8);
    static_assert(max_mid == (MID{1} << 63) - 2);
    static_assert(!valid_mid(no_owner));
    static_assert(!valid_mid(central_owner));
    static_assert(!valid_mid(MID{1} << 63));
    constexpr std::size_t remaining_ids = 8;
    std::atomic<MID> counter{max_mid - remaining_ids + 1};
    std::array<std::future<Result<MID>>, remaining_ids> workers;
    for (auto& worker : workers) {
        worker = std::async(std::launch::async, [&] { return detail::allocate_mid(counter); });
    }
    std::array<MID, remaining_ids> issued{};
    for (std::size_t index = 0; index < workers.size(); ++index) {
        auto result = workers[index].get();
        if (!result) {
            return 1;
        }
        issued[index] = *result;
    }
    std::sort(issued.begin(), issued.end());
    for (std::size_t index = 0; index < issued.size(); ++index) {
        if (issued[index] != max_mid - remaining_ids + 1 + index) {
            return 2;
        }
    }
    if (counter.load() != central_owner || detail::allocate_mid(counter) ||
        counter.load() != central_owner) {
        return 4;
    }
    for (const auto invalid : {invalid_mid, central_owner, MID{1} << 63, UINT64_MAX}) {
        counter.store(invalid);
        auto result = detail::allocate_mid(counter);
        if (result || !result.error().is(module_system::ErrorCode::MidExhausted) ||
            !result.error().is_operation(module_system::ErrorOperation::AllocateMid) ||
            counter.load() != invalid) {
            return 3;
        }
    }
    std::cout << "PASS: MID boundaries, concurrent final allocation and no wrap\n";
}
