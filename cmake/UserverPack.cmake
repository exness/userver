set(CPACK_PACKAGE_VENDOR "userver.tech")
set(CPACK_PACKAGE_DESCRIPTION
    "🐙 userver framework\
    A modern open source asynchronous framework with a rich set of \
    abstractions for fast and comfortable creation of C++ microservices, \
    services and utilities."
)

set(CPACK_PACKAGE_NAME "libuserver-all-dev")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_PACKAGE_VERSION "${USERVER_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${USERVER_MAJOR_VERSION}")
set(CPACK_PACKAGE_VERSION_MINOR "${USERVER_MINOR_VERSION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://userver.tech")
set(CPACK_PACKAGE_CONTACT "Antony Polukhin <antoshkka@userver.tech>")
set(CPACK_RESOURCE_FILE_LICENSE "${USERVER_ROOT_DIR}/LICENSE")

# The installation path directory should have 0755 permissions:
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ
    OWNER_WRITE
    OWNER_EXECUTE
    GROUP_READ
    GROUP_EXECUTE
    WORLD_READ
    WORLD_EXECUTE
)

# DEB dependencies:
execute_process(COMMAND lsb_release -cs OUTPUT_VARIABLE OS_CODENAME)
if(OS_CODENAME MATCHES "^bookworm")
    set(DEPENDENCIES_FILE "debian-12.md")
elseif(OS_CODENAME MATCHES "^bullseye")
    set(DEPENDENCIES_FILE "debian-11.md")
elseif(OS_CODENAME MATCHES "^noble")
    set(DEPENDENCIES_FILE "ubuntu-24.04.md")
elseif(OS_CODENAME MATCHES "^jammy")
    set(DEPENDENCIES_FILE "ubuntu-22.04.md")
elseif(OS_CODENAME MATCHES "^impish")
    set(DEPENDENCIES_FILE "ubuntu-21.04.md")
elseif(OS_CODENAME MATCHES "^focal")
    set(DEPENDENCIES_FILE "ubuntu-20.04.md")
elseif(OS_CODENAME MATCHES "^bionic")
    set(DEPENDENCIES_FILE "ubuntu-18.04.md")
endif()

if(DEPENDENCIES_FILE)
    execute_process(
        COMMAND cat "${USERVER_ROOT_DIR}/scripts/docs/en/deps/${DEPENDENCIES_FILE}"
        COMMAND tr "\n" " "
        COMMAND sed "s/ \\(.\\)/, \\1/g"
        OUTPUT_VARIABLE CPACK_DEBIAN_PACKAGE_DEPENDS
    )
else()
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6")
endif()

# CPack setup is ready. Including it:
include(CPack)
