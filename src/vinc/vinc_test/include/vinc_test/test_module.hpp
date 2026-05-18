#pragma once

#include <iostream>

#include <module/base_module.hpp>
#include <module/module_manager.hpp>

namespace vinc
{

class testModule : public BaseModule
{
public:
    testModule(std::string node_name);

    int32_t node_interface(std::vector<std::string> arg) override;

    static int32_t module_interface(std::vector<std::string> arg);

private:

};

} //namespace vinc



