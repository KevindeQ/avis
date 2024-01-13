cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

if(NOT DEFINED AVIS_VERSION_MAJOR)
  set(AVIS_VERSION_MAJOR 1)
endif()
if(NOT DEFINED AVIS_VERSION_MINOR)
  set(AVIS_VERSION_MINOR 0)
endif()
if(NOT DEFINED AVIS_VERSION_PATCH)
  set(AVIS_VERSION_PATCH 0)
endif()

if (NOT PACKAGE_VERSION)
  set(PACKAGE_VERSION
    "${AVIS_VERSION_MAJOR}.${AVIS_VERSION_MINOR}.${AVIS_VERSION_PATCH}")
endif()

project(AVIS
  VERSION ${AVIS_VERSION_MAJOR}.${AVIS_VERSION_MINOR}.${AVIS_VERSION_PATCH}
  LANGUAGES C CXX ASM)

  # This C++ standard is required to build Avis
set(AVIS_REQUIRED_CXX_STANDARD 20)

# If the cache contains CMAKE_CXX_STANDARD it means that it's a old CMakeCache.txt and we can just inform
# the user and then reset it.
if($CACHE{CMAKE_CXX_STANDARD} AND $CACHE{CMAKE_CXX_STANDARD} LESS ${AVIS_REQUIRED_CXX_STANDARD})
  message(WARNING "Resetting cache value for CMAKE_CXX_STANDARD to ${AVIS_REQUIRED_CXX_STANDARD}")
  unset(CMAKE_CXX_STANDARD CACHE)
endif()

# If CMAKE_CXX_STANDARD is still set after the cache unset above it means that the user requested it
# and we allow it to be set to something newer than the required standard because otherwise the generation fails.
if(DEFINED CMAKE_CXX_STANDARD AND CMAKE_CXX_STANDARD LESS ${AVIS_REQUIRED_CXX_STANDARD})
  message(FATAL_ERROR "Requested CMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD} which is less than the required ${AVIS_REQUIRED_CXX_STANDARD}.")
endif()

set(CMAKE_CXX_STANDARD ${AVIS_REQUIRED_CXX_STANDARD} CACHE STRING "C++ standard to conform to")
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(FATAL_ERROR "
No build type selected. You need to pass -DCMAKE_BUILD_TYPE=<type> in order to configure Avis.
Available options are:
  * -DCMAKE_BUILD_TYPE=Release - For an optimized build with no assertions or debug info.
  * -DCMAKE_BUILD_TYPE=Debug - For an unoptimized build with assertions and debug info.
  * -DCMAKE_BUILD_TYPE=RelWithDebInfo - For an optimized build with no assertions but with debug info.
  * -DCMAKE_BUILD_TYPE=MinSizeRel - For a build optimized for size instead of speed.
")
endif()

# Sanity check the source directory to make sure that we are not trying to generate an in-source
# build (unless on MSVC_IDE, where it is ok), and to make sure that we don't have any stray
# generated files lying around in the tree (which would end up getting picked up by header
# search, instead of the correct versions).
if( CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR AND NOT MSVC_IDE )
  message(FATAL_ERROR "In-source builds are not allowed.
Please create a directory and run cmake from there, passing the path
to this source directory as the last argument.
This process created the file `CMakeCache.txt' and the directory `CMakeFiles'.
Please delete them.")
endif()

string(TOUPPER "${CMAKE_BUILD_TYPE}" uppercase_CMAKE_BUILD_TYPE)

if (CMAKE_BUILD_TYPE AND
    NOT uppercase_CMAKE_BUILD_TYPE MATCHES "^(DEBUG|RELEASE|RELWITHDEBINFO|MINSIZEREL)$")
  message(WARNING "Unknown value for CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
endif()

set(AVIS_SRC_DIR     ${CMAKE_CURRENT_SOURCE_DIR})
set(AVIS_LIB_DIR     ${AVIS_SRC_DIR}/lib)
set(AVIS_INCLUDE_DIR ${AVIS_SRC_DIR}/include)
set(AVIS_ASSETS_DIR    ${AVIS_SRC_DIR}/assets)

# Include additional support CMake files
include(${AVIS_SRC_DIR}/cmake/ShaderSupport.cmake)

# Define options to control the inclusion and default build behavior for components which may
# not strictly be necessary (tools, tests).
#
# This is primarily to support building smaller or faster project files.
option(AVIS_INCLUDE_APPS "Generate build targets for the Avis applications." ON)
option(AVIS_BUILD_APPS
  "Build the Avis applications. If OFF, just generate build targets." ON)

option(AVIS_BUILD_TESTS
  "Build Avis unit tests. If OFF, just generate build targets." OFF)
option(AVIS_INCLUDE_TESTS "Generate build targets for the Avis unit tests." ON)

Include(FetchContent)

add_subdirectory(extern)
add_subdirectory(lib)

if(AVIS_INCLUDE_APPS)
  add_subdirectory(apps)
endif()

if(AVIS_INCLUDE_TESTS)
  add_subdirectory(${AVIS_SRC_DIR}/tests)
endif()