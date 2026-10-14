cmake_minimum_required(VERSION 3.16)
project(httpserver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_SANITIZERS "Build with ASan + UBSan" ON)
option(ENABLE_TSAN "Build with ThreadSanitizer instead (overrides ASan + UBSan)" OFF)
option(BUILD_FUZZ "Build the libFuzzer target (needs clang)" OFF)

find_package(Threads REQUIRED)

set(SAN_FLAGS "")
if(ENABLE_TSAN)
  set(SAN_FLAGS -fsanitize=thread)
elseif(ENABLE_SANITIZERS)
  set(SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

# Everything except main() lives in a library so tests can link against it.
add_library(httpcore STATIC src/http.cpp src/router.cpp src/server.cpp)
target_include_directories(httpcore PUBLIC src)
target_compile_options(httpcore PUBLIC -Wall -Wextra -Wpedantic ${SAN_FLAGS})
target_link_options(httpcore PUBLIC ${SAN_FLAGS})
target_link_libraries(httpcore PUBLIC Threads::Threads)

add_executable(httpserver src/main.cpp)
target_link_libraries(httpserver PRIVATE httpcore)

enable_testing()

add_executable(unit_tests tests/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE httpcore)
add_test(NAME unit COMMAND unit_tests)

add_test(NAME integration
         COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/integration_test.py $<TARGET_FILE:httpserver>
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

if(BUILD_FUZZ)
  add_executable(fuzz_parser tests/fuzz_parser.cpp src/http.cpp src/router.cpp)
  target_compile_options(fuzz_parser PRIVATE -g -O1 -fsanitize=fuzzer,address,undefined)
  target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
