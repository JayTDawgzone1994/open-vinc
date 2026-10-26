cmake_minimum_required(VERSION 3.20)
file(READ "${VINC_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt" notices)
string(REPLACE "\r\n" "\n" notices "${notices}")

function(require_text text source)
    string(REPLACE "\r\n" "\n" text "${text}")
    string(STRIP "${text}" text)
    string(FIND "${notices}" "${text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Third-party inventory is missing or stale: ${source}")
    endif()
endfunction()

foreach(package IN ITEMS tcl tomlplusplus fmt spdlog sqlite pybind11 cpp-httplib json picosha2)
    file(READ "${VINC_SOURCE_DIR}/library/${package}/revision.txt" revision)
    require_text("${revision}" "${package} revision")
endforeach()

# Check real retained texts, not just package names. Updating a pin must not
# silently ship a previous version's permission or disclaimer wording.
foreach(path IN ITEMS
        library/tcl/upstream/license.terms
        library/tcl/upstream/utf8proc/LICENSE.md
        library/tcl/upstream/libtommath/LICENSE
        library/tcl/upstream/compat/zlib/LICENSE
        library/tomlplusplus/upstream/LICENSE
        library/fmt/upstream/LICENSE
        library/spdlog/upstream/LICENSE
        library/sqlite/upstream/sqlite3.c
        library/pybind11/upstream/LICENSE
        library/cpp-httplib/upstream/LICENSE
        library/json/upstream/LICENSE.MIT
        library/picosha2/upstream/LICENSE)
    if(NOT EXISTS "${VINC_SOURCE_DIR}/${path}")
        # Optional Python/UI sources need not be initialized for a native SDK.
        if(path MATCHES "library/(pybind11|cpp-httplib|json|picosha2)/")
            continue()
        endif()
        message(FATAL_ERROR "Missing license source: ${path}")
    endif()
    file(READ "${VINC_SOURCE_DIR}/${path}" original)
    if(path MATCHES "sqlite3.c$")
        string(FIND "${original}" "*/" end)
        math(EXPR end "${end} + 2")
        string(SUBSTRING "${original}" 0 ${end} original)
    endif()
    require_text("${original}" "${path}")
endforeach()

file(READ "${VINC_SOURCE_DIR}/library/fmt/upstream/include/fmt/format.h" fmt)
string(FIND "${fmt}" "*/" end)
math(EXPR end "${end} + 2")
string(SUBSTRING "${fmt}" 0 ${end} fmt)
require_text("${fmt}" "fmt optional compiled-object exception")
file(STRINGS "${VINC_SOURCE_DIR}/library/fmt/upstream/include/fmt/chrono.h" chrono_attribution
    REGEX "Copyright Paul Dreik" ENCODING UTF-8)
if(NOT chrono_attribution)
    message(FATAL_ERROR "No fmt chrono attribution found")
endif()
require_text("${chrono_attribution}" "fmt chrono duration conversion")

foreach(path IN ITEMS
        library/tomlplusplus/upstream/include/toml++/impl/unicode.hpp
        library/json/upstream/include/nlohmann/detail/output/serializer.hpp
        library/json/upstream/include/nlohmann/detail/conversions/to_chars.hpp
        library/json/upstream/include/nlohmann/thirdparty/hedley/hedley.hpp
        library/json/upstream/include/nlohmann/detail/meta/cpp_future.hpp)
    if(EXISTS "${VINC_SOURCE_DIR}/${path}")
        file(STRINGS "${VINC_SOURCE_DIR}/${path}" attributions
            REGEX "SPDX-FileCopyrightText:|SPDX-License-Identifier:|^[ \t]*// Copyright \\(c\\) 2008-2009 Bjoern Hoehrmann"
            ENCODING UTF-8)
        if(NOT attributions)
            message(FATAL_ERROR "No source attributions found: ${path}")
        endif()
        foreach(attribution IN LISTS attributions)
            require_text("${attribution}" "${path}")
        endforeach()
    endif()
endforeach()

file(READ "${VINC_SOURCE_DIR}/src/vinc/ui/web/package-lock.json" package_lock)
foreach(package IN ITEMS vue @vue/runtime-core @vue/runtime-dom @vue/shared @vue/reactivity)
    string(JSON version GET "${package_lock}" packages "node_modules/${package}" version)
    require_text("${package} ${version}" "${package} version")
    set(path "${VINC_SOURCE_DIR}/src/vinc/ui/web/node_modules/${package}/LICENSE")
    if(EXISTS "${path}")
        file(READ "${path}" original)
        require_text("${original}" "${package}")
    endif()
endforeach()
message(STATUS "Third-party licenses, incorporated-code attributions and fmt exception verified")
