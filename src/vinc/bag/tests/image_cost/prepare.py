"""Prepare isolated typed/generic Recorder consumers; never modify production targets."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


BASELINE = "bdde615a5182741e0f6e30b071deef2b8ed1c068"
UNUSED_TYPES = 128


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("repository", type=Path)
    parser.add_argument("work", type=Path)
    args = parser.parse_args()
    root = args.repository.resolve()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    files = ["include/vinc/bag/bag.hpp", "include/vinc/bag/detail/database.hpp",
             "include/vinc/bag/error.hpp", "src/database.cpp", "src/recorder.cpp",
             "module/recorder_module.cpp"]
    hashes = {}
    for variant in ("typed", "generic"):
        source = work / variant
        for relative in files:
            repository_path = "src/vinc/bag/" + relative
            data = (subprocess.check_output(["git", "show", BASELINE + ":" + repository_path], cwd=root)
                    if variant == "typed" else (root / repository_path).read_bytes())
            destination = source / "bag" / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            hashes[f"{variant}/{relative}"] = hashlib.sha256(data).hexdigest()
        shutil.copyfile(root / "library/sqlite/upstream/sqlite3.h", source / "sqlite3.h")
        fixture = root / "src/vinc/sdk/tests/installed_recorder"
        shutil.copyfile(fixture / "app.config", source / "app.config")
        module = (fixture / "module.cpp").read_text("utf-8")
        # Identical workload for both adapters: one paced topic, 1,000 records.
        module = module.replace("record_count = 64", "record_count = 1000")
        module = module.replace("ring_slots = 128", "ring_slots = 1024")
        module = module.replace("message_bytes = 4096", "message_bytes = 131072")
        module = module.replace("#include <optional>", "#include <optional>\n#include <thread>")
        module = module.replace("public:\n", "    std::uint64_t sequence_ = 0;\npublic:\n")
        module = module.replace("for (std::uint64_t sequence = 1; sequence <= record_count; ++sequence) {",
                                "constexpr std::size_t records_per_command = 100;\n"
                                "            for (std::size_t index = 0; index < records_per_command; ++index) {\n"
                                "                const auto sequence = ++sequence_;")
        module = module.replace('args.size() == 1 && args[0] == "burst"',
                                'args.size() == 2 && args[0] == "burst"')
        module = module.replace("constexpr std::size_t payload_bytes = 32;",
                                'const std::size_t payload_bytes = args[1] == "large" ? 65536 : 1024;\n'
                                '            constexpr auto publication_interval = std::chrono::milliseconds(2);')
        module = module.replace("message->data().sequence = sequence;",
                                "std::this_thread::sleep_for(publication_interval);\n"
                                "                message->data().sequence = sequence;")
        (source / "module.cpp").write_text(module, encoding="utf-8")
        (source / "startup.tcl").write_text('puts "COST_READY"\n', encoding="utf-8")
        messages = source / "messages"
        messages.mkdir()
        shutil.copyfile(fixture / "messages/RecorderSample.msg", messages / "RecorderSample.msg")
        for index in range(UNUSED_TYPES):
            (messages / f"CostUnused{index}.msg").write_text(
                "uint64_t sequence;\nstring label;\nuint8_t[] payload;\n", encoding="utf-8")
        (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(RecorderCost LANGUAGES CXX)
find_package(vinc CONFIG REQUIRED)
if(MSVC)
    # The unchanged typed UI adapter also expands the same large catalog.
    add_compile_options(/bigobj)
endif()
add_message_dir("${CMAKE_CURRENT_SOURCE_DIR}/messages")
add_library(cost_bag STATIC bag/src/database.cpp bag/src/recorder.cpp)
set_target_properties(cost_bag PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(cost_bag BEFORE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/bag/include")
target_include_directories(cost_bag PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cost_bag PUBLIC vinc::message_bus_client PRIVATE vinc::detail_sqlite vinc::osal)
vinc_add_module_plugin(cost_recorder bag/module/recorder_module.cpp)
target_include_directories(cost_recorder BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bag/include")
target_link_libraries(cost_recorder PRIVATE vinc::sdk cost_bag vinc::messages)
set_target_properties(cost_recorder PROPERTIES VINC_MODULE_TYPE bagRecorder
    VINC_STOP_TIMEOUT_CONFIG bag_stop_timeout VINC_SYSTEM_MODULE_NAME recorder EXCLUDE_FROM_ALL TRUE)
set_property(GLOBAL PROPERTY VINC_APP_BAG_TARGET cost_recorder)
add_module(recorder_probe MODULE_TYPE_NAME RecorderProbe SOURCES module.cpp)
target_include_directories(recorder_probe BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bag/include")
target_link_libraries(recorder_probe PRIVATE cost_bag)
vinc_add_app(CostApp INSTANCE_NAME recorder-image-cost
    VINC_BUILD_CONFIG app.config STARTUP_SCRIPT startup.tcl)
''', encoding="utf-8")
    (work / "manifest.json").write_text(json.dumps({
        "typed_revision": BASELINE, "inputs_sha256": hashes, "unused_types": UNUSED_TYPES,
        "records": 1000, "payload_bytes": [1024, 65536], "publisher_sleep_ms": 2,
        "ring_slots": 1024, "ring_message_bytes": 131072,
        "note": "Both adapters use the same current prebuilt SDK. Full build means external App only."
    }, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
