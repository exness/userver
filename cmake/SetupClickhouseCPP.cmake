option(USERVER_DOWNLOAD_PACKAGE_CLICKHOUSECPP "Download and setup clickhouse-cpp" ${USERVER_DOWNLOAD_PACKAGES})

if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    find_package(lz4 REQUIRED)
    include(SetupAbseil)
endif()

if(NOT USERVER_FORCE_DOWNLOAD_PACKAGES)
    if(USERVER_DOWNLOAD_PACKAGE_CLICKHOUSECPP)
        find_package(clickhouse-cpp QUIET)
    else()
        find_package(clickhouse-cpp REQUIRED)
    endif()

    if(clickhouse-cpp_FOUND)
        if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
            target_link_libraries(clickhouse-cpp INTERFACE lz4::lz4 absl::int128)
        endif()
        return()
    endif()
endif()

include(DownloadUsingCPM)
include(SetupAbseil)

cpmaddpackage(
    NAME
    clickhouse-cpp
    VERSION
    2.5.1
    GIT_TAG
    f606f9a3b27d54403ebc6f7b055f4110864fb97c
    GITHUB_REPOSITORY
    ClickHouse/clickhouse-cpp
    SYSTEM
    OPTIONS
    "WITH_SYSTEM_ABSEIL ON"
    "WITH_SYSTEM_LZ4 ON"
    "DEBUG_DEPENDENCIES OFF"
)

if(NOT TARGET clickhouse-cpp-lib::clickhouse-cpp-lib)
    add_library(clickhouse-cpp-lib::clickhouse-cpp-lib ALIAS clickhouse-cpp-lib)
endif()
