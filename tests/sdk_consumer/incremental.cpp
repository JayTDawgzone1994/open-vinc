#include <vinc/vinc.hpp>
#include "value.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }

    const int expected = argv[1][0] - '0';
    if (sdk_incremental_value != expected) {
        return 2;
    }

    return 0;
}
