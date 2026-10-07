// Compile actual generator output: successful generation alone cannot validate
// C++ name lookup or the meaning of emitted literals.
#include <vinc_messages/all_messages.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

using vinc::Constants;
static_assert(Constants::decimal == 10.0);
static_assert(Constants::eight == 8.0);
static_assert(Constants::fraction == 0.10000000000000001);
static_assert(Constants::smallest == std::numeric_limits<double>::denorm_min());
static_assert(Constants::largest == std::numeric_limits<double>::max());
static_assert(Constants::single == 0.1f);
static_assert(Constants::smallest_single == std::numeric_limits<float>::denorm_min());
static_assert(Constants::largest_single == std::numeric_limits<float>::max());
static_assert(Constants::embedded == std::string_view("a\0b", 3));
static_assert(Constants::digits == std::string_view("\0" "127", 4));
static_assert(Constants::escapes == "\n\r\t\"\\");
static_assert(Constants::empty.empty());
static_assert(Constants::placeholders == "${VALUE} ${DO_MESSAGE_CLASS} ${DO_INCLUDE_DEPENDENT_MESSAGES}");
static_assert(Constants::utf8 == "訊息🚀");
static_assert(Constants::Base == 7);

int main() {
    if (!std::signbit(Constants::negative_zero) || !std::isnan(Constants::not_a_number)) {
        std::cerr << "floating sign/NaN not preserved\n";
        return 1;
    }

    auto message = vinc::make_message<vinc::Names>();
    if (!message) {
        std::cerr << "message factory returned no object\n";
        return 1;
    }

    message->data().Result = 1;
    message->data().b = 2;
    message->data().access = 3;
    message->data().Point.data().x = 4;
    auto copied = message->data();
    auto assigned = copied.assign(message->data());
    if (!assigned || copied.Result.get() != 1 || copied.Point.data().x.get() != 4) {
        std::cerr << "generated field name lookup/assignment failed\n";
        return 1;
    }

    const auto moved = std::move(*message);
    if (moved.data().b.get() != 2 || moved.data().access.get() != 3) {
        std::cerr << "generated move constructor failed\n";
        return 1;
    }
    std::cout << "PASS: generated constants and names\n";
}
