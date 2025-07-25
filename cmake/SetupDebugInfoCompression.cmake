function(setup_linker_debug_info_compression)
    # check_linker_flag is available from 3.18
    if(CMAKE_VERSION LESS 3.18)
        message(STATUS "Using linker debug info compression: none")
        return()
    endif()

    include(CheckLinkerFlag)
    check_linker_flag(CXX "-Wl,--compress-debug-sections=zstd" LINKER_HAS_ZSTD)
    if(NOT LINKER_HAS_ZSTD)
        check_linker_flag(CXX "-Wl,--compress-debug-sections=zlib" LINKER_HAS_GZ)
    endif()

    if(LINKER_HAS_ZSTD)
        message(STATUS "Using linker debug info compression: zstd")
        add_link_options("-Wl,--compress-debug-sections=zstd")
    elseif(LINKER_HAS_GZ)
        message(STATUS "Using linker debug info compression: z")
        add_link_options("-Wl,--compress-debug-sections=zlib")
    else()
        message(STATUS "Using linker debug info compression: none")
    endif()
endfunction()

function(setup_compiler_debug_info_compression)
    userver_is_cxx_compile_option_supported(COMPILER_HAS_ZSTD "-gz=zstd")

    if(COMPILER_HAS_ZSTD)
        message(STATUS "Using compiler debug info compression: zstd")
        add_compile_options("-gz=zstd")
    else()
        message(STATUS "Using compiler debug info compression: z")
        add_compile_options("-gz")
    endif()
endfunction()

set(USERVER_DEBUG_INFO_COMPRESSION
    "auto"
    CACHE STRING "Linker and compiler debug info compression algorithm (z, zstd, none, auto)"
)

if(USERVER_DEBUG_INFO_COMPRESSION STREQUAL "z")
    message(STATUS "Using compiler debug info compression: z")
    add_compile_options("-gz")
    message(STATUS "Using linker debug info compression: z")
    add_link_options("-gz")
elseif(USERVER_DEBUG_INFO_COMPRESSION STREQUAL "zstd")
    message(STATUS "Using linker debug info compression: zstd")
    add_link_options("-gz=zstd")
    message(STATUS "Using compiler debug info compression: zstd")
    add_compile_options("-gz=zstd")
elseif(USERVER_DEBUG_INFO_COMPRESSION STREQUAL "none")
    message(STATUS "Using linker debug info compression: none")
    message(STATUS "Using compiler debug info compression: none")
elseif(USERVER_DEBUG_INFO_COMPRESSION STREQUAL "auto")
    setup_linker_debug_info_compression()
    setup_compiler_debug_info_compression()
else()
    message(FATAL_ERROR "Unknown USERVER_DEBUG_INFO_COMPRESSION value")
endif()
