cmake_minimum_required(VERSION 3.21)

project(LanChat VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(LANCHAT_BUILD_TESTS "Build the LANChat unit tests" ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

# Everything except main.cpp, so the unit tests can link the same code.
add_library(lanchat_lib STATIC
    appconfig.h
    ollamaclient.cpp
    ollamaclient.h
    translationrestorer.cpp
    translationrestorer.h
    protocol/chathistory.cpp
    protocol/chathistory.h
    protocol/chatsession.cpp
    protocol/chatsession.h
    protocol/connectionregistry.cpp
    protocol/connectionregistry.h
    protocol/conversationidentity.cpp
    protocol/conversationidentity.h
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

target_include_directories(lanchat_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(lanchat_lib PUBLIC Qt6::Widgets Qt6::Network)

qt_add_executable(lanchat
    main.cpp
)

target_link_libraries(lanchat PRIVATE lanchat_lib)

set_target_properties(lanchat PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

if(WIN32)
    enable_language(RC)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/lanchat.ico" LANCHAT_ICON_PATH)
    configure_file(resources/lanchat.rc.in ${CMAKE_CURRENT_BINARY_DIR}/lanchat.rc @ONLY)
    target_sources(lanchat PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/lanchat.rc)
endif()

install(TARGETS lanchat BUNDLE DESTINATION . RUNTIME DESTINATION bin)

qt_generate_deploy_app_script(
    TARGET lanchat
    OUTPUT_SCRIPT deploy_script
    NO_TRANSLATIONS
)
install(SCRIPT ${deploy_script})

if(LANCHAT_BUILD_TESTS)
    find_package(Qt6 COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "Qt6 Test not found; skipping LANChat unit tests")
    endif()
endif()
