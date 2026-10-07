// Local Shell tests for parsing, quoting, UTF-8 editing, name reservation and dispatch.
// No child process is needed for these command/editor contracts.

#include <vinc/shell/detail/command_router.hpp>
#include <vinc/shell/detail/shell.hpp>
#include <vinc/shell/detail/tcl_runtime.hpp>
#include <vinc/shell/detail/error_catalog.hpp>
#include <sstream>
#include <iostream>
#include <stdexcept>

using namespace vinc;

template <class T> T checked_value(vinc::Result<T> result) {
    if (!result) {
        throw std::runtime_error(vinc::error_to_string(result.error()));
    }
    return std::move(*result);
}

template <class T> void check(const vinc::Result<T>& result, const char* message) {
    if (!result) {
        throw std::runtime_error(std::string(message) + ": " + vinc::error_to_string(result.error()));
    }
}

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void test_parsing_and_quoting() {
    auto args = shell::parse_line(R"(node open "D:\My Project\file.txt" '' 'a b' a\ b "say \"hi\"")");
    check(args && args->arguments == std::vector<std::string>{"node", "open", "D:\\My Project\\file.txt", "",
                                                              "a b", "a b", "say \"hi\""},
          "quotes, empty arguments, escapes and Windows paths");
    check(!shell::parse_line("node \"unfinished"), "unfinished quote rejected");
    check(bool(shell::parse_line("node \"unfinished", true)), "unfinished completion allowed");
    check(!shell::parse_line(std::string(4097, 'x')), "bounded command");
    auto url = shell::parse_line("node https://example.test/a#b");
    check(url && url->arguments[1] == "https://example.test/a#b", "interactive argument content preserved");
    for (const std::string value : {"", "one", "a b", "a'b", "a\"b", "\\\\host\\dir\\", "中文"}) {
        auto roundtrip = shell::parse_line(shell::quote_argument(value));
        check(roundtrip && roundtrip->arguments == std::vector<std::string>{value},
              "completion quote roundtrip");
    }
}

void test_initialization_failure() {
    auto manager = checked_value(ModuleManager::create());
    std::ostringstream output;
    const auto uninitialized = Shell::create(*manager, output);
    check(!uninitialized && uninitialized.error().is(shell::ErrorCode::TclNotInitialized),
          "Shell creation reports missing Tcl initialization without throwing");
    const auto invalid = shell::initialize_tcl(nullptr);
    check(!invalid && invalid.error().is(shell::ErrorCode::InvalidArgument),
          "invalid initialization remains retryable");
}

void test_error_contracts() {
    for (const auto& entry : shell::detail::error_entries) {
        const auto failure = Error::from_raw(ErrorDomain::Shell, entry.value, 0);
        check(error_to_string(failure).find(entry.text) != std::string::npos,
              "every Shell error has a static catalog entry");
    }
    for (const auto& entry : shell::detail::operation_entries) {
        const auto failure = Error::from_raw(ErrorDomain::Shell, 1, entry.value);
        check(error_to_string(failure).find(entry.text) != std::string::npos,
              "every Shell operation has a static catalog entry");
    }

    const auto quote = shell::parse_line("node \"unfinished");
    check(!quote && quote.error().is(shell::ErrorCode::UnterminatedQuote) &&
              quote.error().operation == static_cast<std::int32_t>(shell::ErrorOperation::ParseLine),
          "parser returns typed code and operation");

    auto manager = checked_value(ModuleManager::create());
    std::ostringstream output;
    std::vector<output_hub::Subscription> subscriptions;
    for (std::size_t index = 0; index < config::output_hub_max_subscribers; ++index) {
        auto subscription = manager->output_hub().subscribe({1, 1});
        if (!subscription) {
            break;
        }

        subscriptions.push_back(std::move(*subscription));
    }
    const auto exhausted = Shell::create(*manager, output);
    check(!exhausted && exhausted.error().is(shell::ErrorCode::OutputUnavailable) &&
              !exhausted.error().causes().empty() &&
              exhausted.error().causes().front().domain == ErrorDomain::OutputHub,
          "creation wraps lower-layer subscription failure");
    subscriptions.clear();
    auto owner = checked_value(Shell::create(*manager, output));
    check(manager->register_micro_program("negative",
                                          [](auto&) {
                                              return -300;
                                          }),
          "register negative result");
    const auto result = owner->process_line("negative");
    check(result && *result == -300, "application negative status remains a successful Result value");

    shell::SocketShellOptions invalid;
    invalid.max_connections = 0;
    const auto bad_socket = owner->start_socket_shell(invalid);
    check(!bad_socket && bad_socket.error().is(shell::ErrorCode::InvalidArgument), "invalid socket options");
    shell::SocketShellOptions valid;
    valid.port = 0;
    check(owner->start_socket_shell(valid), "retry after failed socket initialization");
    const auto repeated = owner->start_socket_shell(valid);
    check(!repeated && repeated.error().is(shell::ErrorCode::AlreadyStarted), "duplicate socket startup");
    owner->request_shutdown();
    owner->wait_shutdown();
    const auto closed = owner->process_line("help");
    check(!closed && closed.error().is(shell::ErrorCode::Closing), "commands rejected after shutdown");
}

void test_line_editor() {
    shell::LineEditor editor;
    check(editor.append("hello 中文"), "UTF-8 append");
    editor.backspace();
    check(editor.line() == "hello 中", "backspace removes complete Chinese code point");
    check(editor.submit() == "hello 中", "submit preserves bytes");
    editor.append("draft");
    editor.older();
    check(editor.line() == "hello 中", "first up is newest history");
    editor.newer();
    check(editor.line() == "draft", "down restores unsubmitted draft");
    editor.submit();
    editor.older();
    check(editor.line() == "draft", "history cursor reset after submit");
    editor.replace("abc中文");
    check(editor.visible_tail(4) == "中文", "display tail keeps UTF-8 boundaries");
}

void test_parser_boundaries() {
    const auto joined = shell::parse_line(R"(node ab"cd"'ef' tail\ "a\ b" 'c\d')");
    check(joined && joined->arguments == std::vector<std::string>{"node", "abcdef", "tail a\\ b", "c\\d"},
          "adjacent quotes and quoted/unquoted backslashes retain their meaning");

    const auto escaped_tab = shell::parse_line("node a\\\tb");
    check(escaped_tab && escaped_tab->arguments == std::vector<std::string>{"node", "a\tb"},
          "escaped tab stays inside one argument");
    check(!shell::parse_line("node bad\nline", true), "completion also rejects control characters");

    const std::string full_line(config::shell_max_line_bytes, 'x');
    check(bool(shell::parse_line(full_line)), "exact byte limit accepted");
    check(!shell::parse_line(full_line + "x"), "byte limit plus one rejected");

    std::string arguments;
    for (std::size_t i = 0; i < config::shell_max_arguments; ++i) {
        arguments += "x ";
    }
    const auto at_limit = shell::parse_line(arguments);
    check(at_limit && at_limit->arguments.size() == config::shell_max_arguments && at_limit->trailing_space &&
              at_limit->last_start == arguments.size(),
          "argument limit and completion insertion position preserved");
    check(!shell::parse_line(arguments + "x"), "argument limit plus one rejected");

    shell::LineEditor editor;
    check(editor.append(full_line), "editor accepts exact byte limit");
    check(!editor.append("x") && editor.line() == full_line, "rejected append preserves editor content");
    check(!editor.replace(full_line + "x") && editor.line() == full_line,
          "rejected replacement preserves editor content");
}

void test_dispatch_and_lifetime() {
    auto manager_owner = vinc::ModuleManager::create();
    if (!manager_owner) {
        throw std::runtime_error(vinc::error_to_string(manager_owner.error()));
    }
    auto& manager = **manager_owner;
    std::ostringstream out;
    {
        auto first_owner = checked_value(vinc::Shell::create(manager, out));
        auto& first = *first_owner;
        const auto unknown = first.process_line("unknown");
        check(!unknown && unknown.error().is(shell::ErrorCode::UnknownCommand), "structured command error");
    }
    {
        auto second_owner = checked_value(vinc::Shell::create(manager, out));
        auto& second = *second_owner;
        check(checked_value(second.process_line("help")) == 0, "Shell recreation has no stale callbacks");
    }
    check(!manager.has_micro_program("help") && !manager.has_micro_program("exit"),
          "manager does not own Shell callbacks");
    check(!bool(manager.register_micro_program("exit",
                                               [](auto&) {
                                                   return 0;
                                               })),
          "builtin namespace reserved");
    shell::CommandRouter router(manager);
    check(router.initialize(), "initialize router");
    const auto completion = router.complete("he");
    check(completion && completion->choices == std::vector<std::string>{"help"}, "builtin completion");
    check(bool(manager.register_micro_program(
              "custom",
              [](auto&) {
                  return -300;
              },
              [](auto&) {
                  return std::vector<std::string>{"one", "two words"};
              })),
          "legacy command registration");
    check(checked_value(router.execute({"custom"})).code == -300 &&
              checked_value(router.execute({"custom"})).message.find("-300") != std::string::npos,
          "negative status is not mistaken for unknown command");
    const auto parameters = router.complete("custom ");
    check(parameters && parameters->choices == std::vector<std::string>{"one", "two words"},
          "parameter candidates survive without global registration");
    manager.begin_shutdown();
    check(bool(manager.wait_shutdown(control::Clock::now() + std::chrono::seconds(2))),
          "empty shutdown is complete");
}

void test_contract_rendering() {
    ModuleContractDiagnostic diagnostic;
    diagnostic.mid = 12;
    diagnostic.pid = 34;
    diagnostic.exit = {osal::ExitKind::Code, 76};
    ModuleContractReport report;
    const std::string reason = "array\x1b[2J\ninvalid";
    std::copy(reason.begin(), reason.end(), report.reason.begin());
    report.has_index = true;
    report.index = 10;
    report.size = 3;
    report.line = 99;
    report.text_truncated = true;
    diagnostic.report = report;
    auto text = shell::contract_diagnostic_text("worker", diagnostic);
    check(text.find("MID=12 PID=34 exited code=76") != std::string::npos, "fault identity and exit");
    check(text.find("index=10 size=3") != std::string::npos, "fault bounds");
    check(text.find('\x1b') == std::string::npos && text.find('\n') == std::string::npos,
          "fault text cannot inject terminal controls");
    check(text.find("requested=no") != std::string::npos && text.find("truncated") != std::string::npos,
          "truthful termination and truncation");
    diagnostic.report = std::unexpected(Error{module_system::ErrorCode::IncompleteFaultReport,
                                               module_system::ErrorOperation::ReadFaultReport});
    diagnostic.exit_deadline_expired = true;
    text = shell::contract_diagnostic_text("worker", diagnostic);
    check(text.find("Incomplete fault report") != std::string::npos, "partial report is not fabricated");
    check(text.find("fatal exit deadline expired=yes") != std::string::npos &&
              text.find("OS termination requested=no") != std::string::npos,
          "deadline expiry does not imply successful OS termination");
}

int main(int, char** argv) {
    try {
        test_initialization_failure();
        check(shell::initialize_tcl(argv[0]), "initialize Tcl");
        test_error_contracts();
        test_contract_rendering();
        test_parsing_and_quoting();
        test_line_editor();
        test_parser_boundaries();
        test_dispatch_and_lifetime();
        std::cout << "shell unit tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
