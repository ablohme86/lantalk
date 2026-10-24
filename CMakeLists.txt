cmake_minimum_required(VERSION 3.21)

project(LanTalk VERSION 1.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(LANTALK_BUILD_TESTS "Build the LANTalk unit tests" ON)
# OFF builds only lantalkd, so a server needs no Qt Widgets.
option(LANTALK_BUILD_CLIENT "Build the LANTalk client" ON)

if(LANTALK_BUILD_CLIENT)
    find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Network)
endif()

# The central relay service runs headless on a server, so it needs only
# QtCore and QtNetwork. It shares the Ollama translation code with the client.
add_library(lantalk_central_lib STATIC
    appconfig.h
    messagetranslator.cpp
    messagetranslator.h
    ollamaclient.cpp
    ollamaclient.h
    ollamadiscovery.cpp
    ollamadiscovery.h
    protocol/peerdiscovery.h
    protocol/quotedmessage.cpp
    protocol/quotedmessage.h
    central/centrallink.cpp
    central/centrallink.h
    central/centralserver.cpp
    central/centralserver.h
    central/translationservice.cpp
    central/translationservice.h
    central/webserver.cpp
    central/webserver.h
    protocol/centralprotocol.h
    protocol/conversationidentity.cpp
    protocol/conversationidentity.h
)
target_include_directories(lantalk_central_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lantalk_central_lib PUBLIC LANTALK_VERSION="${PROJECT_VERSION}")
target_link_libraries(lantalk_central_lib PUBLIC Qt6::Network)
# The web client lantalkd serves to browsers.
qt_add_resources(lantalk_central_lib lantalk_web
    PREFIX /web
    BASE central/web
    FILES
        central/web/index.html
        central/web/app.css
        central/web/app.js
        central/web/manifest.webmanifest
        central/web/icon.svg
)

if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
endif()

qt_add_executable(lantalkd
    central/main.cpp
)
target_link_libraries(lantalkd PRIVATE lantalk_central_lib)
set_target_properties(lantalkd PROPERTIES MACOSX_BUNDLE FALSE)
if(UNIX AND NOT APPLE)
    set_target_properties(lantalkd PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
endif()

install(TARGETS lantalkd RUNTIME DESTINATION bin)

if(LANTALK_BUILD_CLIENT)
    # Everything except main.cpp, so the unit tests can link the same code.
    add_library(lantalk_lib STATIC
        appconfig.h
        translationrestorer.cpp
        translationrestorer.h
        protocol/chathistory.cpp
        protocol/chathistory.h
        protocol/chatsession.cpp
        protocol/chatsession.h
        protocol/chattransport.cpp
        protocol/chattransport.h
        protocol/centralclient.cpp
        protocol/centralclient.h
        protocol/centralprotocol.h
        protocol/connectionregistry.cpp
        protocol/connectionregistry.h
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
        gui/uilanguage.cpp
        gui/uilanguage.h
    )

    target_include_directories(lantalk_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    # The version shown in the window title; scripts/bump-version.sh updates it.
    target_compile_definitions(lantalk_lib PUBLIC LANTALK_VERSION="${PROJECT_VERSION}")

    target_link_libraries(lantalk_lib PUBLIC lantalk_central_lib Qt6::Widgets Qt6::Network)

    # The new-message chime plays through Qt Multimedia when it is installed;
    # without it LANTalk falls back to the system beep.
    find_package(Qt6 QUIET COMPONENTS Multimedia)
    if(Qt6Multimedia_FOUND)
        target_link_libraries(lantalk_lib PUBLIC Qt6::Multimedia)
        target_compile_definitions(lantalk_lib PUBLIC LANTALK_HAVE_MULTIMEDIA)
    else()
        message(STATUS "Qt6 Multimedia not found; new messages use the system beep")
    endif()
    qt_add_resources(lantalk_lib lantalk_sounds
        PREFIX /sounds
        BASE resources
        FILES resources/message.wav
    )

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
endif()
