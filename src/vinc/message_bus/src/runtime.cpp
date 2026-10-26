#include <vinc/message_bus/detail/runtime.hpp>
#include <vinc/control/channel.hpp>

namespace vinc::message_bus::detail {
Result<std::shared_ptr<Client>> RuntimeAccess::create(std::shared_ptr<control::Session> session) {
    if (!session) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::AttachRuntime});
    }

    try {
        ControlTransport transport;
        transport.send = [session](control::Frame frame) {
            return session->send(std::move(frame));
        };
        transport.close = [session] {
            session->close();
        };
        return create(std::move(transport));
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::AttachRuntime});
    }
}
}
