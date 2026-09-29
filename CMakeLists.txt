cmake_minimum_required(VERSION 3.16)
project(kv_server VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(Threads REQUIRED)

option(ENABLE_THREAD_SANITIZER "Build with ThreadSanitizer when supported" OFF)

add_library(kv_core
    src/command.cpp
    src/command_parser.cpp
    src/kv_store.cpp
    src/client_session.cpp
    src/server.cpp
    src/thread_pool.cpp
)
target_include_directories(kv_core PUBLIC include)
target_link_libraries(kv_core PUBLIC Threads::Threads)
target_compile_options(kv_core PRIVATE -Wall -Wextra -Wpedantic)

if(ENABLE_THREAD_SANITIZER AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(kv_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(kv_core PUBLIC -fsanitize=thread)
endif()

add_executable(kv_server src/main.cpp)
target_link_libraries(kv_server PRIVATE kv_core)

add_executable(kv_client src/client_main.cpp)
target_compile_options(kv_client PRIVATE -Wall -Wextra -Wpedantic)

add_executable(kv_benchmark benchmarks/benchmark_main.cpp)
target_link_libraries(kv_benchmark PRIVATE Threads::Threads)
target_compile_options(kv_benchmark PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(kv_tests tests/test_main.cpp)
    target_link_libraries(kv_tests PRIVATE kv_core)
    target_compile_options(kv_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME kv_unit_tests COMMAND kv_tests unit)
    add_test(NAME kv_integration_tests COMMAND kv_tests integration $<TARGET_FILE:kv_server>)
    add_test(NAME kv_stress_tests COMMAND kv_tests stress $<TARGET_FILE:kv_server>)
    add_test(NAME kv_benchmark_tests COMMAND kv_tests benchmark $<TARGET_FILE:kv_server> $<TARGET_FILE:kv_benchmark>)
endif()
