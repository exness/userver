project(userver-tool-http-client-perf CXX)

find_package(Boost REQUIRED CONFIG COMPONENTS program_options)

add_executable(${PROJECT_NAME} "httpclient_perf.cpp")
target_link_libraries(${PROJECT_NAME}
    userver::core
    Boost::program_options
)
