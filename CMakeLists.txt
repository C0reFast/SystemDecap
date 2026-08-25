cmake_minimum_required(VERSION 3.16)
project(system_decap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(sdc-native native/main.cpp)
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
  add_test(NAME native_help COMMAND sdc-native --help)
  add_test(NAME native_smoke COMMAND sdc-native --profile smoke --memory-mib 8 --duration-ms 15)
endif()
