project(userver-clickhouse-tests-basic-chaos CXX)

add_executable(${PROJECT_NAME} "clickhouse_service.cpp")
target_link_libraries(${PROJECT_NAME} userver::core userver::clickhouse)

userver_chaos_testsuite_add(ENV "TESTSUITE_CLICKHOUSE_SERVER_START_TIMEOUT=120.0")
