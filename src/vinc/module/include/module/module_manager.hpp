#pragma once

#include <iostream>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <functional>

#include <vinc_define.hpp>
#include <module/base_module.hpp>

namespace vinc
{

using CommandFunction = std::function<int32_t(std::vector<std::string>& args )>;
using AutoCompleteFunction = std::function<std::vector<std::string>(std::vector<std::string>& args )>;

struct microProgram
{
    std::string name = "unknown";
    CommandFunction main_func = nullptr;
    AutoCompleteFunction autoComplete_func = nullptr;
};

class ModuleManager
{
public:

    ~ModuleManager() = default;

    static ModuleManager& get();

    int8_t register_module_factory(const std::string& module_name, int32_t (*factory_func)(std::vector<std::string>));

    int8_t unregister_module_factory(const std::string& module_name);

    int8_t register_runtime_node(BaseModule* node);

    int8_t register_micro_program(const std::string& name, CommandFunction main_func, AutoCompleteFunction autoComplete_func = nullptr);

    int8_t unregister_runtime_node(const std::string& node_name);

    int8_t register_module_autoComplete(const std::string& module_name, std::vector<std::string> (*autoComplete_func)(std::vector<std::string>));

    int32_t call_module_interface(const std::string& module_name, std::vector<std::string> args);

    int32_t call_node_interface(const std::string& node_name, std::vector<std::string> args);

    std::vector<std::string> call_module_autoComplete(const std::string& module_name, std::vector<std::string> args);

    std::vector<std::string> call_node_autoComplete(const std::string& node_name, std::vector<std::string> args);

    int32_t call_micro_program(const std::string& program_name, std::vector<std::string> args);

    std::vector<std::string> call_micro_program_autoComplete(const std::string& program_name, std::vector<std::string> args);

    // 取得所有已註冊的模組名稱（工廠）
    std::vector<std::string> get_registered_factory_names() const;

    // 取得所有正在執行的節點名稱 (Active Nodes)
    std::vector<std::string> get_active_node_names() const;

    std::vector<std::string> get_micro_program_names() const;

    bool has_factory(const std::string& module_name) const;
    bool has_active_node(const std::string& node_name) const;
    bool has_micro_program(const std::string& program_name) const;

private:

    ModuleManager() = default;

    std::map<std::string, int32_t (*)(std::vector<std::string>)> _module_factories;
    std::map<std::string, std::vector<std::string> (*)(std::vector<std::string>)> _module_autoCompletes;
    std::map<std::string, BaseModule*> _runtime_nodes;

    std::map<std::string, microProgram> _microPrograms;

};



}