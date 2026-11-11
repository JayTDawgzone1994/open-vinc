# Exercise a real framework entry, including load-before-open and restart policy.
# Callers own isolated App/data/work paths and serialize fixed-port UI execution.
function(verify_log_settings app data work timeout)
    file(MAKE_DIRECTORY "${work}")
    set(disable_script "${work}/disable.tcl")
    set(disabled_script "${work}/disabled.tcl")
    set(enabled_script "${work}/enabled.tcl")
    file(WRITE "${disable_script}" "param get vinc.log_file_bytes\nparam get vinc.log_total_bytes\nparam set vinc.log_file_enabled false\nparam save\nlog status\nexit\n")
    file(WRITE "${disabled_script}" "log status\nif {![catch {log reopen} reason]} {error {Reopen bypassed disabled files}}\nputs LOG_DISABLED_REOPEN_REJECTED\nparam set vinc.log_file_enabled true\nparam save\nlog status\nexit\n")
    file(WRITE "${enabled_script}" "log status\nexit\n")
    foreach(phase disable disabled enabled)
        execute_process(COMMAND "${app}" --shell-port 0 -S "${${phase}_script}"
            WORKING_DIRECTORY "${work}" TIMEOUT ${timeout}
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
        file(WRITE "${work}/${phase}.log" "${output}\n${errors}")
        if(NOT result STREQUAL "0" OR NOT output MATCHES "All module processes exited"
                OR output MATCHES "Script failed")
            message(FATAL_ERROR "Log Param App ${phase} failed: ${result}\n${output}\n${errors}")
        endif()
        if(phase STREQUAL "disabled")
            if(NOT output MATCHES "file output disabled by startup settings"
                    OR NOT output MATCHES "file=disabled"
                    OR NOT output MATCHES "queued=0 dropped=0"
                    OR NOT output MATCHES "file_lost=0"
                    OR NOT output MATCHES "LOG_DISABLED_REOPEN_REJECTED"
                    OR output MATCHES "file=enabled|Details: framework log files")
                message(FATAL_ERROR "Disabled App reported incorrect file state: ${output}\n${errors}")
            endif()
            file(GLOB_RECURSE after_logs "${data}/logs/*")
            if(NOT "${after_logs}" STREQUAL "${before_logs}")
                message(FATAL_ERROR "Disabled App changed the Log file inventory")
            endif()
            foreach(path IN LISTS before_logs)
                string(SHA256 key "${path}")
                file(SHA256 "${path}" digest)
                file(TIMESTAMP "${path}" modified "%s" UTC)
                if(NOT digest STREQUAL "${hash_${key}}" OR NOT modified STREQUAL "${time_${key}}")
                    message(FATAL_ERROR "Disabled App modified existing Log: ${path}")
                endif()
            endforeach()
        else()
            if(NOT output MATCHES "file=enabled")
                message(FATAL_ERROR "App did not retain/apply enabled policy: ${output}\n${errors}")
            endif()
        endif()
        if(phase STREQUAL "disable")
            if(NOT output MATCHES "vinc.log_file_bytes \\[uint64\\] = 16777216"
                    OR NOT output MATCHES "vinc.log_total_bytes \\[uint64\\] = 134217728")
                message(FATAL_ERROR "Log capacity defaults missing: ${output}\n${errors}")
            endif()
            file(GLOB_RECURSE before_logs "${data}/logs/*")
            foreach(path IN LISTS before_logs)
                string(SHA256 key "${path}")
                file(SHA256 "${path}" hash_${key})
                file(TIMESTAMP "${path}" time_${key} "%s" UTC)
            endforeach()
        endif()
    endforeach()
endfunction()
