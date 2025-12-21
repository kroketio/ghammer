file(GLOB SERVER_SOURCE_FILES
    "*.h"
    "*.cpp"
    "lib/stb/*.h"
    "lib/stb/*.cpp"
    "web/*.h"
    "web/*.cpp"
    "web/routes/*.h"
    "web/routes/*.cpp"
    "web/http/*.h"
    "web/http/*.cpp"
    "catalog/*.h"
    "catalog/*.cpp"
)

add_executable(server main.cpp
    ${SERVER_SOURCE_FILES}
)

target_include_directories(server PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${RapidJSON_INCLUDE_DIRS})

target_link_libraries(server PUBLIC
    QCoro6::Network
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
    Qt6::Concurrent

    shared
)

# Static Qt: headless, no platform plugins.
get_target_property(_qt_core_type Qt6::Core TYPE)
if(_qt_core_type STREQUAL "STATIC_LIBRARY")
    qt_import_plugins(server EXCLUDE_BY_TYPE platforms platformthemes platforminputcontexts)
endif()

# Next to ghammer_client: the editor starts its local server from its own directory.
install(TARGETS server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set_target_properties(server PROPERTIES
        OUTPUT_NAME ghammer_server
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# End-to-end tests against the real server binary; not in the default build.
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
  add_executable(server_tests EXCLUDE_FROM_ALL tests/realtime_tests.cpp)
  target_link_libraries(server_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::WebSockets Qt6::Test ZLIB::ZLIB)
  target_compile_definitions(server_tests PRIVATE SERVER_BINARY="$<TARGET_FILE:server>")
  add_dependencies(server_tests server)
endif()
