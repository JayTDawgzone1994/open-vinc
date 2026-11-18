#include <vinc_messages/message_names.hpp>

int main() {
    using namespace vinc::message;
    if (message_names.size() != 3 || !name_for_hash(0).empty() || hash_for_name("missing") != 0) {
        return 1;
    }

    for (const auto& entry : message_names) {
        if (entry.hash == 0 || entry.name.empty() || name_for_hash(entry.hash) != entry.name ||
            hash_for_name(entry.name) != entry.hash) {
            return 1;
        }
    }

    return 0;
}
