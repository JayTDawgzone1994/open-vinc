#include <vinc/ui/server.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
constexpr auto request_timeout = std::chrono::seconds{5};

void check(bool condition, const char* description) {
    if (!condition) {
        throw std::runtime_error(description);
    }
}

template <class T> T take(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }

    return std::move(*result);
}

struct Fixture {
    std::filesystem::path root;
    explicit Fixture(const std::filesystem::path& source) {
        root = source.parent_path() / ("server-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(std::filesystem::create_directory(root), "exclusive fixture directory");
        std::filesystem::copy(source / "app", root / "app", std::filesystem::copy_options::recursive);
        std::filesystem::copy(source / "plugins", root / "plugins", std::filesystem::copy_options::recursive);
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    check(static_cast<bool>(output), "write fixture");
}

void package(const std::filesystem::path& root, const char* directory, const char* id, int version = 1) {
    const auto folder = root / "plugins" / directory;
    std::filesystem::create_directory(folder);
    const Json manifest{{"id", id}, {"name", "External"}, {"version", "1"}, {"entry", "index.js"},
                        {"apiVersion", version}, {"official", true},
                        {"panels", Json::array({{{"id", "main"}, {"title", "External"}, {"multiple", false}}})}};
    write(folder / "plugin.json", manifest.dump());
    write(folder / "index.js", "export const example = 1;");
}

Json get_catalog(httplib::Client& client) {
    auto response = client.Get("/api/ui");
    check(response && response->status == 200, "catalog request");
    return Json::parse(response->body);
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "dist path argument");
        Fixture fixture(std::filesystem::absolute(argv[1]));
        package(fixture.root, "external", "test.external");
        package(fixture.root, "future", "test.future", 2);
        package(fixture.root, "duplicate1", "test.duplicate");
        package(fixture.root, "duplicate2", "test.duplicate");
        auto server = take(vinc::ui::Server::start({fixture.root, "localhost", 0}));
        const auto port = server->port();
        httplib::Client client("127.0.0.1", port);
        client.set_read_timeout(request_timeout);
        const auto catalog = get_catalog(client);
        check(catalog["plugins"].size() == 2, "duplicate and future plugins rejected");
        check(catalog["diagnostics"].size() == 3, "rejection diagnostics retained");
        const auto external = catalog["plugins"][0];
        const auto official = catalog["plugins"][1];
        check(external["id"] == "test.external" && external["official"] == false, "manifest cannot self-assert trust");
        check(official["id"] == "vinc.example" && official["official"] == true, "build-pinned official hash");
        auto index = client.Get("/");
        check(index && index->status == 200 && index->get_header_value("Content-Security-Policy").find("frame-ancestors 'none'") != std::string::npos, "workspace and security headers");
        const auto entry = external["entry"].get<std::string>();
        auto script = client.Get(entry);
        check(script && script->body == "export const example = 1;", "exact package bytes");
        write(fixture.root / "plugins/external/index.js", "export const example = 2;");
        auto unchanged = client.Get(entry);
        check(unchanged && unchanged->body == script->body, "startup snapshot immutable");
        auto rejected_host = client.Get("/api/ui", httplib::Headers{{"Host", "attacker.example"}});
        check(rejected_host && rejected_host->status == 403, "foreign host refused");
        auto rejected_origin = client.Get("/api/ui", httplib::Headers{{"Origin", "http://attacker.example"}});
        check(rejected_origin && rejected_origin->status == 403, "foreign origin refused");
        auto missing = client.Get("/plugins/../secret.txt");
        check(missing && missing->status == 404, "no arbitrary filesystem route");
        auto post = client.Post("/api/ui");
        check(post && post->status == 405, "read-only HTTP API");
        auto occupied = vinc::ui::Server::start({fixture.root, "127.0.0.1", port});
        check(!occupied && occupied.error().is(vinc::ui::ErrorCode::BindFailed), "port conflict reported");
        server->stop();
        check(!server->running(), "stop joins listener");
        server = take(vinc::ui::Server::start({fixture.root, "127.0.0.1", port}));
        auto updated = get_catalog(client);
        check(updated["plugins"][0]["digest"] != external["digest"], "content update changes approval identity");
        server.reset();
        write(fixture.root / "plugins/vinc.example/index.js", "export const changed = true;");
        server = take(vinc::ui::Server::start({fixture.root, "127.0.0.1", port}));
        check(get_catalog(client)["plugins"][1]["official"] == false, "modified official package requires trust");
        server.reset();
        auto invalid = vinc::ui::Server::start({fixture.root / "missing", "127.0.0.1", 0});
        check(!invalid && invalid.error().is(vinc::ui::ErrorCode::InvalidPackage), "missing app fails with UI error");
        std::cout << "UI server tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
