cmake_minimum_required(VERSION 3.16)
project(system_decap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SDC_NATIVE_SOURCES
  native/main.cpp
  native/cli.cpp
  native/probe_registry.cpp
  native/probes/timer.cpp
  native/probes/cache_latency.cpp
  native/probes/tlb.cpp
  native/probes/memory_bandwidth.cpp
  native/probes/cache_bandwidth.cpp
  native/probes/stride_prefetch.cpp
  native/probes/memory_parallelism.cpp
  native/probes/page_policy.cpp
  native/probes/loaded_memory_latency.cpp
  native/probes/store_forwarding.cpp
  native/probes/instruction_fetch.cpp
  native/probes/pipeline.cpp
  native/probes/compute_scaling.cpp
  native/probes/branch_predictor.cpp
  native/probes/branch_structures.cpp
  native/probes/core_latency.cpp
  native/probes/false_sharing.cpp
  native/probes/numa.cpp
  native/probes/rob.cpp
  native/probes/os_overheads.cpp
)

add_executable(sdc-native ${SDC_NATIVE_SOURCES})
target_include_directories(sdc-native PRIVATE native)
target_compile_options(sdc-native PRIVATE
  -O3
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -fno-omit-frame-pointer
)
find_package(Threads REQUIRED)
target_link_libraries(sdc-native PRIVATE Threads::Threads)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME native_help COMMAND sdc-native --help)
  add_test(NAME native_smoke COMMAND sdc-native --profile smoke --memory-mib 8 --duration-ms 15)
  add_test(
    NAME native_list_probes
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/assert_native_probe.py
            $<TARGET_FILE:sdc-native> list
  )
  add_test(
    NAME native_timer_only
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/assert_native_probe.py
            $<TARGET_FILE:sdc-native> timer-only
  )
  add_test(
    NAME native_all_probes_independent
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/assert_native_probe.py
            $<TARGET_FILE:sdc-native> all-independent
  )
endif()
