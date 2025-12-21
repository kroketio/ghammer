cmake_minimum_required(VERSION 3.16)

# gHammer's version: writes ghammer_version.h
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GHAMMER_VERSION LIMIT_COUNT 1)
string(STRIP "${GHAMMER_VERSION}" GHAMMER_VERSION)
if(NOT GHAMMER_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must hold a version like 1.3.0, not '${GHAMMER_VERSION}'")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(ghammer
  VERSION "${GHAMMER_VERSION}"
  DESCRIPTION "gHammer: level editor for Godot"
  LANGUAGES CXX
)

include(GNUInstallDirs)

configure_file("${CMAKE_SOURCE_DIR}/cmake/ghammer_version.h.in" "${CMAKE_BINARY_DIR}/generated/ghammer_version.h" @ONLY)
include_directories("${CMAKE_BINARY_DIR}/generated")
message(STATUS "Initiating compile using CMake ${CMAKE_VERSION}")

list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING
        "Default build type: Release" FORCE)
endif()
add_link_options($<$<AND:$<CONFIG:Release>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-s>)

set(THREADS_PREFER_PTHREAD_FLAG ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
option(BUILD_SERVER "" ON)
option(BUILD_CLIENT "" OFF)
# Server-only build: no client, no Qt Quick / private Gui.
option(SERVER_ONLY "" OFF)
option(USE_VENDORED_LIBS "" ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

include(FindCcache)

find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(RapidJSON REQUIRED)

if(SERVER_ONLY)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent WebSockets Sql Network)
else()
  find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent WebSockets Sql QuickWidgets Gui GuiPrivate Quick QuickPrivate Qml ShaderTools Svg)
endif()

if(USE_VENDORED_LIBS)
  set(USE_QT_VERSION 6 CACHE STRING "" FORCE)
  set(QCORO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(QCORO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(QCORO_WITH_QTTEST OFF CACHE BOOL "" FORCE)
  set(QCORO_WITH_QTDBUS OFF CACHE BOOL "" FORCE)
  set(QCORO_WITH_QTQUICK OFF CACHE BOOL "" FORCE)
  set(QCORO_WITH_QML OFF CACHE BOOL "" FORCE)
  add_subdirectory(external/qcoro EXCLUDE_FROM_ALL)
else()
  find_package(QCoro6 REQUIRED COMPONENTS Network)
endif()

add_subdirectory(src/shared)
if(NOT SERVER_ONLY)
  if(USE_VENDORED_LIBS)
    # Box3D: physics.
    set(BOX3D_DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/box3d EXCLUDE_FROM_ALL)
    # assimp: model import, importers only.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
    set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
    set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ZLIB OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    # Bundled minizip: a static system libminizip drags in bzip2 and more.
    set(ASSIMP_BUILD_MINIZIP ON CACHE BOOL "" FORCE)
    unset(UNZIP_FOUND CACHE)
    add_subdirectory(external/assimp EXCLUDE_FROM_ALL)
  else()
    find_package(box3d CONFIG REQUIRED)
    find_package(assimp REQUIRED)
  endif()
  add_subdirectory(src/client)
endif()

if(BUILD_SERVER)
  add_subdirectory(src/server)
endif()

# The client starts ghammer_server from its own directory.
if(TARGET client AND TARGET server)
  add_dependencies(client server)
endif()
