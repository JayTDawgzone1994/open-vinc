#pragma once

#include <iostream>
#include <cstdint>
#include <string>

namespace vinc
{

enum error_code : int32_t 
{
    SUCCESS                         = 0,
    
    // System & Shell Errors (-1 ~ -99)
    ERR_UNKNOWN                      = -1,
    ERR_INVALID_ARGUMENTS            = -2,
    ERR_COMMAND_NOT_FOUND            = -3,
    ERR_BUFFER_OVERFLOW              = -4,
    ERR_PERMISSION_DENIED            = -5,

    // Module Manager Errors (-100 ~ -199)
    ERR_FACTORY_ALREADY_EXISTS       = -100,
    ERR_FACTORY_NOT_FOUND            = -101,
    ERR_NODE_ALREADY_EXISTS          = -102,
    ERR_NODE_NOT_FOUND               = -103,
    ERR_RESERVED_NAME_CONFLICT       = -104,
    ERR_MODULE_INTERFACE_FAILED      = -105,
    ERR_MICRO_PROGRAM_ALREADY_EXISTS = -106,
    ERR_MICRO_PROGRAM_NOT_FOUND      = -107,

    // BaseModule & Runtime Errors (-200 ~ -299)
    ERR_TOO_MANY_INSTANCES           = -200,
    ERR_NODE_INIT_FAILED             = -201,
    ERR_NODE_INTERFACE_FAILED        = -202,
    ERR_AUTOCOMPLETE_FAILED          = -203,
    
    // Resource Errors (-300 ~ -399)
    ERR_OUT_OF_MEMORY                = -300,
    ERR_DEVICE_NOT_READY             = -301,
    ERR_IO_ERROR                     = -302
};

inline std::string error_to_string(int32_t code) 
{
    switch (code) {
        case SUCCESS:                               return "SUCCESS";
        
        case ERR_UNKNOWN:                           return "ERR_UNKNOWN";
        case ERR_INVALID_ARGUMENTS:                 return "ERR_INVALID_ARGUMENTS";
        case ERR_COMMAND_NOT_FOUND:                 return "ERR_COMMAND_NOT_FOUND";
        case ERR_BUFFER_OVERFLOW:                   return "ERR_BUFFER_OVERFLOW";
        case ERR_PERMISSION_DENIED:                 return "ERR_PERMISSION_DENIED";

        case ERR_FACTORY_ALREADY_EXISTS:            return "ERR_FACTORY_ALREADY_EXISTS";
        case ERR_FACTORY_NOT_FOUND:                 return "ERR_FACTORY_NOT_FOUND";
        case ERR_NODE_ALREADY_EXISTS:               return "ERR_NODE_ALREADY_EXISTS";
        case ERR_NODE_NOT_FOUND:                    return "ERR_NODE_NOT_FOUND";
        case ERR_RESERVED_NAME_CONFLICT:            return "ERR_RESERVED_NAME_CONFLICT";
        case ERR_MODULE_INTERFACE_FAILED:           return "ERR_MODULE_INTERFACE_FAILED";
        case ERR_MICRO_PROGRAM_ALREADY_EXISTS:      return "ERR_MICRO_PROGRAM_ALREADY_EXISTS";
        case ERR_MICRO_PROGRAM_NOT_FOUND:           return "ERR_MICRO_PROGRAM_NOT_FOUND";
        case ERR_TOO_MANY_INSTANCES:                return "ERR_TOO_MANY_INSTANCES";
        case ERR_NODE_INIT_FAILED:                  return "ERR_NODE_INIT_FAILED";
        case ERR_NODE_INTERFACE_FAILED:             return "ERR_NODE_INTERFACE_FAILED";
        case ERR_AUTOCOMPLETE_FAILED:               return "ERR_AUTOCOMPLETE_FAILED";

        case ERR_OUT_OF_MEMORY:                      return "ERR_OUT_OF_MEMORY";
        case ERR_DEVICE_NOT_READY:                   return "ERR_DEVICE_NOT_READY";
        case ERR_IO_ERROR:                           return "ERR_IO_ERROR";

        default: return "UNKNOWN_ERROR_CODE (" + std::to_string(code) + ")";
    }
}

} // namespace vinc
