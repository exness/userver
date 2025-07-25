# Functions for creating targets consisting of base protobuf files and userver asynchronous gRPC adaptors.
#
# On inclusion: - finds Protobuf package - finds gRPC package - sets up project-wide venv-userver-grpc for userver
# protobuf plugin
#
# Provides: - userver_generate_grpc_files function - userver_add_grpc_library function
#
# See the Doxygen documentation on add_grpc_library.
#
# Implementation note: public functions here should be usable even without a direct include of this script, so the
# functions should not rely on non-cache variables being present.

include_guard(GLOBAL)

# Pack initialization into a function to avoid non-cache variable leakage.
function(_userver_prepare_grpc)
    if(USERVER_CONAN)
        find_package(gRPC REQUIRED)
        get_target_property(PROTO_GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)
        get_target_property(PROTO_GRPC_PYTHON_PLUGIN gRPC::grpc_python_plugin LOCATION)
    else()
        include("${CMAKE_CURRENT_LIST_DIR}/SetupGrpc.cmake")
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/SetupProtobuf.cmake")
    set_property(GLOBAL PROPERTY userver_grpc_cpp_plugin "${PROTO_GRPC_CPP_PLUGIN}")
    set_property(GLOBAL PROPERTY userver_grpc_python_plugin "${PROTO_GRPC_PYTHON_PLUGIN}")
    set_property(GLOBAL PROPERTY userver_protobuf_protoc "${PROTOBUF_PROTOC}")
    set_property(GLOBAL PROPERTY userver_protobuf_version "${Protobuf_VERSION}")

    if(NOT USERVER_GRPC_SCRIPTS_PATH)
        get_filename_component(USERVER_DIR "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
        set(USERVER_GRPC_SCRIPTS_PATH "${USERVER_DIR}/scripts/grpc")
    endif()
    set_property(GLOBAL PROPERTY userver_grpc_scripts_path "${USERVER_GRPC_SCRIPTS_PATH}")

    message(STATUS "Protobuf version: ${Protobuf_VERSION}")
    message(STATUS "Protobuf compiler: ${PROTOBUF_PROTOC}")
    message(STATUS "gRPC version: ${gRPC_VERSION}")

    # Used by grpc/CMakeLists.txt
    set(gRPC_VERSION
        "${gRPC_VERSION}"
        PARENT_SCOPE
    )

    if(Protobuf_INCLUDE_DIR)
        set_property(GLOBAL PROPERTY userver_protobuf_import_dir "${Protobuf_INCLUDE_DIR}")
    elseif(protobuf_INCLUDE_DIR)
        set_property(GLOBAL PROPERTY userver_protobuf_import_dir "${protobuf_INCLUDE_DIR}")
    elseif(Protobuf_INCLUDE_DIRS)
        set_property(GLOBAL PROPERTY userver_protobuf_import_dir "${Protobuf_INCLUDE_DIRS}")
    elseif(protobuf_INCLUDE_DIRS)
        set_property(GLOBAL PROPERTY userver_protobuf_import_dir "${protobuf_INCLUDE_DIRS}")
    else()
        message(FATAL_ERROR "Invalid Protobuf package")
    endif()

    if(NOT Protobuf_VERSION)
        message(FATAL_ERROR "Invalid Protobuf package")
    endif()
    if(NOT gRPC_VERSION)
        message(FATAL_ERROR "Invalid gRPC package")
    endif()
    if(NOT PROTOBUF_PROTOC)
        message(FATAL_ERROR "protoc not found")
    endif()
    if(NOT PROTO_GRPC_CPP_PLUGIN)
        message(FATAL_ERROR "grpc_cpp_plugin not found")
    endif()
    if(NOT PROTO_GRPC_PYTHON_PLUGIN)
        message(FATAL_ERROR "grpc_python_plugin not found")
    endif()

    include("${CMAKE_CURRENT_LIST_DIR}/UserverTestsuite.cmake")

    get_property(protobuf_category GLOBAL PROPERTY userver_protobuf_version_category)
    set(requirements_name "requirements-${protobuf_category}.txt")

    userver_venv_setup(
        NAME userver-grpc
        PYTHON_OUTPUT_VAR USERVER_GRPC_PYTHON_BINARY
        REQUIREMENTS "${USERVER_GRPC_SCRIPTS_PATH}/${requirements_name}"
        UNIQUE
    )
    set_property(GLOBAL PROPERTY userver_grpc_python_binary "${USERVER_GRPC_PYTHON_BINARY}")
endfunction()

_userver_prepare_grpc()

function(userver_generate_grpc_files)
    set(options)
    set(one_value_args CPP_FILES CPP_USRV_FILES GENERATED_INCLUDES SOURCE_PATH OUTPUT_PATH)
    set(multi_value_args PROTOS INCLUDE_DIRECTORIES)
    cmake_parse_arguments(GEN_RPC "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    get_property(USERVER_GRPC_SCRIPTS_PATH GLOBAL PROPERTY userver_grpc_scripts_path)
    get_property(USERVER_GRPC_PYTHON_BINARY GLOBAL PROPERTY userver_grpc_python_binary)
    get_property(PROTO_GRPC_CPP_PLUGIN GLOBAL PROPERTY userver_grpc_cpp_plugin)
    get_property(PROTO_GRPC_PYTHON_PLUGIN GLOBAL PROPERTY userver_grpc_python_plugin)
    get_property(PROTOBUF_PROTOC GLOBAL PROPERTY userver_protobuf_protoc)
    get_property(USERVER_PROTOBUF_IMPORT_DIR GLOBAL PROPERTY userver_protobuf_import_dir)
    get_property(Protobuf_VERSION GLOBAL PROPERTY userver_protobuf_version)
    set(PROTO_GRPC_USRV_PLUGIN "${USERVER_GRPC_SCRIPTS_PATH}/protoc_usrv_plugin.sh")

    if(GEN_RPC_INCLUDE_DIRECTORIES)
        set(include_options)
        foreach(include ${GEN_RPC_INCLUDE_DIRECTORIES})
            if(NOT IS_ABSOLUTE ${include})
                if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${include})
                    set(include ${CMAKE_CURRENT_SOURCE_DIR}/${include})
                elseif(EXISTS ${CMAKE_SOURCE_DIR}/${include})
                    set(include ${CMAKE_SOURCE_DIR}/${include})
                endif()
            endif()
            get_filename_component(include "${include}" REALPATH BASE_DIR "/")
            if(EXISTS ${include})
                list(APPEND include_options -I ${include})
            else()
                message(WARNING "Include directory ${include} for protoc generator not found")
            endif()
        endforeach()
    endif()

    if(NOT "${GEN_RPC_OUTPUT_PATH}" STREQUAL "")
        if(NOT IS_ABSOLUTE "${GEN_RPC_OUTPUT_PATH}")
            message(SEND_ERROR "OUTPUT_PATH='${GEN_RPC_OUTPUT_PATH}' is a relative path, which is unsupported.")
        endif()
        set(GENERATED_PROTO_DIR "${GEN_RPC_OUTPUT_PATH}")
    else()
        set(GENERATED_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
    endif()

    get_filename_component(GENERATED_PROTO_DIR "${GENERATED_PROTO_DIR}" REALPATH BASE_DIR "/")

    if(NOT "${GEN_RPC_SOURCE_PATH}" STREQUAL "")
        if(NOT IS_ABSOLUTE "${GEN_RPC_SOURCE_PATH}")
            message(SEND_ERROR "SOURCE_PATH='${GEN_RPC_SOURCE_PATH}' is a relative path, which is unsupported.")
        endif()
        set(root_path "${GEN_RPC_SOURCE_PATH}")
    else()
        set(root_path "${CMAKE_CURRENT_SOURCE_DIR}/proto")
    endif()

    get_filename_component(root_path "${root_path}" REALPATH BASE_DIR "/")

    set(pyi_out_param "")
    if(Protobuf_VERSION VERSION_GREATER_EQUAL "3.20.0")
        set(pyi_out_param "--pyi_out=${GENERATED_PROTO_DIR}")
    endif()

    set(protoc_flags
        "--cpp_out=${GENERATED_PROTO_DIR}"
        "--grpc_out=${GENERATED_PROTO_DIR}"
        "--usrv_out=${GENERATED_PROTO_DIR}"
        "--python_out=${GENERATED_PROTO_DIR}"
        "--grpc_python_out=${GENERATED_PROTO_DIR}"
        ${pyi_out_param}
        -I
        "${root_path}"
        -I
        "${USERVER_PROTOBUF_IMPORT_DIR}"
        ${include_options}
        "--plugin=protoc-gen-grpc=${PROTO_GRPC_CPP_PLUGIN}"
        "--plugin=protoc-gen-usrv=${PROTO_GRPC_USRV_PLUGIN}"
        "--plugin=protoc-gen-grpc_python=${PROTO_GRPC_PYTHON_PLUGIN}"
    )
    if(Protobuf_VERSION VERSION_GREATER_EQUAL "3.12.0" AND Protobuf_VERSION VERSION_LESS "3.15.0")
        list(APPEND protoc_flags "--experimental_allow_proto3_optional")
    endif()

    set(proto_abs_paths)
    set(proto_rel_paths)
    foreach(proto_file ${GEN_RPC_PROTOS})
        get_filename_component(proto_file "${proto_file}" REALPATH BASE_DIR "${root_path}")
        get_filename_component(path "${proto_file}" DIRECTORY)
        get_filename_component(name_base "${proto_file}" NAME_WLE)
        file(RELATIVE_PATH path_base "${root_path}" "${path}/${name_base}")
        list(APPEND proto_abs_paths "${proto_file}")
        list(APPEND proto_rel_paths "${path_base}")
    endforeach()
    set(did_generate_proto_sources FALSE)

    set(proto_dependencies_globs ${GEN_RPC_INCLUDE_DIRECTORIES})
    list(TRANSFORM proto_dependencies_globs APPEND "/*.proto")
    list(APPEND proto_dependencies_globs "${root_path}/*.proto" "${USERVER_GRPC_SCRIPTS_PATH}/*")
    file(GLOB_RECURSE proto_dependencies ${proto_dependencies_globs})

    set(generated_cpps)
    set(generated_usrv_cpps)
    set(pyi_init_files)

    foreach(proto_rel_path ${proto_rel_paths})
        list(APPEND generated_cpps ${GENERATED_PROTO_DIR}/${proto_rel_path}.pb.h
             ${GENERATED_PROTO_DIR}/${proto_rel_path}.pb.cc
        )

        set(has_service_sources FALSE)
        # The files have not been generated yet, so we can't get the information on services from protoc at this stage.
        # HACK: file contains service <=> a line starts with 'service '
        file(
            STRINGS "${root_path}/${proto_rel_path}.proto" proto_service_string
            REGEX "^service "
            ENCODING UTF-8
        )
        if(proto_service_string)
            set(has_service_sources TRUE)
        endif()

        if(has_service_sources)
            list(
                APPEND
                generated_usrv_cpps
                ${GENERATED_PROTO_DIR}/${proto_rel_path}_client.usrv.pb.hpp
                ${GENERATED_PROTO_DIR}/${proto_rel_path}_client.usrv.pb.cpp
                ${GENERATED_PROTO_DIR}/${proto_rel_path}_service.usrv.pb.hpp
                ${GENERATED_PROTO_DIR}/${proto_rel_path}_service.usrv.pb.cpp
            )
            list(APPEND generated_cpps ${GENERATED_PROTO_DIR}/${proto_rel_path}.grpc.pb.h
                 ${GENERATED_PROTO_DIR}/${proto_rel_path}.grpc.pb.cc
            )
        endif()

        if(pyi_out_param)
            get_filename_component(proto_rel_dir "${proto_rel_path}" DIRECTORY)
            set(pyi_init_file "${CMAKE_CURRENT_BINARY_DIR}/proto/${proto_rel_dir}/__init__.py")
            file(WRITE "${pyi_init_file}" "")
            list(APPEND pyi_init_files "${pyi_init_file}")
        endif()
    endforeach()

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/proto")

    _userver_initialize_codegen_flag()
    add_custom_command(
        OUTPUT ${generated_cpps} ${generated_usrv_cpps}
        COMMAND ${CMAKE_COMMAND} -E env "USERVER_GRPC_PYTHON_BINARY=${USERVER_GRPC_PYTHON_BINARY}" "${PROTOBUF_PROTOC}"
                ${protoc_flags} ${proto_abs_paths}
        DEPENDS ${proto_dependencies}
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Running gRPC C++ protocol buffer compiler for ${root_path}" ${CODEGEN}
    )
    _userver_codegen_register_files("${generated_cpps};${generated_usrv_cpps}")
    message(STATUS "Scheduled build-time generation of protos in ${root_path}")

    set_source_files_properties(${generated_cpps} ${generated_usrv_cpps} ${pyi_init_files} PROPERTIES GENERATED 1)

    if(GEN_RPC_GENERATED_INCLUDES)
        set(${GEN_RPC_GENERATED_INCLUDES}
            ${GENERATED_PROTO_DIR}
            PARENT_SCOPE
        )
    endif()
    if(GEN_RPC_CPP_FILES)
        set(${GEN_RPC_CPP_FILES}
            ${generated_cpps}
            PARENT_SCOPE
        )
    endif()
    if(GEN_RPC_CPP_USRV_FILES)
        set(${GEN_RPC_CPP_USRV_FILES}
            ${generated_usrv_cpps}
            PARENT_SCOPE
        )
    endif()
endfunction()

function(userver_add_grpc_library NAME)
    set(options)
    set(one_value_args SOURCE_PATH OUTPUT_PATH)
    set(multi_value_args PROTOS INCLUDE_DIRECTORIES)
    cmake_parse_arguments(RPC_LIB "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    userver_generate_grpc_files(
        PROTOS
        ${RPC_LIB_PROTOS}
        INCLUDE_DIRECTORIES
        ${RPC_LIB_INCLUDE_DIRECTORIES}
        SOURCE_PATH
        "${RPC_LIB_SOURCE_PATH}"
        OUTPUT_PATH
        "${RPC_LIB_OUTPUT_PATH}"
        GENERATED_INCLUDES
        include_paths
        CPP_FILES
        generated_sources
        CPP_USRV_FILES
        generated_usrv_sources
    )
    add_library(${NAME} STATIC ${generated_sources} ${generated_usrv_sources})
    target_compile_options(${NAME} PUBLIC -Wno-unused-parameter)
    target_include_directories(${NAME} SYSTEM PUBLIC $<BUILD_INTERFACE:${include_paths}>)
    target_link_libraries(${NAME} PUBLIC userver::grpc)
endfunction()
