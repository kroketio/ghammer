# Initial cache for the static Linux build (ghammer_amd64 image):
#   cmake -S /src -B /out/build-linux-amd64 -GNinja -C /src/cmake/linux-static.cmake
# glibc, libxcb and libwayland stay shared.

set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(CMAKE_C_COMPILER gcc-12 CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER g++-12 CACHE FILEPATH "")
set(CMAKE_PREFIX_PATH "/opt/ghammer/qt;/opt/ghammer/deps" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc" CACHE STRING "")
set(ZLIB_USE_STATIC_LIBS ON CACHE BOOL "")
set(BUILD_SERVER ON CACHE BOOL "")

# Static archives carry no dependencies: these go last on every link line.
set(_deps /opt/ghammer/deps/lib)
set(CMAKE_CXX_STANDARD_LIBRARIES
    "${_deps}/libfontconfig.a ${_deps}/libfreetype.a ${_deps}/libexpat.a ${_deps}/libpng16.a ${_deps}/libz.a -ldl -lpthread"
    CACHE STRING "")
