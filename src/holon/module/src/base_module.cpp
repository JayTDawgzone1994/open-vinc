#include <module/base_module.hpp>
#include <module/module_manager.hpp>

namespace holon
{

BaseModule::BaseModule(std::string node_name)
{
std::string base_name = node_name;
    int32_t suffix = 0;
    auto& manager = ModuleManager::get();

    while (true) 
    {
        if (suffix == 0) {
            _node_name = base_name;
        } else {
            _node_name = base_name + "_" + std::to_string(suffix);
        }

        if (manager.has_factory(_node_name) || manager.has_active_node(_node_name)) 
        {
            suffix++;
        } 
        else 
        {
            if (manager.register_runtime_node(this) == 0) {
                std::cout << "[BaseModule] Node registered successfully as: " << _node_name << std::endl;
                break;
            } else {
                suffix++;
            }
        }

        /*if (suffix > 999) 
        {
            std::cerr << "[BaseModule] Fatal: Too many instances or naming conflicts for: " << base_name << std::endl;
            _node_name = base_name + "_failed_" + std::to_string(std::rand() % 1000); 
            break; 
        }*/
    }
}

BaseModule::~BaseModule()
{
    if(ModuleManager::get().unregister_runtime_node(_node_name) != 0)
    {
        std::cerr << "[BaseModule] Failed to unregister node, how did you manage to get here?: " << _node_name << std::endl;
    }
}



}