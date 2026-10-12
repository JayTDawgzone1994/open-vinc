#include <vinc/python/detail/message_bus.hpp>
#include <vinc_messages/PythonTestFrame.hpp>
#include <vinc_messages/PythonTestTypes.hpp>

namespace vinc::python {
void bind_fixture_messages(py::module_& module, py::module_& views) {
    // Test schemas are deliberately separate from the deployed application catalog.
    auto fixtures = module.def_submodule("_test_messages");
    bind_message<PythonTestFrame>(fixtures, views);
    bind_message<PythonTestTypes>(fixtures, views);
    fixtures.attr("Frame") = fixtures.attr("PythonTestFrame");
    fixtures.attr("Types") = fixtures.attr("PythonTestTypes");
}
}
