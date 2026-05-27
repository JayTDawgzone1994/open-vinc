#include <vinc_test/test_module.hpp>
#include <axon_bus/axon_bus.hpp>
#include <memory>
#include <vector>

namespace vinc
{

// 一個用來測試 AxonBus 的假訊息
class DummyMsg : public vinc::BaseMessage {
public:
    DummyMsg() {
        _topic_name = "test_topic";
    }

    std::string to_string() const override { return "DummyMsg"; }
    uint64_t type_hash() const override { return 1; }
    std::string msg_name_str() const override { return "DummyMsg"; }
    void serialize(std::vector<uint8_t>& buffer) const override {}
    int8_t deserialize(const std::vector<uint8_t>& buffer, size_t& offset) override { return 0; }
};

REGISTER_MODULE(testModule, "testModule")

// Subscribe callback handler
static void dummy_handler(const BaseMessage* msg, void* userData)
{
    std::cout << "[testModule] Received message on topic: " << msg->topic_name() << std::endl;
}

testModule::testModule(std::string node_name): 
BaseModule(node_name) 
{
    // Ensure engine is started so worker threads run
    AxonBus::get().start_engine();

    // Subscribe to test_topic
    _sub_id = AxonBus::get().low_level_subscribe("test_topic", dummy_handler, this);
    std::cout << "[testModule] Subscribed to 'test_topic' with id: " << _sub_id << std::endl;
}

testModule::~testModule()
{
    if (_sub_id != 0) {
        AxonBus::get().unsubscribe("test_topic", _sub_id);
    }
}

int32_t testModule::node_interface(std::vector<std::string> arg)
{
    std::cout << "[testModule] Sending message to 'test_topic'..." << std::endl;

    // Use std::make_shared because AxonBus::make_message template implementation is in .cpp
    auto msg = std::make_shared<DummyMsg>();
    AxonBus::get().low_level_publish("test_topic", msg);

    return 0;
}

int32_t testModule::module_interface(std::vector<std::string> arg)
{
    std::cout << "Hello from testModule's static module_interface!" << std::endl;
    new testModule("testNode");
    return 0;
}

}