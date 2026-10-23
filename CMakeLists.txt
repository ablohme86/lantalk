cmake_minimum_required(VERSION 3.21)

project(LanTalk VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(LANTALK_BUILD_TESTS "Build the LANTalk unit tests" ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

# Everything except main.cpp, so the unit tests can link the same code.
add_library(lantalk_lib STATIC
    appconfig.h
    ollamaclient.cpp
    ollamaclient.h
    messagetranslator.cpp
    messagetranslator.h
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
    protocol/quotedmessage.cpp
    protocol/quotedmessage.h
    tui/terminalui.cpp
    tui/terminalui.h
    gui/appicon.cpp
    gui/appicon.h
    gui/dialogs.cpp
    gui/dialogs.h
    gui/chatinput.cpp
    gui/chatinput.h
    gui/chatlog.cpp
    gui/chatlog.h
    gui/chattab.cpp
    gui/chattab.h
    gui/mainwindow.cpp
    gui/mainwindow.h
)

target_include_directories(lantalk_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The version shown in the window title; scripts/bump-version.sh updates it.
target_compile_definitions(lantalk_lib PUBLIC LANTALK_VERSION="${PROJECT_VERSION}")

target_link_libraries(lantalk_lib PUBLIC Qt6::Widgets Qt6::Network)

qt_add_executable(lantalk
    main.cpp
)

target_link_libraries(lantalk PRIVATE lantalk_lib)

set_target_properties(lantalk PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

if(UNIX AND NOT APPLE)
    # The Linux release tarball ships the Qt libraries it deploys next to the
    # executable.
    include(GNUInstallDirs)
    set_target_properties(lantalk PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
endif()

if(WIN32)
    enable_language(RC)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/lantalk.ico" LANTALK_ICON_PATH)
    configure_file(resources/lantalk.rc.in ${CMAKE_CURRENT_BINARY_DIR}/lantalk.rc @ONLY)
    target_sources(lantalk PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/lantalk.rc)
endif()

install(TARGETS lantalk BUNDLE DESTINATION . RUNTIME DESTINATION bin)

qt_generate_deploy_app_script(
    TARGET lantalk
    OUTPUT_SCRIPT deploy_script
    NO_TRANSLATIONS
)
install(SCRIPT ${deploy_script})

if(LANTALK_BUILD_TESTS)
    find_package(Qt6 COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "Qt6 Test not found; skipping LANTalk unit tests")
    endif()
endif()
