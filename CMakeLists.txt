cmake_minimum_required(VERSION 3.16)
project(httpserver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_SANITIZERS "Build with ASan + UBSan" ON)

add_executable(httpserver src/main.cpp src/http.cpp src/router.cpp)
target_compile_options(httpserver PRIVATE -Wall -Wextra -Wpedantic)

if(ENABLE_SANITIZERS)
  target_compile_options(httpserver PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(httpserver PRIVATE -fsanitize=address,undefined)
endif()
