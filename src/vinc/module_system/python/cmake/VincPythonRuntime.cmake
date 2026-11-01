include_guard(GLOBAL)

# This selects declared targets, not compiler flags on an arbitrary dependency
# graph. Source bindings, installed bindings and link tests share the policy.
function(vinc_python_runtime_targets output_runtime output_errors)
    if(MSVC)
        if(TARGET vinc_module_runtime_python_runtime)
            set(runtime vinc_module_runtime_python_runtime)
            set(errors vinc_errors_python_runtime)
        else()
            set(runtime vinc::module_runtime_python_runtime)
            set(errors vinc::errors_python_runtime)
        endif()
    else()
        # Linux/macOS keep their existing system-runtime contract until native
        # validation justifies another policy. No implicit -static fallback.
        set(runtime vinc::module_runtime)
        set(errors vinc::errors)
    endif()
    foreach(target IN ITEMS "${runtime}" "${errors}")
        if(NOT TARGET "${target}")
            message(FATAL_ERROR "Python runtime target is unavailable: ${target}")
        endif()
    endforeach()
    set(${output_runtime} "${runtime}" PARENT_SCOPE)
    set(${output_errors} "${errors}" PARENT_SCOPE)
endfunction()
