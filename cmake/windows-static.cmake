# Initial cache for the static Windows build (ghammer_win64 image):
#   cmake -S /src -B /out/build-windows-amd64 -GNinja -C /src/cmake/windows-static.cmake

set(CMAKE_TOOLCHAIN_FILE /opt/ghammer/win/toolchain.cmake CACHE FILEPATH "")
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(CMAKE_PREFIX_PATH "/opt/ghammer/win/qt;/opt/ghammer/win/deps" CACHE STRING "")
set(QT_HOST_PATH /opt/ghammer/host-qt CACHE PATH "")
set(CMAKE_EXE_LINKER_FLAGS "-static -static-libgcc -static-libstdc++" CACHE STRING "")
# Qt's ODBC plugin ends with -Bdynamic: switch back to static for the C++ runtime.
set(CMAKE_C_STANDARD_LIBRARIES "-Wl,-Bstatic" CACHE STRING "")
set(CMAKE_CXX_STANDARD_LIBRARIES "-Wl,-Bstatic" CACHE STRING "")
set(CMAKE_C_FLAGS "-I/opt/ghammer/win/deps/include" CACHE STRING "")
set(CMAKE_CXX_FLAGS "-I/opt/ghammer/win/deps/include" CACHE STRING "")
set(ZLIB_USE_STATIC_LIBS ON CACHE BOOL "")
set(BUILD_SERVER ON CACHE BOOL "")
