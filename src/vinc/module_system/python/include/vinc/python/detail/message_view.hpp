#pragma once
#include <vinc/python/detail/bridge.hpp>
#include <vinc/python/detail/conversion.hpp>
#include <vinc/message/generated.hpp>

namespace vinc::python {
namespace storage = message::detail;

[[noreturn]] inline void expired_message() {
    PyErr_SetString(PyExc_ReferenceError, "message reference has expired");
    throw py::error_already_set();
}

// All fields are guarded by the GIL. The backing object owns either a local
// message or a loan; child views retain it without copying the payload.
struct PayloadOwner {
    std::shared_ptr<void> backing;
    bool valid = true;
    bool busy = false;
};

struct MessageNode {
    storage::Binding binding;
    std::shared_ptr<PayloadOwner> owner;

    storage::Info check() const {
        if (!owner || !owner->valid) {
            expired_message();
        }

        if (owner->busy) {
            raise_error(Error{message_bus::ErrorCode::OperationInProgress, message_bus::ErrorOperation::Submit});
        }

        auto result = storage::info(binding.state, binding.handle);
        if (!result && (result.error().is(message::ErrorCode::StaleHandle) ||
                        result.error().is(message::ErrorCode::Closed))) {
            expired_message();
        }

        return unwrap(std::move(result));
    }

    MessageNode child(std::uint64_t index) const {
        check();
        auto handle = unwrap(storage::child(binding.state, binding.handle, index));
        return {{binding.state, handle, true}, owner};
    }
};

// Released-GIL native operations borrow one payload exclusively. Concurrent
// Python access reports Busy rather than racing an image being copied/sealed.
struct PayloadUse {
    std::shared_ptr<PayloadOwner> owner;
    explicit PayloadUse(const MessageNode& node) : owner(node.owner) {
        node.check();
        owner->busy = true;
    }
    ~PayloadUse() {
        owner->busy = false;
    }
};

template<class T> struct View : MessageNode {};
template<class T> struct FieldValue {
    using Type = T;
};
template<class T> struct FieldValue<message::Scalar<T>> {
    using Type = T;
};

inline MessageNode detached(const MessageNode& node) {
    node.check();
    return {unwrap(storage::clone(node.binding)), std::make_shared<PayloadOwner>()};
}

template<class T> View<T> fresh_view() {
    storage::Binding binding;
    if constexpr (storage::Record<T>) {
        binding = unwrap(storage::create_record(T::schema()));
    } else {
        binding.state = storage::local_state();
        binding.handle = unwrap(storage::add_root(binding.state,
            storage::type<typename storage::IsArray<T>::Element>()));
    }
    return {{std::move(binding), std::make_shared<PayloadOwner>()}};
}

template<class T> py::object read_value(const MessageNode& node, std::uint64_t index = 0) {
    node.check();
    if constexpr (storage::scalar<T>) {
        return py::cast(storage::decode<T>(unwrap(storage::read(node.binding.state, node.binding.handle, index))));
    } else {
        return py::cast(View<T>{node});
    }
}

template<class T> void assign_value(const MessageNode& node, py::handle value);

template<class T> void append_value(const MessageNode& node, py::handle value) {
    node.check();
    if constexpr (storage::scalar<T>) {
        auto bytes = storage::encode(scalar_value<T>(value));
        unwrap(storage::append(node.binding.state, node.binding.handle, bytes));
    } else {
        auto temporary = fresh_view<T>();
        assign_value<T>(temporary, value);
        unwrap(storage::append_child(node.binding.state, node.binding.handle,
            temporary.binding.state, temporary.binding.handle));
    }
}

template<class T> void assign_value(const MessageNode& node, py::handle value) {
    node.check();
    if constexpr (storage::scalar<T>) {
        unwrap(storage::write(node.binding.state, node.binding.handle, 0, storage::encode(scalar_value<T>(value))));
    } else {
        if (py::isinstance<View<T>>(value)) {
            auto source = py::cast<View<T>>(value);
            source.check();
            unwrap(storage::replace(node.binding, source.binding));
            return;
        }

        if constexpr (std::is_same_v<T, message::MessageString>) {
            if (!PyBytes_Check(value.ptr()) && !PyUnicode_Check(value.ptr())) {
                throw py::type_error("message strings require str, bytes or a MessageString view");
            }

            // str is explicitly UTF-8 encoded; bytes remain unmodified, including NUL.
            auto text = py::cast<std::string>(value);
            unwrap(storage::preflight_assign(node.binding.state, node.binding.handle, text.size()));
            unwrap(storage::assign(node.binding.state, node.binding.handle,
                std::as_bytes(std::span(text.data(), text.size())), text.size()));
        } else if constexpr (storage::is_array<T>) {
            // Validate/build off to the side: conversion or capacity failure must
            // not partially replace an existing field or a shared loan.
            using Element = typename storage::IsArray<T>::Element;
            if constexpr (storage::scalar<Element>) {
                if constexpr (std::is_same_v<Element, std::uint8_t>) {
                    if (PyBytes_Check(value.ptr())) {
                        const auto count = static_cast<std::uint64_t>(PyBytes_Size(value.ptr()));
                        node.check();
                        unwrap(storage::preflight_assign(node.binding.state, node.binding.handle, count));
                        auto bytes = std::span(PyBytes_AsString(value.ptr()), static_cast<std::size_t>(count));
                        unwrap(storage::assign(node.binding.state, node.binding.handle, std::as_bytes(bytes), count));
                        return;
                    }
                }
                std::vector<std::byte> bytes;
                std::uint64_t count = 0;
                for (auto element : py::reinterpret_borrow<py::iterable>(value)) {
                    auto encoded = storage::encode(scalar_value<Element>(element));
                    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
                    ++count;
                }
                node.check();
                unwrap(storage::preflight_assign(node.binding.state, node.binding.handle, count));
                unwrap(storage::assign(node.binding.state, node.binding.handle, bytes, count));
            } else {
                auto temporary = fresh_view<T>();
                for (auto element : py::reinterpret_borrow<py::iterable>(value)) {
                    append_value<Element>(temporary, element);
                }
                node.check();
                unwrap(storage::replace(node.binding, temporary.binding));
            }
        } else {
            throw py::type_error("record assignment requires the matching message data view");
        }
    }
}

template<class T> std::string view_name() {
    if constexpr (storage::Record<T>) {
        return "_Data_" + std::string(T::message_name);
    } else if constexpr (std::is_same_v<T, message::MessageString>) {
        return "MessageString";
    } else if constexpr (storage::is_array<T>) {
        return "_Array_" + view_name<typename storage::IsArray<T>::Element>();
    } else {
        return std::to_string(storage::scalar_code<T>());
    }
}

template<class T> void bind_view(py::module_& module);

template<class T> struct FieldVisitor {
    py::module_& module;
    py::class_<View<T>>& type;

    template<class Field> void field(const char* name, std::uint64_t index) {
        using Value = typename FieldValue<Field>::Type;
        if constexpr (!storage::scalar<Value>) {
            bind_view<Value>(module);
        }
        type.def_property(name,
            [index](const View<T>& view) {
                return read_value<Value>(view.child(index));
            },
            [index](const View<T>& view, py::handle value) {
                assign_value<Value>(view.child(index), value);
            });
    }
};

template<class T> void bind_view(py::module_& module) {
    auto name = view_name<T>();
    if (py::hasattr(module, name.c_str())) {
        return;
    }

    auto type = py::class_<View<T>>(module, name.c_str());
    type.def("__copy__", [](const View<T>& view) {
        return View<T>{detached(view)};
    });
    type.def("assign", [](const View<T>& view, py::handle value) {
        assign_value<T>(view, value);
    });
    if constexpr (storage::Record<T>) {
        T::ribosome_visit_fields(FieldVisitor<T>{module, type});
    } else {
        type.def("copy", [](const View<T>& view) {
            return View<T>{detached(view)};
        });
        using Element = typename storage::IsArray<T>::Element;
        if constexpr (!storage::scalar<Element>) {
            bind_view<Element>(module);
        }
        auto item = [](const View<T>& view, py::handle key) {
            const auto index = index_value(key, view.check().size);
            if constexpr (storage::scalar<Element>) {
                return read_value<Element>(view, index);
            } else {
                return read_value<Element>(view.child(index));
            }
        };
        auto set = [](const View<T>& view, py::handle key, py::handle value) {
            const auto index = index_value(key, view.check().size);
            view.check();
            if constexpr (storage::scalar<Element>) {
                unwrap(storage::write(view.binding.state, view.binding.handle, index,
                    storage::encode(scalar_value<Element>(value))));
            } else {
                assign_value<Element>(view.child(index), value);
            }
        };
        type.def("__len__", [](const View<T>& view) {
            return view.check().size;
        });
        type.def_property_readonly("capacity", [](const View<T>& view) {
            return view.check().capacity;
        });
        type.def("__getitem__", item);
        type.def("__setitem__", set);
        type.def("set", set);
        type.def("at", [item](const View<T>& view, py::handle key) {
            auto value = item(view, key);
            if constexpr (storage::scalar<Element>) {
                return value;
            } else {
                return py::cast(View<Element>{detached(py::cast<View<Element>>(value))});
            }
        });
        type.def("resize", [](const View<T>& view, py::handle size) {
            view.check();
            unwrap(storage::resize(view.binding.state, view.binding.handle, scalar_value<std::uint64_t>(size), false));
        });
        type.def("reserve", [](const View<T>& view, py::handle size) {
            view.check();
            unwrap(storage::resize(view.binding.state, view.binding.handle, scalar_value<std::uint64_t>(size), true));
        });
        type.def("clear", [](const View<T>& view) {
            view.check();
            unwrap(storage::resize(view.binding.state, view.binding.handle, 0, false));
        });
        type.def("shrink_to_fit", [](const View<T>& view) {
            view.check();
            unwrap(storage::shrink(view.binding.state, view.binding.handle));
        });
        type.def("push_back", [](const View<T>& view, py::handle value) {
            append_value<Element>(view, value);
        });
        type.def("pop_back", [](const View<T>& view) {
            auto count = view.check().size;
            if (count == 0) {
                throw py::index_error("pop from empty MessageArray");
            }

            unwrap(storage::resize(view.binding.state, view.binding.handle, count - 1, false));
        });
        if constexpr (std::is_same_v<T, message::MessageString>) {
            type.def("to_bytes", [](const View<T>& view) {
                const auto count = view.check().size;
                std::string bytes(static_cast<std::size_t>(count), '\0');
                for (std::uint64_t index = 0; index < count; ++index) {
                    auto value = unwrap(storage::read(view.binding.state, view.binding.handle, index));
                    bytes[static_cast<std::size_t>(index)] = static_cast<char>(storage::decode<std::uint8_t>(value));
                }
                return py::bytes(bytes);
            });
            type.def("decode", [](py::object self) {
                return self.attr("to_bytes")().attr("decode")("utf-8", "strict");
            });
        }
    }
}
}
