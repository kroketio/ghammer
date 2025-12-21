
# VMF import: optimised even in Debug (it is the startup stall).
set(VMF_IMPORT_SOURCES
        lib/vmfpp/vmfpp.cpp
        lib/vmfpp/structs/Node.cpp
        lib/vmfpp/detail/StringUtils.cpp
        engine/scene.cpp
        engine/mesh.cpp)
set_source_files_properties(${VMF_IMPORT_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")

file(GLOB ENGINE_SOURCE_FILES CONFIGURE_DEPENDS
    "*.h"
    "*.cpp"
    "engine/*.h"
    "engine/*.cpp"
    "vulkan/*.h"
    "vulkan/*.cpp"
    "vulkan/render/*.h"
    "vulkan/render/*.cpp"
    "vulkan/tools/*.h"
    "vulkan/tools/*.cpp"
    "prefs/*.h"
    "prefs/*.cpp"
    "project/*.h"
    "project/*.cpp"
    "net/*.h"
    "net/*.cpp"
    "ui/*.h"
    "ui/*.cpp"
    "lib/*.h"
    "lib/*.cpp"
    "lib/qt/*.h"
    "lib/qt/*.cpp"
    "lib/vmfpp/*.h"
    "lib/vmfpp/*.cpp"
    "lib/vmfpp/detail/*.h"
    "lib/vmfpp/detail/*.cpp"
    "lib/vmfpp/structs/*.h"
    "lib/vmfpp/structs/*.cpp"
)

qt6_add_resources(RESOURCES
        assets.qrc
        lib/qt/theme.qrc
)

qt_add_executable(client main.cpp
        ${ENGINE_SOURCE_FILES}
        ${RESOURCES}
)

# Blender / Godot icons at :/icons_<set>/. Set GHAMMER_ICON_DIR (a folder with icons_blender/ and icons_godot/)
# to read them from disk instead: much faster builds.
set(GHAMMER_ICON_DIR "" CACHE PATH "Read the Blender / Godot icons from this folder instead of embedding them")
if(GHAMMER_ICON_DIR)
    message(STATUS "Icons read from ${GHAMMER_ICON_DIR}")
    target_compile_definitions(client PRIVATE GHAMMER_ICON_DIR="${GHAMMER_ICON_DIR}")
    target_link_libraries(client PRIVATE Qt6::CorePrivate)
else()
    foreach(_set blender godot)
        set(_dir "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons_${_set}")
        file(GLOB _icons CONFIGURE_DEPENDS "${_dir}/*.svg" "${_dir}/*.png")
        qt_add_resources(client "icons_${_set}" PREFIX "/icons_${_set}" BASE "${_dir}" FILES ${_icons} BIG_RESOURCES)
    endforeach()
endif()

# Third-party license texts for Help › About, at :/licenses/<file>.
file(GLOB LICENSE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses/*.txt")
qt_add_resources(client "licenses"
        PREFIX "/licenses"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses"
        FILES ${LICENSE_FILES}
)

# Godot runtime addon (godot/addons/ghammer), bundled for the export.
file(GLOB_RECURSE GHAMMER_ADDON_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/godot/addons/ghammer/*")
qt_add_resources(client "ghammer_addon"
        PREFIX "/ghammer"
        BASE "${CMAKE_SOURCE_DIR}/godot"
        FILES ${GHAMMER_ADDON_FILES}
)

# Shaders, baked to :/shaders/<name>.<stage>.qsb.
qt_add_shaders(client "vk_shaders"
        GLSL
        "310es,430"
        NOHLSL
        NOMSL
        PREFIX
        "/"
        BASE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES
        "shaders/scene.vert"
        "shaders/scene.frag"
        "shaders/shadow.vert"
        "shaders/shadow.frag"
        "shaders/shadow_point.vert"
        "shaders/shadow_point.frag"
        "shaders/gbuffer.vert"
        "shaders/entity.vert"
        "shaders/entity.frag"
        "shaders/billboard.vert"
        "shaders/billboard.frag"
        "shaders/gbuffer.frag"
        "shaders/fsquad.vert"
        "shaders/sky.frag"
        "shaders/ssao.frag"
        "shaders/ssao_blur.frag"
        "shaders/ssao_apply.frag"
        "shaders/fxaa.frag"
        "shaders/wire.vert"
        "shaders/wire.frag"
        "shaders/seldepth.vert"
        "shaders/seldepth.frag"
        "shaders/text.vert"
        "shaders/text.frag"
        "shaders/grid.vert"
        "shaders/grid.frag"
        "shaders/gridwire.vert"
        "shaders/gridwire.frag"
        "shaders/gridtext.vert"
        "shaders/selbox.vert"
        "shaders/selbox.frag"
        "shaders/dash.frag"
        "shaders/marquee3d.vert"
        "shaders/marquee3d.frag"
        "shaders/cutwire.frag"
        "shaders/cutfill.frag"
        "shaders/collision.vert"
        "shaders/collision.frag"
        "shaders/ioline.vert"
        "shaders/ioline.frag"
        "shaders/gridwire_density.frag"
)


target_include_directories(client PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(client PUBLIC
        QCoro6::Network
        Qt6::Core
        Qt6::Widgets
        Qt6::Network
        Qt6::Gui
        Qt6::Quick
        Qt6::Qml
        Qt6::WebSockets
        Qt6::Concurrent
        Qt6::QuickWidgets
        Qt6::Svg
        shared
)
target_link_libraries(client PRIVATE box3d::box3d)

# Static Qt links only xcb: add Wayland and the input contexts.
get_target_property(_qt_core_type Qt6::Core TYPE)
if(_qt_core_type STREQUAL "STATIC_LIBRARY" AND UNIX AND NOT APPLE)
    find_package(Qt6 QUIET COMPONENTS WaylandClient)
    set(_extra_plugins)
    foreach(_p QWaylandIntegrationPlugin QWaylandXdgShellIntegrationPlugin QWaylandBradientDecorationPlugin
               QWaylandAdwaitaDecorationPlugin QComposePlatformInputContextPlugin QIbusPlatformInputContextPlugin)
        if(TARGET Qt6::${_p})
            list(APPEND _extra_plugins Qt6::${_p})
        endif()
    endforeach()
    qt_import_plugins(client INCLUDE ${_extra_plugins})
endif()
set_target_properties(client PROPERTIES
        WIN32_EXECUTABLE TRUE  # Windows: no console window
        OUTPUT_NAME ghammer_client
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    # XDG desktop entry and hicolor icons; the id matches QGuiApplication::desktopFileName (main.cpp).
    install(FILES assets/linux/io.kroket.ghammer.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/linux/icon-512.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
            RENAME io.kroket.ghammer.png)
    install(FILES assets/linux/icon-256.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
            RENAME io.kroket.ghammer.png)
endif()

qt_add_qml_module(client
        URI Main
        VERSION 1.0
        OUTPUT_DIRECTORY qml_main
        RESOURCE_PREFIX ""
        QML_FILES
        qml/QCheckBox.qml
        qml/Main.qml
        qml/ViewportMenuBarTextButton.qml
        qml/ViewportMenuBarToggleButton.qml
        qml/PanelChip.qml
        qml/VertexScaleControls.qml
        qml/LayoutDesigner.qml
        qml/PanelInputBlocker.qml
        qml/ViewportGrid2D.qml
        qml/ViewGizmo.qml
        qml/MaterialPreview3D.qml
        qml/NoticeBox.qml
        qml/PieMenu.qml
        qml/QuickMaterialStrip.qml
        qml/ProjectSplash.qml
)

if(DEBUG)
    message(STATUS "DEBUG BUILD")
    target_compile_definitions(client PRIVATE DEBUG=1)
endif()

target_compile_options(client PRIVATE -Wno-deprecated-declarations)

target_link_libraries(client PRIVATE
        Qt6::Widgets
        Qt6::GuiPrivate
        Qt6::QuickPrivate
)

# Geometry unit tests.
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
  add_executable(brush_tests tests/brush_tests.cpp engine/brush.cpp engine/mesh.cpp engine/gameobject.cpp
          engine/material.cpp engine/arch_gen.cpp engine/vertex_ops.cpp engine/layout_gen.cpp
          engine/face_groups.cpp engine/face_graph.cpp engine/face_select.cpp engine/brush_select.cpp engine/map_format.cpp
          engine/stair_gen.cpp engine/entity.cpp engine/entity_io_defs.cpp engine/io.cpp engine/io_sim.cpp
          engine/godot_io_export.cpp engine/material_overrides.cpp engine/orm_compose.cpp
          engine/godot_text.cpp engine/gltf_writer.cpp engine/godot_export.cpp engine/smooth_units.cpp engine/color_lut.cpp engine/color_grade.cpp
          engine/path_curve.cpp engine/brush_deform.cpp engine/modifiers.cpp engine/mod_array.cpp engine/mod_curve.cpp
          engine/collision_hulls.cpp engine/undo_stack.cpp engine/brush_bevel.cpp
          project/project.cpp project/project_config.cpp project/godot_project.cpp)
  target_include_directories(brush_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(brush_tests PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Test shared box3d::box3d)
  enable_testing()
  add_test(NAME brush_tests COMMAND brush_tests)
endif()

target_link_libraries(client PRIVATE assimp::assimp)
