project(userver-compiler-rt-parts)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(SOURCES lib/builtins/muloti4.c)
    add_library(${PROJECT_NAME} OBJECT ${SOURCES})
else()
    add_library(${PROJECT_NAME} INTERFACE)
endif()
_userver_install_targets(COMPONENT universal TARGETS ${PROJECT_NAME})
