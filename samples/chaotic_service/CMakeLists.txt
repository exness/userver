cmake_minimum_required(VERSION 3.14)
project(userver-samples-chaotic_service CXX)

# /// [find_userver]
find_package(
    userver
    COMPONENTS core chaotic
    REQUIRED
)
# Note: If userver was added via add_subdirectory(path/to/userver), then the userver_setup_environment() should be
# called here. /// [find_userver]

# /// [objects]
add_library(${PROJECT_NAME}_objs OBJECT src/say_hello.hpp src/say_hello.cpp src/hello_service.hpp src/hello_service.cpp)
target_link_libraries(${PROJECT_NAME}_objs userver::core)
target_include_directories(${PROJECT_NAME}_objs PUBLIC src)
# /// [objects]

# /// [chaotic] cmake
file(GLOB_RECURSE SCHEMAS ${CMAKE_CURRENT_SOURCE_DIR}/schemas/*.yaml)
userver_target_generate_chaotic(
    ${PROJECT_NAME}-chgen
    # Generate serializers for responses
    GENERATE_SERIALIZERS
    # Map '/components/schemas/*' JSONSchema types to C++ types in 'samples::STEM' namespace
    LAYOUT "/components/schemas/([^/]*)/=samples::{stem}::{0}"
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/src
    SCHEMAS ${SCHEMAS}
    RELATIVE_TO ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(${PROJECT_NAME}_objs ${PROJECT_NAME}-chgen)
# /// [chaotic]

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
