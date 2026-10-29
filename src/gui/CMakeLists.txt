# SPDX-License-Identifier: GPL-3.0-or-later
#
# lanes-gui.exe: Qt Quick front-end. Qt is linked dynamically (LGPLv3); the
# build runs windeployqt so the program starts from the build folder.

qt_standard_project_setup(REQUIRES 6.8)

# Models, controller and painted items: a library, so lanes_gui_tests can use them too.
qt_add_library(lanes_gui_lib STATIC
    app_controller.cpp
    app_controller.hpp
    download_model.cpp
    download_model.hpp
    format.cpp
    format.hpp
    segment_bar.cpp
    segment_bar.hpp
    speed_graph.cpp
    speed_graph.hpp)
target_include_directories(lanes_gui_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(lanes_gui_lib PUBLIC lanes::core Qt6::Quick Qt6::Qml)
if(WIN32)
    target_sources(lanes_gui_lib PRIVATE win/shell.cpp win/shell.hpp)
    target_link_libraries(lanes_gui_lib PRIVATE dwmapi shell32 ole32 comctl32 user32)
endif()
lanes_set_compiler_options(lanes_gui_lib)
if(MSVC)
    # qmlcachegen's generated code makes Qt's inline headers report unreachable code (C4702).
    target_compile_options(lanes_gui_lib PRIVATE /wd4702)
endif()

# The license texts are embedded under their file names (About → Open-source licenses).
set_source_files_properties(${PROJECT_SOURCE_DIR}/LICENSE PROPERTIES QT_RESOURCE_ALIAS LICENSE)
set_source_files_properties(${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
    PROPERTIES QT_RESOURCE_ALIAS THIRD_PARTY_NOTICES.md)
qt_add_qml_module(lanes_gui_lib
    URI Lanes
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Lanes
    QML_FILES
        qml/Main.qml
        qml/AddDialog.qml
        qml/ProgressWindow.qml
        qml/OptionsWindow.qml
        qml/QueuesWindow.qml
        qml/DownloadRow.qml
        qml/Icon.qml
        qml/ToolButtonIcon.qml
        qml/MessageDialog.qml
        qml/SpeedLimitMenu.qml
    RESOURCES
        ${PROJECT_SOURCE_DIR}/LICENSE
        ${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
        resources/lanes.ico)

qt_add_executable(lanes_gui WIN32 main.cpp)
set_target_properties(lanes_gui PROPERTIES OUTPUT_NAME lanes-gui)
if(WIN32)
    # Version resource ("Lanes Download Manager") and the icon (also used by the tray).
    configure_file(lanes.rc.in ${CMAKE_CURRENT_BINARY_DIR}/lanes.rc @ONLY)
    target_sources(lanes_gui PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/lanes.rc)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/lanes.rc PROPERTIES
        OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/lanes.ico)
endif()
target_link_libraries(lanes_gui PRIVATE lanes_gui_lib lanes_gui_libplugin Qt6::QuickControls2)
lanes_set_compiler_options(lanes_gui)

# Qt DLLs, plugins and QML imports next to the executables (copied only when changed).
if(WIN32)
    get_target_property(qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(qt_bin ${qmake} DIRECTORY)
    add_custom_command(TARGET lanes_gui POST_BUILD
        COMMAND ${qt_bin}/windeployqt.exe --no-translations --no-system-d3d-compiler
                --no-opengl-sw --no-compiler-runtime --no-system-dxc-compiler --no-ffmpeg
                --qmldir ${CMAKE_CURRENT_SOURCE_DIR}/qml $<TARGET_FILE:lanes_gui>
                > ${CMAKE_CURRENT_BINARY_DIR}/windeployqt.log
        COMMENT "windeployqt lanes-gui"
        VERBATIM)
endif()
