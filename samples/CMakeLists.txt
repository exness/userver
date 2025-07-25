project(userver-samples CXX)

add_custom_target(${PROJECT_NAME})

# Imitate an installed userver in samples.
file(WRITE "${CMAKE_BINARY_DIR}/package_stubs/userverConfig.cmake" )

add_subdirectory(config_service)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-config_service)

add_subdirectory(testsuite-support)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-testsuite-support)

if(USERVER_FEATURE_CHAOTIC)
    add_subdirectory(chaotic_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-chaotic_service)

    if(USERVER_FEATURE_CHAOTIC_EXPERIMENTAL)
        add_subdirectory(chaotic_openapi_service)
        add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-chaotic-openapi_service)
    endif()
endif()

if(USERVER_FEATURE_OTLP)
    add_subdirectory(otlp_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-otlp_service)
endif()

if(USERVER_FEATURE_GRPC)
    add_subdirectory(grpc-generic-proxy)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-grpc-generic-proxy)

    add_subdirectory(grpc_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-grpc_service)

    add_subdirectory(grpc_middleware_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-grpc_middleware_service)
endif()

add_subdirectory(json2yaml)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-json2yaml)

add_subdirectory(dns-resolver)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-dns-resolver)

add_subdirectory(http-client-perf)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-http-client-perf)

add_subdirectory(hello_service)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-hello_service)

add_subdirectory(embedded_files)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-embedded_files)

add_subdirectory(http_middleware_service)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-http_middleware_service)

add_subdirectory(multipart_service)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-multipart_service)

add_subdirectory(netcat)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-netcat)

add_subdirectory(websocket_service)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-websocket_service)

add_subdirectory(https_service)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-https_service)

add_subdirectory(tcp_service)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-tcp_service)

add_subdirectory(tcp_full_duplex_service)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-tcp_full_duplex_service)

if(USERVER_FEATURE_MONGODB)
    add_subdirectory(http_caching)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-http_caching)

    add_subdirectory(mongo_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-mongo_service)

    add_subdirectory(mongo-support)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-mongo-support)
endif()

if(USERVER_FEATURE_POSTGRESQL)
    add_subdirectory(postgres_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-postgres_service)

    add_subdirectory(postgres_auth)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-postgres_auth)

    add_subdirectory(digest_auth_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-digest_auth)

    add_subdirectory(postgres-support)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-postgres-support)

    add_subdirectory(postgres_cache_order_by)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-postgres_cache_order_by)
endif()

add_subdirectory(production_service)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-production_service)

add_subdirectory(static_service)
add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-static_service)

if(USERVER_FEATURE_REDIS)
    add_subdirectory(redis_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-redis_service)
endif()

if(USERVER_FEATURE_CLICKHOUSE)
    add_subdirectory(clickhouse_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-clickhouse_service)
endif()

if(USERVER_FEATURE_RABBITMQ)
    add_subdirectory(rabbitmq_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-rabbitmq_service)
endif()

if(USERVER_FEATURE_KAFKA)
    add_subdirectory(kafka_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-kafka_service)
endif()

if(USERVER_FEATURE_MYSQL)
    add_subdirectory(mysql_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-mysql_service)
endif()

if(USERVER_FEATURE_YDB AND USERVER_FEATURE_MONGODB)
    add_subdirectory(ydb_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-ydb_service)
endif()

if(USERVER_FEATURE_SQLITE)
    add_subdirectory(sqlite_service)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-sqlite_service)
endif()

if(USERVER_FEATURE_S3API)
    add_subdirectory(s3api)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}-s3api)
endif()

# Clean up fake userver installation above to avoid messing with find_package(userver) in the parent project.
file(REMOVE "${CMAKE_BINARY_DIR}/package_stubs/userverConfig.cmake")
