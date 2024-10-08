# ExpressCpp

Fast, unopinionated, minimalist web framework for C++
Perfect for building REST APIs

![Logo of ExpressCpp](./doc/logo_expresscpp.png)

[![Conan](https://api.bintray.com/packages/expresscpp/expresscpp/expresscpp%3Aexpresscpp/images/download.svg)](https://bintray.com/expresscpp/expresscpp/expresscpp%3Aexpresscpp/_latestVersion)
[![pipeline status](https://gitlab.com/expresscpp/expresscpp/badges/master/pipeline.svg)](https://gitlab.com/expresscpp/expresscpp/commits/master)
[![expresscpp_http](https://gitlab.com/expresscpp/expresscpp/badges/master/coverage.svg?job=test:linux:gcc9)](https://gitlab.com/expresscpp/expresscpp/commits/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![c++17](https://img.shields.io/badge/C++-17-blue.svg?style=flat&logo=c%2B%2B)](https://en.wikipedia.org/wiki/C%2B%2B17)

## Design goals

ExpressCpp aims to be for C++ the same as express for Node.JS including its ecosystem of middlewares and extensions.

Express and Node.JS:

```js
const express = require('express');
const app = express();
app.get('/', (req, res) => res.send('Hello World!'));
const port = 3000;
app.listen(port, () => console.log(`Listening on port ${port}!`));
```

ExpressCpp:

```cpp
#include "expresscpp/expresscpp.hpp"
int main() {
  auto expresscpp = std::make_shared<expresscpp::ExpressCpp>();
  expresscpp->Get("/", [](auto /*req*/, auto res) { res->Send("hello world!"); });
  constexpr uint16_t port = 3000u;
  expresscpp->Listen(port,[=](auto /*ec*/) { std::cout << "Listening on port " << port << std::endl; }).Run();
  return 0;
}
```
## Development Goals

This is being turned into a learning tool to provide a non-trivial boost + cmake + conan2 example.  Nearly all of the conan provided example repos are toy level in complexity and trivial.  They don't ever address a real project example.  Most of the boost examples I found were between 4-10 years old and were on old conan 1.x, using a conanfile.txt, or were of toy level complexity.   Since boost is the biggest pain in the *** to compile and configure, it is the acid test for cross compiling and portability. It's just headers right?  No, it is more and its build is non-standard.  So my goal was to take a fork of espresscpp (which is interesting on its own, though unmaintained) and make it work in modern 2024 era.  There  is some work to cleanup the conanfile.py.  Some of the dependencies might be super old in the cpp side.  But it appears to compile for raspi5 if you follow the cross compiler steps below.


## Using me

### conan

```bash
conan remote add expresscpp https://api.bintray.com/conan/expresscpp/expresscpp/
```

add this to you conan file:

```txt
expresscpp/0.11.0@expresscpp/testing
```

this to your cmake:

```cmake
find_package(expresscpp)
# ...
target_link_libraries(my_target PRIVATE expresscpp::expresscpp)
```

### vendoring as subdirectory

```cmake
add_subdirectory(thirdparty/expresscpp)
# ...
target_link_libraries(my_target PRIVATE expresscpp::expresscpp)
```

### installing and using find_package

```cmake
git clone https://gitlab.com/expresscpp/expresscpp.git
cd expresscpp
mkdir build
cd build
cmake ..
make -j
sudo make install

find_package(expresscpp)
# ...
target_link_libraries(my_target PRIVATE expresscpp::expresscpp)
```

## Build instructions (e.g. ubuntu)

### Dependencies

- boost[asio, beast, uuid]
- nlohmann/json
- libfmt
- gtest (optional)

#### Conan

```bash
sudo apt install -y cmake gcc-9 g++-9 python3-pip

# conan for dependency management: use conan 2.0
sudo pip3 install conan --upgrade

mkdir -p build
cd build
cmake .. -DEXPRESSCPP_USE_CONAN_DEPENDENCIES=ON
cmake --build . -j
```

You may need to configure your conan default the first time:

conan profile detect

To build with conan as part of the cmake build, you must use the -DEXPRESSCPP_USE_CONAN_DEPENDENCIES=ON.  This is a project specific flag.  It is not a standard cmake flag.  It's actually very clever what it will do.  Instead of requiring an explicit conan command, it runs it from cmake.  And why not.  This works extremely well for most cases on standard platforms: Linux Windows Mac.

#### Cross Compiling

If you need to cross compile, follow these steps.

- First review the docs for CMake Toolchain File: https://cmake.org/cmake/help/latest/variable/CMAKE_TOOLCHAIN_FILE.html , and conan cross compiling: https://docs.conan.io/2/tutorial/consuming_packages/cross_building_with_conan.html

If you don't read those or loosely try to understand those concepts, do not continue.  If you are a pro at autoconf, this process is somewhat similar, however, complicated by years and layers of conan1.x and conan2.x docs interleaved across search results and stack overflow.  Don't muddy your approach by searching for answers.  The above two documents should cover it.

Your first step is to create a conan profile for your target platform.  Let's assume a Raspi5 for this example.  It's slightly non-standard, since the 64bit ARM (AARCH64) is somewhat new and in the past people have lazily still been running 32bit mode raspian releases.  As such, you won't find a bunch of good examples for a Raspi with a 64bit OS target.  So this is a great way to prove that it works, and you won't be tempted to look for answers.

Two approaches for this first step: either create a .conan2/profile file manually, or create one on your target, and copy the contents over (this may work with modifications).  Minimally you will need to know a bit about the processor and compiler specs.  

Create a profile for raspi5 in ~/.conan2/profiles/raspiarm64:

```
[settings]
os=Linux
arch=armv8
compiler=gcc
build_type=Release
compiler.cppstd=gnu17
compiler.libcxx=libstdc++11
compiler.version=11.4
[buildenv]
CC=aarch64-linux-gnu-gcc
CXX=aarch64-linux-gnu-g++
LD=aarch64-linux-gnu-ld
```
If you have challenges with the above, go back and review the conan website for profiles and creating profiles.

Assumptions:
a) you know your processor and variant (this can get complicated for armv5-7 however, we are on armv8): armv8
b) you have a cross compiler installed.  For the raspi you should just use the 64bit compiler from arm that runs on a linux x86_64 host.  You can grab that from the arm website, or on Ubuntu, install: sudo apt-get install g++-aarch64-linux-gnu gcc-aarch64-linux-gnu
c) you have a relatively modern version of cmake.  3.22 should be fine.  On linux, install from a script found at the cmake website.  Ubuntu packages will be out of date.
d) Release mode is ok.  If you need debug mode, go review the changes needed to build for debug targets.

The steps:

```
conan install .. --build=missing --profile:build default --profile:host raspiarm64
source ./Release/generators/conanbuild.sh
cmake .. -DCMAKE_TOOLCHAIN_FILE=Release/generators/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
make
```

Note: the most likely problem encountered is a profile issue, with a profile not existing.  Fix that first.
Next likely error is you don't have the right toolchain in the path.  aarch64 gcc compilers need to be in the path.  Fix that.  Then you might encounter a problem durring the conan command.  A typical one is some compiler settings mismatch.  If your conan is quite old, you might see it complains about an unknown gcc version.  This can be fixed manually by editing ~/.conan2/settings.yaml.  Another issue is lack of internet access will stop you, since conan dependencies are remote.  If that is ok, then there might be a problem compiling the dependencies.  Note how we used a --build=missing parameter.  This means it will build anything it can't find precompiled in the supported releases in conan.io.  This is wonderful because it means whatever the cross compile target platform, it should try to compile for you.  

After conan issues are resolved, you should see no errors, and all green results.  Red is an error.  You **must** have a conan_toolchain.cmake generated in the subdirectories.    If conanbuild.sh doesn't exist, it probably means you are on conan1.6 or earlier.  Update your conan.

For the final step, run cmake.  This will fail if it detects an old cmake.  That's fine, update your cmake.  If anything else was previously wrong, it will probably have resulted in an error and so will this.  All problems resolved, means cmake finishes without errors and a nice clean output:

```
-- -------------------------------------------------------
-- PROJECT_VERSION:.....................0.31.0
-- EXPRESSCPP_USE_CONAN_DEPENDENCIES:...OFF
-- EXPRESSCPP_BUILD_TESTS:..............OFF
-- EXPRESSCPP_BUILD_EXAMPLES:...........OFF
-- CMAKE_VERSION:.......................3.22.6
-- CMAKE_C_COMPILER:....................
-- CMAKE_CXX_COMPILER:................../usr/bin/aarch64-linux-gnu-g++
-- CMAKE_BUILD_TYPE:....................Release
-- BUILD_SHARED_LIBS:...................OFF
-- CLANG_TIDY_EXE:......................
-- -------------------------------------------------------
```

If you see the above is still using /usr/bin/c++ and not a cross compiler, something has gone wrong.  Repeat your steps to see where you went wrong.  It will not fail you if you are meticulous in your steps.  

Finally run make.  If that fails, you can always double check your commands using make VERBOSE=1.  On thing is conan will not run forever without maintenance.  Package repositories can get stale, packages can get removed, dependencies might get stale, etc.  It's nature, and software code rot.  Welcome to github.

Welcome to cross compiling 101.  You have graduated. 

#### Cross compiling with a sysroot (YOCTO)

TODO: Add yocto instructions that use a sysroot

#### Debian

```bash
sudo apt install -y cmake gcc-9 g++-9

# get debian dependencies
sudo apt install -y libboost-all-dev nlohmann-json3-dev libfmt-dev libgtest-dev

mkdir -p build
cd build
cmake ..
cmake --build . -j
```

## Features/Examples

| name                 | file                                                                         |
|----------------------|------------------------------------------------------------------------------|
| url query params     | [./example/query_params.cpp](./example/query_params.cpp)                     |
| url params           | [./example/url_params.cpp](./example/url_params.cpp)                         |
| auth-like middleware | [./example/middleware_auth_like.cpp](./example/middleware_auth_like.cpp)     |
| log-like middleware  | [./example/middleware_logger_like.cpp](./example/middleware_logger_like.cpp) |
| error handler        | [./example/error_handler.cpp](./example/error_handler.cpp)                   |
| variadic middlewares | [./example/multiple_handlers.cpp](./example/multiple_handlers.cpp)           |
| subrouting           | [./example/router.cpp](./example/router.cpp)                                 |

## Official Middlewares

| name                       | file                                                     |
|----------------------------|----------------------------------------------------------|
| static file provider       | [./example/serve_static.cpp](./example/serve_static.cpp) |
| favicon provider(embedded) | [./example/favicon.cpp](./example/serve_favicon.cpp)     |

- expresscpp-logger -> TODO
- expresscpp-grpc-proxy -> TODO
- expresscpp-reverse-proxy -> TODO
- expresscpp-basic-auth -> TODO

## Similiar projects

| name                | repo                                         |
|---------------------|----------------------------------------------|
| BeastHttp           | https://github.com/0xdead4ead/BeastHttp/     |
| crow (unmaintained) | https://github.com/ipkn/crow                 |
| Simple-Web-Server   | https://gitlab.com/eidheim/Simple-Web-Server |
| restinio            | https://github.com/stiffstream/restinio      |
| served              | https://github.com/meltwater/served          |
