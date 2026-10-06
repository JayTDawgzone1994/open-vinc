#pragma once

// Convenience entry point for supported framework APIs. Individual component headers
// remain available. Generated application messages must be included separately.
// Internal detail contracts and Ribosome are not aggregate API entries.

#include <vinc/error.hpp>
#include <vinc/common_error.hpp>
#include <vinc/contract.hpp>
#include <vinc/osal/error.hpp>
#include <vinc/result.hpp>
#include <vinc/time.hpp>
#include <vinc/config.hpp>
#include <vinc/module/base_module.hpp>
#include <vinc/module/context.hpp>
#include <vinc/module/mid.hpp>
#include <vinc/module/contract_report.hpp>
#include <vinc/module/host/module_manager.hpp>
#include <vinc/message/base_message.hpp>
#include <vinc/message/storage.hpp>
#include <vinc/message/generated.hpp>
#include <vinc/message_bus/message_bus.hpp>
#include <vinc/control/channel.hpp>
#include <vinc/control/protocol.hpp>
#include <vinc/output_hub/output_hub.hpp>
#include <vinc/output_hub/text_renderer.hpp>
#include <vinc/shell/error.hpp>
#include <vinc/shell/socket_shell_options.hpp>
#include <vinc/concurrency/interprocess_mutex.hpp>
#include <vinc/osal/path.hpp>
#include <vinc/osal/dynamic_library.hpp>
#include <vinc/osal/file_lock.hpp>
#include <vinc/osal/pipe.hpp>
#include <vinc/osal/process.hpp>
#include <vinc/osal/process_pipe.hpp>
#include <vinc/osal/random.hpp>
#include <vinc/osal/shared_atomic.hpp>
#include <vinc/osal/shared_memory.hpp>
#include <vinc/osal/socket.hpp>
#include <vinc/osal/socket_address.hpp>
#include <vinc/osal/socket_poller.hpp>
#include <vinc/osal/terminal.hpp>
