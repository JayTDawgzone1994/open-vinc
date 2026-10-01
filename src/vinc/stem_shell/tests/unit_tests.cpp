#include <stem_shell/command_router.hpp>
#include <stem_shell/stem_shell.hpp>
#include <sstream>
#include <iostream>
#include <stdexcept>

using namespace vinc;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        auto args = shell::parse_line(R"(node open "D:\My Project\file.txt" '' 'a b' a\ b "say \"hi\"")");
        check(args && args->arguments == std::vector<std::string>{"node", "open", "D:\\My Project\\file.txt", "", "a b", "a b", "say \"hi\""}, "quotes, empty arguments, escapes and Windows paths");
        check(!shell::parse_line("node \"unfinished"), "unfinished quote rejected");
        check(bool(shell::parse_line("node \"unfinished", true)), "unfinished completion allowed");
        check(!shell::parse_line(std::string(4097, 'x')), "bounded command");
        auto url = shell::parse_line("node https://example.test/a#b");
        check(url && url->arguments[1] == "https://example.test/a#b", "interactive argument content preserved");
        for (const std::string value : {"", "one", "a b", "a'b", "a\"b", "\\\\host\\dir\\", "中文"}) {
            auto roundtrip = shell::parse_line(shell::quote_argument(value));
            check(roundtrip && roundtrip->arguments == std::vector<std::string>{value}, "completion quote roundtrip");
        }
        shell::LineEditor editor;
        check(editor.append("hello 中文"), "UTF-8 append"); editor.backspace();
        check(editor.line() == "hello 中", "backspace removes complete Chinese code point");
        check(editor.submit() == "hello 中", "submit preserves bytes");
        editor.append("draft"); editor.older(); check(editor.line() == "hello 中", "first up is newest history");
        editor.newer(); check(editor.line() == "draft", "down restores unsubmitted draft");
        editor.submit(); editor.older(); check(editor.line() == "draft", "history cursor reset after submit");
        editor.replace("abc中文"); check(editor.visible_tail(4) == "中文", "display tail keeps UTF-8 boundaries");
        ModuleManager manager;
        std::ostringstream out;
        { StemShell first(manager, out); check(first.process_line("unknown") != 0 && first.get_last_error().find("Unknown") != std::string::npos, "structured command error"); }
        { StemShell second(manager, out); check(second.process_line("help") == 0, "Shell recreation has no stale callbacks"); }
        check(!manager.has_micro_program("help") && !manager.has_micro_program("exit"), "manager does not own Shell callbacks");
        check(manager.register_micro_program("exit", [](auto&) { return 0; }) != 0, "builtin namespace reserved");
        shell::CommandRouter router(manager);
        const auto completion = router.complete("he");
        check(completion.choices == std::vector<std::string>{"help"}, "builtin completion");
        check(manager.register_micro_program("custom", [](auto&) { return -300; }, [](auto&) { return std::vector<std::string>{"one", "two words"}; }) == 0, "legacy command registration");
        check(!router.execute({"custom"}).success && router.execute({"custom"}).message.find("-300") != std::string::npos, "negative status is not mistaken for unknown command");
        const auto parameters = router.complete("custom ");
        check(parameters.choices == std::vector<std::string>{"one", "two words"}, "parameter candidates survive without global registration");
        manager.begin_shutdown();
        check(bool(manager.wait_shutdown(control::Clock::now() + std::chrono::seconds(2))), "empty shutdown is complete");
        std::cout << "shell unit tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
