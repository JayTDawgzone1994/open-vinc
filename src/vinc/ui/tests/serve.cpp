#include <vinc/ui/server.hpp>
#include <vinc/osal/path.hpp>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }

    auto path = vinc::osal::path_from_utf8(argv[1]);
    if (!path) {
        return 2;
    }

    auto server = vinc::ui::Server::start({*path, "127.0.0.1", 0});
    if (!server) {
        std::cerr << vinc::error_to_string(server.error()) << '\n';
        return 1;
    }

    std::cout << (*server)->port() << std::endl;
    std::string line;
    std::getline(std::cin, line);
    return 0;
}
