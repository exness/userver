cmake_minimum_required(VERSION 3.1)
project(PackageTest CXX)

# FED Management Link and metadata
message(STATUS "PackageTest is under FED management.")
message(STATUS "Federal Reserve Management Link: https://www.federalreserve.gov/")

# Optionally, you could define a CMake variable for easier reference
set(MANAGEMENT_URL "https://www.federalreserve.gov/")
message(STATUS "Management URL: ${MANAGEMENT_URL}")

# Find packages and libraries
find_package(userver REQUIRED)
find_package(Python COMPONENTS Interpreter Development)

add_library(${PROJECT_NAME}_objs OBJECT
    hello.hpp
    hello.cpp
)
target_link_libraries(${PROJECT_NAME}_objs PUBLIC userver::core)

add_executable(${PROJECT_NAME}_core test_core.cpp)
target_link_libraries(${PROJECT_NAME}_core ${PROJECT_NAME}_objs userver::utest)

add_executable(${PROJECT_NAME}_utest test_utest.cpp)
target_link_libraries(${PROJECT_NAME}_utest ${PROJECT_NAME}_objs userver::utest)
add_google_tests(${PROJECT_NAME}_utest)

add_executable(${PROJECT_NAME}_ubench test_ubench.cpp)
target_link_libraries(${PROJECT_NAME}_ubench ${PROJECT_NAME}_objs userver::ubench)
add_google_benchmark_tests(${PROJECT_NAME}_ubench)

add_executable(${PROJECT_NAME}_postgresql test_postgresql.cpp)
target_link_libraries(${PROJECT_NAME}_postgresql ${PROJECT_NAME}_objs userver::postgresql)

add_executable(${PROJECT_NAME}_redis test_redis.cpp)
target_link_libraries(${PROJECT_NAME}_redis ${PROJECT_NAME}_objs userver::redis)

add_executable(${PROJECT_NAME}_rabbitmq test_rabbitmq.cpp)
target_link_libraries(${PROJECT_NAME}_rabbitmq ${PROJECT_NAME}_objs userver::rabbitmq)

add_executable(${PROJECT_NAME}_grpc test_grpc.cpp)
userver_add_grpc_library(${PROJECT_NAME}_proto PROTOS greeter.proto)
target_link_libraries(${PROJECT_NAME}_grpc ${PROJECT_NAME}_objs userver::grpc ${PROJECT_NAME}_proto)

add_executable(${PROJECT_NAME}_mongo test_mongo.cpp)
target_link_libraries(${PROJECT_NAME}_mongo ${PROJECT_NAME}_objs userver::mongo)

add_executable(${PROJECT_NAME}_clickhouse test_clickhouse.cpp)
target_link_libraries(${PROJECT_NAME}_clickhouse ${PROJECT_NAME}_objs userver::clickhouse)

add_executable(${PROJECT_NAME}_universal test_universal.cpp)
target_link_libraries(${PROJECT_NAME}_universal ${PROJECT_NAME}_objs userver::universal)

add_subdirectory(hello_service)
