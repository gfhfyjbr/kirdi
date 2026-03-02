cmake_minimum_required(VERSION 3.20)
project(kirdi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ──────────────────────────────────────────────────────────────────
option(KIRDI_BUILD_TESTS   "Build unit tests"            ON)
option(KIRDI_BUILD_SERVER  "Build server binary"          ON)
option(KIRDI_BUILD_CLIENT  "Build client binary"          ON)
option(KIRDI_BUILD_GUI     "Build GUI binary (webview)"   OFF)
option(KIRDI_BUILD_LIB     "Build libkirdi static library" ON)

# ── Dependencies ─────────────────────────────────────────────────────────────
find_package(Boost 1.81 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# nlohmann/json for config parsing (header-only, fetched if not found)
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# webview for GUI (cross-platform native webview)
if(KIRDI_BUILD_GUI)
    set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        webview
        GIT_REPOSITORY https://github.com/webview/webview.git
        GIT_TAG        0.12.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(webview)
endif()

# ── Common library (shared between server and client) ────────────────────────
add_library(kirdi_common STATIC
    src/common/protocol.cpp
    src/common/logger.cpp
    src/common/config.cpp
    src/common/crypto.cpp
)
target_include_directories(kirdi_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(kirdi_common PUBLIC
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    nlohmann_json::nlohmann_json
)

# ── TUN library (platform-specific) ─────────────────────────────────────────
add_library(kirdi_tun STATIC
    src/tun/tun_interface.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(kirdi_tun PRIVATE src/tun/tun_linux.cpp)
    target_compile_definitions(kirdi_tun PUBLIC KIRDI_PLATFORM_LINUX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(kirdi_tun PRIVATE src/tun/tun_macos.cpp)
    target_compile_definitions(kirdi_tun PUBLIC KIRDI_PLATFORM_MACOS)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_sources(kirdi_tun PRIVATE src/tun/tun_windows.cpp)
    target_compile_definitions(kirdi_tun PUBLIC KIRDI_PLATFORM_WINDOWS)
endif()

target_include_directories(kirdi_tun PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(kirdi_tun PUBLIC kirdi_common)

# ── Transport library ────────────────────────────────────────────────────────
add_library(kirdi_transport STATIC
    src/transport/ws_transport.cpp
)
target_include_directories(kirdi_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(kirdi_transport PUBLIC kirdi_common)

# ── Server binary ────────────────────────────────────────────────────────────
if(KIRDI_BUILD_SERVER)
    add_executable(kirdi-server
        src/server/main.cpp
        src/server/server.cpp
        src/server/session.cpp
    )
    target_link_libraries(kirdi-server PRIVATE
        kirdi_common
        kirdi_tun
        kirdi_transport
    )
    install(TARGETS kirdi-server RUNTIME DESTINATION bin)
endif()

# ── Client binary ────────────────────────────────────────────────────────────
if(KIRDI_BUILD_CLIENT)
    add_executable(kirdi-client
        src/client/main.cpp
        src/client/client.cpp
    )
    target_link_libraries(kirdi-client PRIVATE
        kirdi_common
        kirdi_tun
        kirdi_transport
    )
    install(TARGETS kirdi-client RUNTIME DESTINATION bin)
endif()

# ── libkirdi — C API static library for FFI (Rust/Tauri, etc.) ───────────────
if(KIRDI_BUILD_LIB)
    add_library(kirdi STATIC
        src/libkirdi/kirdi.cpp
        src/client/client.cpp
    )
    target_include_directories(kirdi PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(kirdi PUBLIC
        kirdi_common
        kirdi_tun
        kirdi_transport
    )
    install(TARGETS kirdi ARCHIVE DESTINATION lib)
    install(FILES include/kirdi/kirdi.h DESTINATION include/kirdi)
endif()

# ── GUI binary (webview + React) ─────────────────────────────────────────────
if(KIRDI_BUILD_GUI)
    # Enable Obj-C++ for .mm files on macOS
    enable_language(OBJCXX)

    # Build React frontend first
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gui/dist/index.html
        COMMAND npm run build
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/gui
        COMMENT "Building React GUI (npm run build)"
    )
    add_custom_target(gui_frontend DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gui/dist/index.html)

    # ── Layer 1: webview static lib (C++17, no kirdi headers) ────────────────
    add_library(kirdi_webview STATIC
        ${webview_SOURCE_DIR}/core/src/webview.cc
    )
    target_include_directories(kirdi_webview PUBLIC
        ${webview_SOURCE_DIR}/core/include
    )
    target_compile_definitions(kirdi_webview PUBLIC WEBVIEW_STATIC)
    set_target_properties(kirdi_webview PROPERTIES CXX_STANDARD 17)
    target_compile_options(kirdi_webview PRIVATE -Wno-deprecated-declarations)
    if(APPLE)
        target_link_libraries(kirdi_webview PUBLIC
            "-framework WebKit"
            "-framework Cocoa"
        )
    endif()

    # ── Layer 2: VPN bridge (C++23, kirdi internals, NO webview) ─────────────
    add_library(kirdi_gui_bridge STATIC
        src/gui/gui_bridge.cpp
        src/client/client.cpp
    )
    target_include_directories(kirdi_gui_bridge PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    set_target_properties(kirdi_gui_bridge PROPERTIES CXX_STANDARD 23)
    target_link_libraries(kirdi_gui_bridge PUBLIC
        kirdi_common
        kirdi_tun
        kirdi_transport
    )

    # ── Layer 3: GUI executable (C++17 .mm + webview + bridge) ───────────────
    add_executable(kirdi-gui
        src/gui/main.cpp
        src/gui/gui_controller.mm
    )
    add_dependencies(kirdi-gui gui_frontend)

    # main.cpp is C++17
    set_source_files_properties(src/gui/main.cpp PROPERTIES
        COMPILE_OPTIONS "-std=c++17"
    )
    # gui_controller.mm is Obj-C++17
    set_source_files_properties(src/gui/gui_controller.mm PROPERTIES
        COMPILE_FLAGS "-std=c++17 -ObjC++ -Wno-deprecated-declarations"
    )

    target_include_directories(kirdi-gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${webview_SOURCE_DIR}/core/include
    )
    target_compile_definitions(kirdi-gui PRIVATE WEBVIEW_STATIC)
    target_compile_options(kirdi-gui PRIVATE -Wno-deprecated-declarations)
    target_link_libraries(kirdi-gui PRIVATE
        kirdi_gui_bridge
        kirdi_webview
        nlohmann_json::nlohmann_json
    )
    install(TARGETS kirdi-gui RUNTIME DESTINATION bin)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(KIRDI_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(kirdi_tests
        tests/test_protocol.cpp
        tests/test_config.cpp
    )
    target_link_libraries(kirdi_tests PRIVATE
        kirdi_common
        kirdi_tun
        kirdi_transport
        GTest::gtest_main
    )
    include(GoogleTest)
    gtest_discover_tests(kirdi_tests)
endif()
