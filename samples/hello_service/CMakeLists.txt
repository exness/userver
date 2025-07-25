cmake_minimum_required(VERSION 3.14)
project(userver-samples-hello_service CXX)

# /// [find_userver]
find_package(
    userver
    COMPONENTS core
    REQUIRED
)
# Note: If userver was added via add_subdirectory(path/to/userver), then the userver_setup_environment() should be
# called here. /// [find_userver]

# /// [objects]
add_library(${PROJECT_NAME}_objs OBJECT src/say_hello.hpp src/say_hello.cpp src/hello_handler.hpp src/hello_handler.cpp)
target_link_libraries(${PROJECT_NAME}_objs userver::core)
target_include_directories(${PROJECT_NAME}_objs PUBLIC src)
# /// [objects]

# /// [executable]
add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} ${PROJECT_NAME}_objs)
# /// [executable]

# /// [unittests]
add_executable(${PROJECT_NAME}-unittest unittests/say_hello_test.cpp)
target_link_libraries(${PROJECT_NAME}-unittest ${PROJECT_NAME}_objs userver::utest)
add_google_tests(${PROJECT_NAME}-unittest)
# /// [unittests]

# /// [benchmarks]
add_executable(${PROJECT_NAME}_benchmark benchmarks/say_hello_bench.cpp)
target_link_libraries(${PROJECT_NAME}_benchmark ${PROJECT_NAME}_objs userver::ubench)
add_google_benchmark_tests(${PROJECT_NAME}_benchmark)
# /// [benchmarks]

# /// [testsuite] Makes a virtualenv suitable for running pytests from `test` directory and integrates with `ctest`.
userver_testsuite_add_simple()
# /// [testsuite]
