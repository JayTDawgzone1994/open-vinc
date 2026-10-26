// Compile and inspect the merged catalog, including cross-directory nesting.
#include <vinc_messages/message_types.hpp>
#include <cstdlib>
#include <iostream>
#include <type_traits>

int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }

    bool found_frame = false;
    bool found_extra = false;
    vinc::message::visit_types([&]<class T> {
        if constexpr (std::is_same_v<T, vinc::ExternalFrame>) {
            found_frame = true;
        }
        if (T::message_name == "ExternalExtra") {
            found_extra = true;
        }
    });
    auto message = vinc::make_message<vinc::ExternalFrame>();
    message->data().leaf.data().value = 42;
    const auto revision = std::atoi(argv[1]);
    const bool expect_extra = std::atoi(argv[2]) != 0;
    if (!found_frame || found_extra != expect_extra || vinc::ExternalLeaf::revision != revision ||
        message->data().leaf.data().value.get() != 42) {
        std::cerr << "Incomplete or stale generated message catalog\n";
        return 1;
    }
    return 0;
}
