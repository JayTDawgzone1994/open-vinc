#include <module/module_manager.hpp>

namespace holon
{

ModuleManager& ModuleManager::get()
{
    static ModuleManager instance;
    return instance;
}

int8_t ModuleManager::register_module_factory(const std::string& module_name, int32_t (*factory_func)(std::vector<std::string>))
{
    if (_module_factories.find(module_name) != _module_factories.end()) 
    {
        return -1; // 已經註冊過了
    }
    _module_factories[module_name] = factory_func;
    return 0;
}

int8_t ModuleManager::unregister_module_factory(const std::string& module_name)
{
    auto it = _module_factories.find(module_name);
    if (it == _module_factories.end()) 
    {
        return -1; // 沒有這個模組
    }
    _module_factories.erase(it);
    return 0;
}

int8_t ModuleManager::register_module_autoComplete(const std::string& module_name, 
    std::vector<std::string> (*autoComplete_func)(std::vector<std::string>))
{
    if (_module_autoCompletes.find(module_name) != _module_autoCompletes.end()) 
    {
        return -1; // 已經註冊過了
    }
    _module_autoCompletes[module_name] = autoComplete_func;
    return 0;
}

int8_t ModuleManager::register_runtime_node(BaseModule* node)
{
    if (_runtime_nodes.find(node->get_node_name()) != _runtime_nodes.end()) 
    {
        return -1; // 已經註冊過了
    }
    _runtime_nodes[node->get_node_name()] = node;
    return 0;
}

int8_t ModuleManager::register_micro_program(
    const std::string& name, 
    CommandFunction main_func, 
    AutoCompleteFunction autoComplete_func)
{
    if (_microPrograms.find(name) != _microPrograms.end()) 
    {
        return -1;
    }
    microProgram program;
    program.name = name;
    program.main_func = main_func;
    program.autoComplete_func = autoComplete_func;
    _microPrograms[name] = program;
    return 0;
}

int8_t ModuleManager::unregister_runtime_node(const std::string& node_name)
{
    auto it = _runtime_nodes.find(node_name);
    if (it == _runtime_nodes.end()) 
    {
        return -1; // 沒有這個節點
    }
    _runtime_nodes.erase(it);
    return 0;
}

int32_t ModuleManager::call_module_interface(const std::string& module_name, std::vector<std::string> arg)
{
    auto it = _module_factories.find(module_name);
    if (it == _module_factories.end()) 
    {
        return -1; // 沒有這個模組
    }
    return it->second(arg);
}

int32_t ModuleManager::call_node_interface(const std::string& node_name, std::vector<std::string> arg)
{
    auto it = _runtime_nodes.find(node_name);
    if (it == _runtime_nodes.end()) 
    {
        return -1; // 沒有這個節點
    }
    return it->second->node_interface(arg);
}

std::vector<std::string> 
ModuleManager::call_module_autoComplete(
    const std::string& module_name, 
    std::vector<std::string> arg)
{
    auto it = _module_autoCompletes.find(module_name);
    if (it == _module_autoCompletes.end()) 
    {
        return {}; // 沒有這個模組的 autoComplete
    }
    return it->second(arg);
}

std::vector<std::string> 
ModuleManager::call_node_autoComplete(
    const std::string& node_name, 
    std::vector<std::string> arg)
{
    auto it = _runtime_nodes.find(node_name);
    if (it == _runtime_nodes.end()) 
    {
        return {}; // 沒有這個節點的 autoComplete
    }
    return it->second->node_autoComplete(arg);
}

int32_t ModuleManager::call_micro_program(const std::string& program_name, std::vector<std::string> args)
{
    auto it = _microPrograms.find(program_name);
    if (it == _microPrograms.end()) 
    {
        return ERR_MICRO_PROGRAM_NOT_FOUND; // 沒有這個微程式
    }
    if (it->second.main_func == nullptr) 
    {
        return ERR_MICRO_PROGRAM_NOT_FOUND; // 這個微程式沒有主功能
    }
    return it->second.main_func(args);
}

std::vector<std::string> ModuleManager::call_micro_program_autoComplete(const std::string& program_name, std::vector<std::string> args)
{
    auto it = _microPrograms.find(program_name);
    if (it == _microPrograms.end()) 
    {
        return {}; // 沒有這個微程式
    }
    if (it->second.autoComplete_func == nullptr) 
    {
        return {}; // 這個微程式沒有自動完成功能
    }
    return it->second.autoComplete_func(args);
}

// 取得所有已註冊的模組名稱（工廠）
std::vector<std::string> ModuleManager::get_registered_factory_names() const
{
    std::vector<std::string> names;
    for (const auto& [name, _] : _module_factories) 
        names.push_back(name);
    return names;
}

// 取得所有正在執行的節點名稱 (Active Nodes)
std::vector<std::string> ModuleManager::get_active_node_names() const
{
    std::vector<std::string> names;
    for (const auto& [name, _] : _runtime_nodes) 
        names.push_back(name);
    return names;
}

std::vector<std::string> ModuleManager::get_micro_program_names() const
{
    std::vector<std::string> names;
    for (const auto& [name, _] : _microPrograms) 
        names.push_back(name);
    return names;
}

bool ModuleManager::has_factory(const std::string& module_name) const
{
    return _module_factories.find(module_name) != _module_factories.end();
}

bool ModuleManager::has_active_node(const std::string& node_name) const
{
    return _runtime_nodes.find(node_name) != _runtime_nodes.end();
}

bool ModuleManager::has_micro_program(const std::string& program_name) const
{
    return _microPrograms.find(program_name) != _microPrograms.end();
}

}