project(userver-redis-tests CXX)

add_custom_target(${PROJECT_NAME})

add_subdirectory(basic_chaos)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-basic-chaos)

add_subdirectory(database_index)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-database-index)

add_subdirectory(metrics)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-metrics)

add_subdirectory(pubsub)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-pubsub)

add_subdirectory(integration_tests)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-integration)
