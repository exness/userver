# Configure, Build and Install

@anchor quick_start_for_beginners
## Quick start for beginners

1\. @ref ways_to_get_userver "Get userver".

2\. Create a new service project with the following command:

```shell
userver-create-service [--grpc] [--mongo] [--postgresql] myservice
```

This will create `myservice` dir, relative to the current working directory.

If the command above is not found, see @ref service_templates.

To use additional userver libraries later, see @ref service_templates_libraries.

3\. Build and test your service. Run in the service project root:

```shell
make build-debug && \
make test-debug
```
If the tests fail and you see messages like "No XXX installation found. Install it...", then the database server is not
installed in the environment you are running tests. Do not hesitate to install the missing database servers, like
PostgreSQL, MongoDB and others.

4\. To get a feel for how the service runs without needing to set up full production environment, run:

```shell
myservice$ make start-debug
```

Wait until the service starts, then try to send a request.

```shell
curl http://127.0.0.1:8080/hello?name=userver
```

The answer should be something like this:

```shell
> curl http://127.0.0.1:8080/hello?name=userver
Hello, userver!
```

5\. (Optional) Add the service project to Git:

```shell
myservice$ git init && git add . && git commit -m "Initial commit"
```

Push it to GitHub
(see [its documentation](https://docs.github.com/en/migrations/importing-source-code/using-the-command-line-to-import-source-code/adding-locally-hosted-code-to-github#adding-a-local-repository-to-github-using-git)).

There are preconfigured GitHub CI checks that run ctest tests.

Now you are ready for fast and comfortable creation of C++ microservices, services and utilities!

## Quick start for beginners (old way, pre userver 2.8)

@warning Service template repositories are deprecated, because possible sets of userver libraries to include cause
an exponential growth in the number of wanted repositories. For userver 2.9 and later, use
the @ref quick_start_for_beginners "service project generator script" instead.

1\. Press the "Use this template" button at the top right of the appropriate service template repo page:

| Link                                                             | Contains               |
|------------------------------------------------------------------|------------------------|
| https://github.com/userver-framework/service_template            | HTTP                   |
| https://github.com/userver-framework/pg_service_template         | HTTP, PostgreSQL       |
| https://github.com/userver-framework/pg_grpc_service_template    | HTTP, PostgreSQL, gRPC |
| https://github.com/userver-framework/mongo_grpc_service_template | HTTP, MongoDB, gRPC    |

2\. Clone the service:

```shell
git clone https://github.com/your-username/your-service.git && cd your-service
```

3\. Give a proper name to your service and replace all the occurrences of "*service_template" string with that name:

```shell
find . -not -path "./third_party/*" -not -path ".git/*" -not -path './build-*' -type f | xargs sed -i 's/service_template/YOUR_SERVICE_NAME/g'
```

4\. @ref ways_to_get_userver "Get userver".

5\. Build and test the service, see steps 3-4 from the @ref quick_start_for_beginners "updated instruction above".

@anchor service_templates
## Service templates for userver based services

To create a new service project, run:

```shell
userver-create-service [--grpc] [--mongo] [--postgresql] myservice
```

* Project directory will be `myservice`, relative to the current working directory;
* service name will be the last segment of the path;
* without feature flags, the service only has some stubs for HTTP handlers.

If you use @ref devcontainers "Dev Containers", or if you use CPM to download userver,
run this script to get `userver-create-service` command:

@ref service-template/userver-create-service.sh

If instead of installing userver you are planning to build userver as a subdirectory,
call the script from userver directory:

```shell
path/to/userver/scripts/userver-create-service [--grpc] [--mongo] [--postgresql] myservice
```

You'll need to @ref ways_to_get_userver "get userver" before proceeding with local development.

More information on the project directory structure can be found
@ref scripts/docs/en/userver/tutorial/hello_service.md "here".

@anchor service_templates_libraries
### Using additional userver libraries in service templates

The service templates allow to kickstart the development of your production-ready service,
but there can't be a repo for each and every combination of userver libraries.
To use additional userver libraries, e.g. `userver::grpc`, add to the root `CMakeLists.txt`:

```cmake
find_package(userver COMPONENTS core grpc QUIET)
```

Then add the corresponding option to @ref service_templates_presets "cmake presets",
e.g. `"USERVER_FEATURE_GRPC": "ON"`.

@see @ref userver_libraries

See @ref tutorial_services for minimal usage examples of various userver libraries.

@anchor service_templates_presets
### Managing cmake options in service templates

@note If you use @ref userver_install "installed userver", then for most options to take effect, you need to uninstall
userver (if already installed), then install again, passing the desired cmake options.

Service templates use [cmake presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html) for managing
service's cmake options. If you DON'T use installed userver and build userver together with the service,
then userver also takes its @ref cmake_options "cmake options" from there.

If an option has semantic meaning (should be committed to VCS and applied in CI),
then it should be added to `CMakePresets.json`:

```json
{
  "configurePresets": [
    {
      "name": "common-flags",
      "cacheVariables": {
        "USERVER_FEATURE_GRPC": "ON",
        "USERVER_SOME_OPTION": "WHAT"
      }
    }
  ]
}
```

If an option only configures local build (should NOT be committed to VCS and applied in CI),
then it should instead be added to `CMakeUserPresets.json`:

* @ref service-template/CMakeUserPresets.json.example

Service's `Makefile` supports custom presets through additional targets like
`cmake-debug-custom`, `cmake-release-custom`, `build-debug-custom`, etc.

@anchor ways_to_get_userver
## Ways to get userver

You can download prebuilt userver using one of the following ways:

1. @ref devcontainers "Docker (Dev Containers)" ← recommended for beginners;
2. @ref docker_with_ubuntu_22_04 "Docker (manual)";
3. @ref prebuilt_deb_package "prebuilt Debian package";
4. @ref userver_conan "Conan";

Alternatively, install @ref scripts/docs/en/userver/build/dependencies.md "build dependencies" for userver,
then build userver in one of the following ways:

5. @ref userver_install "install userver";
6. let the service template download and build userver as a subdirectory using (@ref userver_cpm "CPM");
7. pass a path to custom userver location to `download_userver()`, then let the service
   @ref service_templates_presets "build userver as a subdirectory".

@anchor devcontainers
## Dev Containers

Dev Containers is the easiest and least problematic way to get prebuilt userver together with its dependencies.

1. Install Docker

   * Linux:
     ```shell
     curl https://get.docker.com/ | sudo sh
     ```
     Allow
     [usage without sudo](https://docs.docker.com/engine/install/linux-postinstall/#manage-docker-as-a-non-root-user)
     (reboot is required) and
     [configure service start on boot](https://docs.docker.com/engine/install/linux-postinstall/#configure-docker-to-start-on-boot-with-systemd).

   * [macOS](https://docs.docker.com/desktop/setup/install/mac-install/)

2. Install IDE extension

   * [VSCode](https://code.visualstudio.com/docs/devcontainers/containers): `ms-vscode-remote.remote-containers`
   * [CLion](https://www.jetbrains.com/help/clion/connect-to-devcontainer.html): "Dev Containers" (**note:** beta)

3. Open the service project

   * See @ref service_templates on how to create a new service from template
   * If CMake asks to configure, deny
   * For CLion, please use JetBrains Gateway to open the project, otherwise CLion gets confused

4. Agree to reopen the project in a Dev Container

5. The Docker container for development will automatically be downloaded (~6GB, may take a while), unpacked and run
   using the config from `.devcontainer` directory

6. On the CMake panel, `Configure > Select Preset > Debug > Do Configure`

7. Open any C++ file in the project, IntelliSense will think for a few seconds, then should start to work properly

8. (Optional)
   [Share Git credentials](https://code.visualstudio.com/remote/advancedcontainers/sharing-git-credentials)
   with the container to perform VCS operations from the IDE

9. You can configure, build, debug and run tests using either the IDE itself or `Makefile` (see the service's README.md)
   using the IDE's integrated terminal

## Downloading packages using CPM

userver uses [CPM](https://github.com/cpm-cmake/CPM.cmake) for downloading missing packages.

By default, we first try to find the corresponding system package.
With `USERVER_FORCE_DOWNLOAD_PACKAGES=ON`, no such attempt is made, and we skip straight to `CPMAddPackage`.
This can be useful to guarantee that the build uses the latest (known to userver) versions of third-party packages.

You can control the usage of CPM with `USERVER_DOWNLOAD_*` options. See @ref cmake_options.
For example, `USERVER_DOWNLOAD_PACKAGES=OFF` is useful for CI and other controlled environments
to make sure that no download attempts are made, which ensures repeatability and the best CI build speed.

For details on the download options, refer to [CPM](https://github.com/cpm-cmake/CPM.cmake) documentation.
Some advice:

- `CPM_SOURCE_CACHE` helps to avoid re-downloads with multiple userver build modes or multiple CPM-using projects;
- `CPM_USE_NAMED_CACHE_DIRECTORIES` (which userver enables by default) avoids junk library names shown in IDEs.

@anchor userver_cpm
### Downloading userver using CPM

userver itself can be downloaded and built using CPM.
In fact, this is what `download_userver()` function does in @ref service_templates "service templates" by default.

`download_userver()` just calls `CPMAddPackage` with some defaults, so you can pin userver `VERSION` or `GIT_TAG`
for reproducible builds.

When acquiring userver via CPM, you first need to install build dependencies. There are options:

* install @ref scripts/docs/en/userver/build/dependencies.md "build dependencies"
* or use base image of @ref docker_with_ubuntu_22_04

To use a @ref service_templates "service template" with CPM, run this script to get `userver-create-service` command:

@ref service-template/userver-create-service.sh

Make sure to @ref service_templates_presets "enable" the CMake options to build userver libraries you need,
then link to those libraries.

@see @ref cmake_options
@see @ref userver_libraries


@anchor userver_install
## Install

You can install userver globally and then use it from anywhere with `find_package`.
Make sure to use the same build mode as for your service, otherwise subtle linkage issues will arise.

@anchor userver_install_debian_package
### Building Debian package using Docker

For Ubuntu 22.04 or Ubuntu 24.04, to build `libuserver-all-dev.deb` package using Docker,
run the following shell command from directory where userver is located. For example, if userver is located in `~/userver` directory you need to run the script below in `~/` directory:

```shell
docker run --rm -it --network ip6net -v $(pwd):/home/user -w /home/user/userver \
   --entrypoint bash ghcr.io/userver-framework/ubuntu-22.04-userver-base:latest ./scripts/docker/run_as_user.sh \
   BUILD_OPTIONS="-DUSERVER_FEATURE_POSTGRESQL=1" ./scripts/build_and_install.sh
```

Make sure to:

1. replace the Docker image if appropriate;
2. pass the @ref cmake_options "cmake options" inside `BUILD_OPTIONS`, in particular...
3. enable the desired @ref userver_libraries "userver libraries";
4. pass the required options for @ref scripts/docs/en/userver/build/dependencies.md "build dependencies", if any.

Install the package with the following:

```shell
sudo dpkg -i ./libuserver-all-dev*.deb
```

### Building Debian package locally

This way suits all Debian-based systems.
To build `libuserver-all-dev.deb` package locally without Docker, first install build dependencies:

@see @ref scripts/docs/en/userver/build/dependencies.md

Then run the following shell command from userver directory:

```shell
BUILD_OPTIONS="-DUSERVER_FEATURE_POSTGRESQL=1" ./scripts/build_and_install.sh
```

Pass the @ref cmake_options "cmake options" inside `BUILD_OPTIONS`. Make sure to at least:

1. enable the desired @ref userver_libraries "userver libraries";
2. pass the required options for @ref scripts/docs/en/userver/build/dependencies.md "build dependencies", if any.

Install the package with the following:

```shell
sudo dpkg -i ./libuserver-all-dev*.deb
```

### Install with cmake --install

@warning Installing userver with cmake --install is NOT recommended due to update and uninstall issues.
For Debian-based systems, please @ref userver_install_debian_package "build and install Debian package" instead.

To install userver build it with `USERVER_INSTALL=ON` flags in `Debug` and `Release` modes:

```shell
cmake -S./ -B./build_debug \
    -DCMAKE_BUILD_TYPE=Debug \
    -DUSERVER_INSTALL=ON \
    -DUSERVER_SANITIZE="ub addr" \
    -GNinja
cmake -S./ -B./build_release \
    -DCMAKE_BUILD_TYPE=Release \
    -DUSERVER_INSTALL=ON \
    -GNinja
cmake --build build_debug/
cmake --build build_release/
cmake --install build_debug/
cmake --install build_release/
```

@see @ref cmake_options

@warning You should **never** delete the build directories when installing locally this way,
otherwise it will be difficult to update or uninstall userver.


### Use userver in your projects

Next, write

```cmake
find_package(userver REQUIRED COMPONENTS core postgresql grpc redis clickhouse mysql rabbitmq mongo)
```

in your `CMakeLists.txt`. Choose only the necessary components.

@see @ref userver_libraries

Finally, link your source with userver component.

For instance:

```cmake
target_link_libraries(${PROJECT_NAME}_objs PUBLIC userver::postgresql)
```

Done! You can use installed userver.

### Additional information:

Link `mariadbclient` additionally for mysql component:

```cmake
target_link_libraries(${PROJECT_NAME}-mysql_objs PUBLIC userver::mysql mariadbclient)
```


@anchor docker_with_ubuntu_22_04
## Docker with Ubuntu 22.04

The Docker images provide a container with all the build dependencies preinstalled and
with a proper setup of PPAs with databases, compilers and tools:

| Image reference                                              | Contains                               |
|--------------------------------------------------------------|----------------------------------------|
| `ghcr.io/userver-framework/ubuntu-22.04-userver-pg:latest`   | PostgreSQL and userver preinstalled    |
| `ghcr.io/userver-framework/ubuntu-22.04-userver:latest`      | userver preinstalled                   |
| `ghcr.io/userver-framework/ubuntu-22.04-userver-base:latest` | only userver dependencies preinstalled |

To run it just use a command like

```shell
docker run --rm -it --network ip6net --entrypoint bash ghcr.io/userver-framework/ubuntu-22.04-userver-pg:latest
```

After that, install the databases and compiler you are planning to use via
`apt install` and start developing.

To install userver in `ghcr.io/userver-framework/ubuntu-22.04-userver-base:latest` you should run the following command

```shell
sudo ./scripts/build_and_install_all.sh
```

Alternatively see @ref userver_install "userver install"

@note The above image is build from `scripts/docker/ubuntu-22.04-pg.dockerfile`,
   `scripts/docker/ubuntu-22.04.dockerfile`
   and `scripts/docker/base-ubuntu-22.04.dockerfile` respectively.
   See `scripts/docker/` directory and `scripts/docker/Readme.md` for more
   inspiration on building your own custom docker containers.


@anchor prebuilt_deb_package
### Using a prebuilt Debian package for Ubuntu 22.04

You can download and install a `.deb` package that is attached to each
[userver release](https://github.com/userver-framework/userver/releases).

The package currently targets Ubuntu 22.04, for other Ubuntu versions your mileage may vary.


@anchor userver_conan
## Conan

@note Conan must have version >= 2.8

Thanks to Open-Source community we have Conan support.

To build the userver Conan package run the following in the userver root directory:

```shell
conan profile new --detect default && conan profile update settings.compiler.libcxx=libstdc++11 default
conan create . --build=missing -pr:b=default
```

Make sure to pass flags corresponding to the desired userver libraries, e.g. `-o with_grpc=0`.

To use userver as a Conan package in your services add a `conanfile.txt` with the required version us the framework,
for example:
```
[requires]
userver/2.*
```

Run `conan install .` to actually install the required package. For more information see
[the official Conan documentation](https://docs.conan.io/2/tutorial/consuming_packages/build_simple_cmake_project.html).

Link with userver in your `CMakeLists.txt` as usual:
```cmake
target_link_libraries(${PROJECT_NAME} PUBLIC userver::grpc)
```

@see @ref userver_libraries


## Yandex Cloud with Ubuntu 22.04

The userver framework is
[available at Yandex Cloud Marketplace](https://yandex.cloud/en/marketplace/products/yc/userver).

To create a VM with preinstalled userver just click the "Create VM" button and
pay for the Cloud hardware usage.

After that the VM is ready to use. SSH to it and use
`find_package(userver REQUIRED)` in the `CMakeLists.txt` to use the preinstalled
userver framework.

You can start with @ref service_templates "service template".

If there a need to update the userver in the VM do the following:

```bash
sudo apt remove libuserver-*

cd /app/userver
sudo git checkout develop
sudo git pull
sudo ./scripts/build_and_install_all.sh
```

## PGO (clang)

PGO compilation consists of 2 compilation stages: profile collecting and compilation with PGO.
You can use PGO compilation doing the following steps:

1) configure userver AND your service with cmake option `-DUSERVER_PGO_GENERATE=ON`, compile the service;
2) run the resulting binary under the production-like workload to collect profile;
3) run llvm-profdata to convert profraw profile data format into profdata:
   ```sh
   llvm-profdata merge -output=code.profdata default.profraw
   ```
4) configure userver AND your service with cmake option `-DUSERVER_PGO_USE=<path_to_profdata>`, compile the service.

The resulting binary should be 2-15% faster than without PGO, depending on the code and workload.

@see @ref cmake_options

----------

@htmlonly <div class="bottom-nav"> @endhtmlonly
⇦ @ref scripts/docs/en/userver/faq.md | @ref scripts/docs/en/userver/build/dependencies.md ⇨
@htmlonly </div> @endhtmlonly

@example service-template/CMakeUserPresets.json.example
@example service-template/userver-create-service.sh
