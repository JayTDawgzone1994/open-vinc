#pragma once

#include <vinc_messages/UserExampleFrame.hpp>

namespace user_example {
// The same payload helper can fill local storage or a Publisher's bounded loan.
vinc::Result<void> fill(vinc::UserExampleFrame& message, std::uint64_t sequence);
vinc::Result<void> demonstrate_storage();
void describe(const vinc::UserExampleFrame& message);
}
