#include <module/base_module.hpp>
#include <vinc_messages/test.hpp>
#include <charconv>
#include <iostream>
#include <optional>

class BusExample final : public vinc::BaseModule {
    using Message = vinc::test;
    std::optional<vinc::bus::Publisher<Message>> publisher_;
    std::optional<vinc::bus::Subscriber<Message>> subscriber_;
    std::optional<vinc::bus::WriteLoan<Message>> loan_;
    static int report(const vinc::bus::Error& e) {
        std::cout << "Bus error " << static_cast<unsigned>(e.code) << ": " << e.message << '\n'; return 1;
    }
    static void fill(Message& m, std::int32_t value) {
        m.data().x = true; vinc::message::detail::checked(m.data().y.copy_from(std::vector<std::int32_t>{value}));
    }
public:
    using BaseModule::BaseModule;
    int32_t on_start(const std::vector<std::string>& input) override {
        auto args = input;
        // Shell includes the command target; direct ModuleManager callers may omit it.
        if (!args.empty() && args.front() == "busExample") args.erase(args.begin());
        if (args.empty() || (args[0] != "pub" && args[0] != "sub")) {
            std::cout << "Usage: busExample pub|sub [topic]\n"; return 1;
        }
        const auto topic = args.size() > 1 ? args[1] : "demo";
        if (args[0] == "pub") {
            auto p = bus().advertise<Message>(topic, {.slots = 2, .max_message_bytes = 1024 * 1024});
            if (!p) return report(p.error()); publisher_.emplace(std::move(*p));
        } else {
            auto s = bus().subscribe<Message>(topic); if (!s) return report(s.error()); subscriber_.emplace(std::move(*s));
        }
        std::cout << '[' << get_node_name() << "] Bus " << args[0] << " topic=" << topic << '\n'; return 0;
    }
    int32_t node_interface(std::vector<std::string> args) override {
        if (!args.empty() && args.front() == get_node_name()) args.erase(args.begin());
        if (args.empty()) return 1;
        const auto& command = args[0];
        if (subscriber_ && (command == "next" || command == "last")) {
            auto r = command == "next" ? subscriber_->next() : subscriber_->get_last();
            if (!r) return report(r.error());
            auto& m = **r;
            std::cout << '[' << get_node_name() << "] value=" << m.data().y.at(0).value()
                      << " publisher=" << m.publisher_name() << " count=" << m.get_topic_count()
                      << " timestamp_ns=" << m.get_message_timestamp() << '\n'; return 0;
        }
        if (!publisher_) return 1;
        if (command == "stats") {
            auto s = publisher_->stats();
            std::cout << "slots=" << s.effective_slots << " published=" << s.published << " collisions=" << s.collisions
                      << " total_wait_ns=" << s.total_wait_ns << " longest_wait_ns=" << s.longest_wait_ns << " current_wait_ns=" << s.current_wait_ns << '\n'; return 0;
        }
        if (command == "commit") {
            if (!loan_) { std::cout << "No outstanding loan\n"; return 1; }
            auto r = publisher_->commit(*loan_); if (!r) return report(r.error()); loan_.reset(); return 0;
        }
        if ((command == "push" || command == "loan") && args.size() == 2) {
            std::int32_t value = 0; auto [end, ec] = std::from_chars(args[1].data(), args[1].data() + args[1].size(), value);
            if (ec != std::errc{} || end != args[1].data() + args[1].size()) return 1;
            if (command == "push") {
                auto m = vinc::make_message<Message>(); fill(*m, value);
                auto r = publisher_->push(*m); return r ? 0 : report(r.error());
            }
            auto r = publisher_->get_next(); if (!r) return report(r.error());
            loan_.emplace(std::move(*r)); fill(**loan_, value); return 0;
        }
        return 1;
    }
    std::vector<std::string> node_autoComplete(std::vector<std::string>) override {
        return publisher_ ? std::vector<std::string>{"push", "loan", "commit", "stats"} : std::vector<std::string>{"next", "last"};
    }
};
REGISTER_MODULE(BusExample, "busExample")
