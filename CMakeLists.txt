cmake_minimum_required(VERSION 2.8.12)

set(CMAKE_C_COMPILER "clang")
set(CMAKE_CXX_COMPILER "clang++")
add_compile_options(
  -Wall
  -Wextra
  -Wstrict-aliasing
  -Wno-unused-parameter
  -Wno-unused-result
  -Wno-missing-field-initializers
  -Wno-sign-compare
  -Wnon-virtual-dtor
  -Wchar-subscripts
  -Wpointer-arith
  -Woverloaded-virtual
  -Wformat
  -Wformat-security
  -Werror=format-security
  -fstack-protector-all
  -fPIE
)
set(CXX_COMPILE_FLAGS "")

# Set non-C compile flags and whole-loading linker flags.
# osquery needs ALL symbols in the libraries it includes for relaxed ctors
# late-loading modules and SQLite introspection utilities.
if(APPLE)
  set(APPLE_MIN_ABI "10.9")
  set(CXX_COMPILE_FLAGS "${CXX_COMPILE_FLAGS} -std=c++11 -stdlib=libc++")
  set(CXX_COMPILE_FLAGS "${CXX_COMPILE_FLAGS} -mmacosx-version-min=${APPLE_MIN_ABI}")
  set(OS_WHOLELINK_PRE "-Wl,-force_load")
  set(OS_WHOLELINK_POST "")
  # Special compile flags for Objective-C++
  set(OBJCXX_COMPILE_FLAGS
    "-x objective-c++ -fobjc-arc -Wno-c++11-extensions -mmacosx-version-min=${APPLE_MIN_ABI}")
elseif(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
  set(FREEBSD TRUE)
  set(CXX_COMPILE_FLAGS "${CXX_COMPILE_FLAGS} -std=c++11 -stdlib=libc++")
  set(OS_WHOLELINK_PRE "")
  set(OS_WHOLELINK_POST "")
else()
  set(LINUX TRUE)
  # Do not use the shared linker flags for modules.
  set(CXX_COMPILE_FLAGS "${CXX_COMPILE_FLAGS} -std=c++11")
  set(OS_WHOLELINK_PRE "-Wl,-whole-archive")
  set(OS_WHOLELINK_POST "-Wl,-no-whole-archive")
endif()

# Use osquery language to set platform/os
execute_process(
  COMMAND "${CMAKE_SOURCE_DIR}/tools/provision.sh" get_platform
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE PLATFORM
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REPLACE "." "_" PLATFORM "${PLATFORM}")
string(TOUPPER "${PLATFORM}" PLATFORM)
list(GET PLATFORM 0 OSQUERY_BUILD_OS)
list(GET PLATFORM 1 OSQUERY_BUILD_DISTRO)

# RHEL6 uses a different gcc 4.9 runtime
if(${OSQUERY_BUILD_DISTRO} STREQUAL "RHEL6")
  set(GCC_RUNTIME "/opt/rh/devtoolset-3/root/usr/")
  message("-- Setting RHEL6 GCC runtime: ${GCC_RUNTIME}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --gcc-toolchain=${GCC_RUNTIME}")
endif()

# make debug (environment variable from Makefile)
if(DEFINED ENV{DEBUG})
  set(CMAKE_BUILD_TYPE "Debug")
  add_compile_options(-g -O0 -pg)
  add_definitions(-DDEBUG)
elseif(DEFINED ENV{SANITIZE})
  # make sanitize (cannot make debug sanitize)
  add_compile_options(-g -O0 -fno-omit-frame-pointer)
  add_definitions(-DNDEBUG)
  add_compile_options(-fsanitize=leak -fsanitize=address)
else()
  add_compile_options(-O2)
  add_definitions(-DNDEBUG)
  # Do not enable fortify with clang: http://llvm.org/bugs/show_bug.cgi?id=16821
  #set(C_COMPILE_FLAGS "${C_COMPILE_FLAGS} -D_FORTIFY_SOURCE=2")
endif()

# make analyze (environment variable from Makefile)
if(DEFINED ENV{ANALYZE})
  set(CMAKE_CXX_COMPILER "${CMAKE_SOURCE_DIR}/tools/analysis/clang-analyze.sh")
endif()

# Finished setting compiler/compiler flags.
project(OSQUERY)

# Make sure deps were built before compiling (else show warning)
execute_process(
  COMMAND "${CMAKE_SOURCE_DIR}/tools/provision.sh" check "${CMAKE_BINARY_DIR}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE OSQUERY_DEPS_MESSAGE
  RESULT_VARIABLE OSQUERY_DEPS_CHECK
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(ASCII 27 Esc)
if(OSQUERY_DEPS_CHECK)
  message(WARNING "${Esc}[31m${OSQUERY_DEPS_MESSAGE}${Esc}[m")
endif()

# Generate version from git
execute_process(
  COMMAND git describe --tags HEAD --always
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE OSQUERY_BUILD_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(DEFINED ENV{SDK_VERSION})
  set(OSQUERY_BUILD_SDK_VERSION "${ENV{SDK_VERSION}}")
else()
  string(REPLACE "-" ";" OSQUERY_BUILD_SDK_VERSION ${OSQUERY_BUILD_VERSION})
  list(GET OSQUERY_BUILD_SDK_VERSION 0 OSQUERY_BUILD_SDK_VERSION)
endif()

# make sdk (tests building SDK-based extensions)
if(DEFINED ENV{SDK})
  set(OSQUERY_BUILD_SDK_ONLY TRUE)
endif()

# Set various platform/platform-version/build version/etc defines.
add_definitions(
  -DOSQUERY_BUILD_VERSION=${OSQUERY_BUILD_VERSION}
  -DOSQUERY_BUILD_SDK_VERSION=${OSQUERY_BUILD_SDK_VERSION}
  -D${OSQUERY_BUILD_OS}
  -D${OSQUERY_BUILD_OS}_${OSQUERY_BUILD_DISTRO}
)

if(APPLE)
  message("-- Building for OS X")
elseif(OSQUERY_BUILD_OS STREQUAL "UBUNTU")
  set(UBUNTU TRUE)
  message("-- Building for Ubuntu")
elseif(OSQUERY_BUILD_OS STREQUAL "CENTOS")
  set(CENTOS TRUE)
  message("-- Building for CentOS")
elseif(OSQUERY_BUILD_OS STREQUAL "RHEL")
  set(RHEL TRUE)
  message("-- Building for RHEL")
elseif(FREEBSD)
  message("-- Building for FreeBSD")
endif()

# Make sure the generated paths exist
execute_process(COMMAND mkdir -p "${CMAKE_BINARY_DIR}/generated")

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake" "${CMAKE_MODULE_PATH}")
set(CMAKE_SKIP_INSTALL_ALL_DEPENDENCY TRUE)

# We need to link some packages as dynamic/dependent.
set(CMAKE_FIND_LIBRARY_SUFFIXES .dylib .so)
find_package(OpenSSL REQUIRED)
find_package(Crypto REQUIRED)
find_package(BZip2 REQUIRED)
find_package(Dl REQUIRED)
find_package(Readline REQUIRED)

# Most dependent packages/libs we want static.
set(CMAKE_FIND_LIBRARY_SUFFIXES .a .dylib .so)
find_package(CppNetlib 0.11.0 REQUIRED)
find_package(Gflags REQUIRED)
find_package(Glog REQUIRED)
find_package(Gtest REQUIRED)
find_package(RocksDB REQUIRED)
find_package(Snappy REQUIRED)
find_package(Sqlite3 REQUIRED)
find_package(Thrift 0.9.1 REQUIRED)

enable_testing()

include(CMakeLibs)
include(Packages)
include(Thrift)

include_directories("${GLOG_INCLUDE_DIRS}")
include_directories("${CMAKE_SOURCE_DIR}/include")
include_directories("${CMAKE_SOURCE_DIR}")
include_directories("/usr/local/include")
link_directories("/usr/local/lib")

add_subdirectory(osquery)
add_subdirectory(tools/tests)

# make docs
find_package(Doxygen)
if(DOXYGEN_FOUND)
  add_custom_target(
    docs
    echo "PROJECT_NUMBER=${OSQUERY_BUILD_VERSION}" |
      cat ${CMAKE_SOURCE_DIR}/docs/Doxyfile - |
      "${DOXYGEN_EXECUTABLE}" -
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Generating API documentation with Doxygen" VERBATIM
  )
endif(DOXYGEN_FOUND)

# make format-all
add_custom_target(
  format-all
  find osquery include tools \( -name "*.h" -o -name "*.cpp" -o -name "*.mm" \)
    -exec clang-format -i {} +
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Formatting all osquery code with clang-format" VERBATIM
)

# make format
add_custom_target(
  format
  python "${CMAKE_SOURCE_DIR}/tools/formatting/git-clang-format.py"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Formatting code staged code changes with clang-format" VERBATIM
)

# make sync
add_custom_target(
  sync
  "${CMAKE_SOURCE_DIR}/tools/sync.sh" "${CMAKE_BINARY_DIR}"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Generating sdk sync: ${CMAKE_BINARY_DIR}/sync"
)
