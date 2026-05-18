#pragma once

#include <iostream>
#include <cstdint>
#include <string>
#include <vector>

#define HOLON_CONCAT_INTERNAL(a, b) a##b
#define HOLON_CONCAT(a, b) HOLON_CONCAT_INTERNAL(a, b)

#define REGISTER_MODULE(ClassType, ModuleName) \
    static_assert(std::is_same_v<decltype(&ClassType::module_interface), int32_t (*)(std::vector<std::string>)>, \
                  "\n\n[holon Error] " #ClassType " must implement: \n" \
                  "static int32_t module_interface(std::vector<std::string> arg)\n"); \
    \
    static struct HOLON_CONCAT(ModuleRegistrar_, __LINE__) { \
        HOLON_CONCAT(ModuleRegistrar_, __LINE__)() { \
            auto& manager = holon::ModuleManager::get(); \
            \
            /* 1. 註冊主要工廠介面 */ \
            manager.register_module_factory(ModuleName, &ClassType::module_interface); \
            \
            /* 2. 匿名模板 Lambda：將成員檢查推遲到實例化階段 */ \
            []<typename T>(holon::ModuleManager& m, const std::string& name) { \
                if constexpr (requires { { T::module_autoComplete(std::declval<std::vector<std::string>>()) } -> std::same_as<std::vector<std::string>>; }) { \
                    /* 只有在 T 真的有 module_autoComplete 且型別正確時，這行才會被編譯 */ \
                    m.register_module_autoComplete(name, &T::module_autoComplete); \
                } \
            }.template operator()<ClassType>(manager, ModuleName); \
        } \
    } HOLON_CONCAT(_instance_, __LINE__);

namespace holon
{

class BaseModule
{
public:
    BaseModule(std::string node_name);

    virtual ~BaseModule();

    virtual int32_t node_interface(std::vector<std::string> arg) = 0;

    virtual std::vector<std::string> node_autoComplete(std::vector<std::string> arg)
    {
        (void)arg;
        return {};
    }

    //Although this is an annotation, 
    //you must implement this function, 
    //otherwise, the compiler will report an error.
    //virtual static int32_t module_interface(std::vector<std::string> arg) = 0;

    //To complete this function, 
    //users can press Tab to use the auto complete feature.
    //virtual static std::vector<std::string> module_autoComplete(std::vector<std::string> arg);

    std::string get_node_name() const
    {
        return _node_name;
    }

private:

    std::string _node_name;

};


} //namespace holon