add_test(
    NAME "${PROJECT_NAME}-sql-coverage_empty"
    COMMAND "${TESTSUITE_PYTHON_BINARY}" -m pytest ${CMAKE_CURRENT_SOURCE_DIR}
    WORKING_DIRECTORY "${USERVER_ROOT_DIR}/testsuite/pytest_plugins"
)

set_tests_properties("${PROJECT_NAME}-sql-coverage_empty" PROPERTIES TIMEOUT 600)
