#include <holon_test/test_module.hpp>



namespace holon
{

REGISTER_MODULE(testModule, "testModule")

testModule::testModule(std::string node_name): 
BaseModule(node_name) 
{}

int32_t testModule::node_interface(std::vector<std::string> arg)
{
    (void)arg;
    std::cout << "Hello from testModule's node_interface!" << std::endl;
    // Implement your node interface logic here
    return 0;
}

int32_t testModule::module_interface(std::vector<std::string> arg)
{
    (void)arg;
    std::cout << "Hello from testModule's static module_interface!" << std::endl;
    new testModule("testNode");
    return 0;
}

}