cmake_minimum_required(VERSION 3.21)

project(LanChat VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

qt_add_executable(lanchat
    main.cpp
    appconfig.h
    ollamaclient.cpp
    ollamaclient.h
    protocol/chatsession.cpp
    protocol/chatsession.h
    protocol/peerdiscovery.h
    tui/terminalui.cpp
    tui/terminalui.h
    gui/appicon.cpp
    gui/appicon.h
    gui/dialogs.cpp
    gui/dialogs.h
    gui/chattab.cpp
    gui/chattab.h
    gui/mainwindow.cpp
    gui/mainwindow.h
)

target_include_directories(lanchat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(lanchat PRIVATE Qt6::Widgets Qt6::Network)

set_target_properties(lanchat PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

install(TARGETS lanchat BUNDLE DESTINATION . RUNTIME DESTINATION bin)

qt_generate_deploy_app_script(
    TARGET lanchat
    OUTPUT_SCRIPT deploy_script
    NO_TRANSLATIONS
)
install(SCRIPT ${deploy_script})
