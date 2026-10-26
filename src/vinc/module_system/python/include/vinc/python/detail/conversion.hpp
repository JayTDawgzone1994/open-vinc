#pragma once
#include <vinc/python/detail/bridge.hpp>
#include <cmath>
#include <limits>

namespace vinc::python {
[[noreturn]] inline void overflow() {
    PyErr_SetString(PyExc_OverflowError, "value is outside the declared VINC type range");
    throw py::error_already_set();
}

template<class T> T scalar_value(py::handle value) {
    if constexpr (std::is_same_v<T, bool>) {
        if (!PyBool_Check(value.ptr())) {
            throw py::type_error("expected bool");
        }

        return value.ptr() == Py_True;
    } else if constexpr (std::is_integral_v<T>) {
        if (!PyLong_Check(value.ptr()) || PyBool_Check(value.ptr())) {
            throw py::type_error("expected int, not bool or a floating-point value");
        }

        if constexpr (std::is_signed_v<T>) {
            const auto integer = PyLong_AsLongLong(value.ptr());
            if (PyErr_Occurred()) {
                throw py::error_already_set();
            }

            if (integer < (std::numeric_limits<T>::min)() || integer > (std::numeric_limits<T>::max)()) {
                overflow();
            }

            return static_cast<T>(integer);
        } else {
            const auto integer = PyLong_AsUnsignedLongLong(value.ptr());
            if (PyErr_Occurred()) {
                throw py::error_already_set();
            }

            if (integer > (std::numeric_limits<T>::max)()) {
                overflow();
            }

            return static_cast<T>(integer);
        }
    } else {
        if ((!PyFloat_Check(value.ptr()) && !PyLong_Check(value.ptr())) || PyBool_Check(value.ptr())) {
            throw py::type_error("expected a number");
        }

        const auto number = PyFloat_AsDouble(value.ptr());
        if (PyErr_Occurred()) {
            throw py::error_already_set();
        }

        if (std::isfinite(number) && std::abs(number) > (std::numeric_limits<T>::max)()) {
            overflow();
        }

        return static_cast<T>(number);
    }
}

inline std::uint64_t index_value(py::handle value, std::uint64_t size) {
    auto indexed = py::reinterpret_steal<py::object>(PyNumber_Index(value.ptr()));
    if (!indexed) {
        throw py::error_already_set();
    }

    int overflowed = 0;
    auto index = PyLong_AsLongLongAndOverflow(indexed.ptr(), &overflowed);
    if (PyErr_Occurred()) {
        throw py::error_already_set();
    }

    if (overflowed != 0) {
        throw py::index_error("index out of range");
    }

    if (index < 0) {
        if (static_cast<std::uint64_t>(-(index + 1)) >= size) {
            throw py::index_error("index out of range");
        }

        return size - static_cast<std::uint64_t>(-(index + 1)) - 1;
    }
    if (static_cast<std::uint64_t>(index) >= size) {
        throw py::index_error("index out of range");
    }

    return static_cast<std::uint64_t>(index);
}
}
