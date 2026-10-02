// Bus fault-injection module used only by tests. Private ring access deliberately
// holds a reader lease so confirmed process death can exercise broker reclamation.

#include <module/base_module.hpp>
#include <vinc_messages/Frame.hpp>
#include <vinc_messages/Point.hpp>
#include <cstdlib>
#include <optional>
#include "ring.hpp"
#include "runtime.hpp"

class BusTestModule : public vinc::BaseModule {
    std::optional<vinc::bus::Publisher<vinc::Frame>> publisher_;
    std::optional<vinc::bus::Subscriber<vinc::Frame>> subscriber_;
    std::optional<vinc::bus::WriteLoan<vinc::Frame>> loan_;
    std::unique_ptr<vinc::Frame> copied_;
    std::string topic_;
    std::shared_ptr<vinc::bus::detail::Ring> held_ring_;
    std::uint32_t held_reader_ = 0;
    static int failure(const vinc::bus::Error& e) { return -100 - static_cast<int>(e.code); }
    static void fill(vinc::Frame& frame, std::uint64_t value, std::size_t bytes) {
        frame.data().sequence = value;
        vinc::message::detail::checked(frame.data().payload.copy_from(std::vector<std::uint8_t>(bytes, static_cast<std::uint8_t>(value % 251))));
        vinc::message::detail::checked(frame.data().detections.resize(1));
        frame.data().detections[0].data().id = static_cast<std::uint32_t>(value);
        frame.data().detections[0].data().label = "nested detection";
    }
public:
    explicit BusTestModule(std::string name) : BaseModule(std::move(name)) {}
    std::string on_name(std::vector<std::string>& args) override {
        if (args.size() >= 2 && args[0] == "--name") {
            auto name = args[1]; args.erase(args.begin(), args.begin() + 2); return name;
        }
        return get_module_name();
    }
    int32_t on_start(const std::vector<std::string>& args) override {
        if (args.size() < 2) return 2;
        topic_ = args[1];
        if (args[0] == "pub") {
            vinc::bus::PublisherOptions options;
            if (args.size() > 2) options.slots = static_cast<std::uint32_t>(std::stoul(args[2]));
            if (args.size() > 3) options.max_message_bytes = std::stoull(args[3]);
            auto r = bus().advertise<vinc::Frame>(topic_, options); if (!r) return failure(r.error()); publisher_.emplace(std::move(*r));
        } else {
            vinc::bus::SubscriberOptions options;
            if (args.size() > 2 && args[2] == "new") options.start = vinc::bus::StartPosition::NewOnly;
            auto r = bus().subscribe<vinc::Frame>(topic_, options); if (!r) return failure(r.error()); subscriber_.emplace(std::move(*r));
        }
        return 0;
    }
    int32_t module_interface(std::vector<std::string> args) override {
        if (args.empty()) return 0;
        const auto& cmd = args[0];
        if (cmd == "crash") std::_Exit(42);
        if (cmd == "pin") {
            auto attachments = vinc::bus::detail::RuntimeAccess::inspect_attachments(bus());
            if (attachments.size() != 1) return -10;
            auto& a = attachments[0]; auto ring = vinc::bus::detail::Ring::open(a.descriptor, false);
            if (!ring) return failure(ring.error());
            auto candidate = (*ring)->candidate(0, true);
            if (!candidate || !(*ring)->protect(*candidate, a.reader)) return -11;
            held_ring_ = std::move(*ring); held_reader_ = a.reader;
            return 0; // Test-only pause at the protected copy boundary.
        }
        if (cmd == "unpin") { if (held_ring_) held_ring_->unprotect(held_reader_); held_ring_.reset(); return 0; }
        if (cmd == "wrong-schema") { auto r = bus().advertise<vinc::Point>(topic_); return r ? 999 : failure(r.error()); }
        if (cmd == "next" || cmd == "last") {
            auto r = cmd == "next" ? subscriber_->next() : subscriber_->get_last();
            if (!r) return failure(r.error()); copied_ = std::move(*r);
            if (copied_->topic_name() != topic_ || copied_->publisher_name() == "unknown" || !copied_->get_message_timestamp() || !copied_->get_topic_count() || !copied_->publisher_instance()) return -999;
            const auto value = copied_->data().sequence.get();
            if (copied_->data().detections[0].data().id.get() != value || copied_->data().detections[0].data().label.str() != "nested detection") return -998;
            for (const auto byte : std::as_const(copied_->data().payload)) if (byte != value % 251) return -997;
            return static_cast<int32_t>(value);
        }
        if (cmd == "copy-value") return copied_ ? static_cast<int32_t>(copied_->data().sequence.get()) : -1;
        if (cmd == "copy-size") return copied_ ? static_cast<int32_t>(copied_->data().payload.size()) : -1;
        if (cmd == "count") return copied_ ? static_cast<int32_t>(copied_->get_topic_count()) : -1;
        if (cmd == "slots") return static_cast<int32_t>(publisher_->stats().effective_slots);
        if (cmd == "published") return static_cast<int32_t>(publisher_->stats().published);
        if (cmd == "collisions") return static_cast<int32_t>(publisher_->stats().collisions);
        if (cmd == "waited") return publisher_->stats().total_wait_ns > 0 && publisher_->stats().longest_wait_ns > 0 ? 1 : 0;
        if (cmd == "drop") { loan_.reset(); return 0; }
        if (cmd == "commit") {
            if (!loan_) return -1;
            auto r = publisher_->commit(*loan_); if (!r) return failure(r.error()); loan_.reset(); return 0;
        }
        if (cmd == "close") { publisher_.reset(); return 0; }
        if (cmd == "push" || cmd == "loan") {
            const auto value = std::stoull(args.at(1)); const auto bytes = args.size() > 2 ? std::stoull(args[2]) : 16;
            if (cmd == "loan") {
                auto r = publisher_->get_next(); if (!r) return failure(r.error()); loan_.emplace(std::move(*r));
                fill(**loan_, value, static_cast<std::size_t>(bytes)); return 0;
            }
            auto message = vinc::make_message<vinc::Frame>(); fill(*message, value, static_cast<std::size_t>(bytes));
            auto r = publisher_->push(*message);
            if (message->data().sequence.get() != value || message->data().payload.size() != bytes) return -996;
            return r ? 0 : failure(r.error());
        }
        return -2;
    }
};
REGISTER_MODULE(BusTestModule, "busTest")
