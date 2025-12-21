set(CMAKE_CXX_STANDARD 20)

file(GLOB SOURCE_FILES
    "*.h"
    "*.cpp"
    "lib/*.h"
    "lib/*.cpp"
    "lib/stb/*.h"
    "lib/stb/*.cpp"
    "lib/tinyexr/*.h"
    "lib/tinyexr/*.hh"
    "lib/tinyexr/*.cpp"
    "models/*.h"
    "models/*.cpp"
)

# rcc names the init function after the file: the base name must be unique in the build.
qt_add_resources(GENERATED_RESOURCES shared_assets.qrc)
add_library(shared STATIC
    ${SOURCE_FILES}
    ${GENERATED_RESOURCES}
)

target_link_libraries(shared PUBLIC
    QCoro6::Network
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::WebSockets
    ZLIB::ZLIB
)

target_include_directories(shared PUBLIC
    ${CMAKE_SOURCE_DIR}/src/
)

