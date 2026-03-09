cmake_minimum_required(VERSION 3.20)
project(kirdi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ──────────────────────────────────────────────────────────────────
option(KIRDI_BUILD_TESTS   "Build unit tests"          ON)
option(KIRDI_BUILD_SERVER  "Build server binary"        ON)
option(KIRDI_BUILD_CLIENT  "Build client binary"        ON)

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

# Windows: link Winsock and IP helper for networking + route APIs
if(WIN32)
    target_link_libraries(kirdi_common PUBLIC ws2_32 iphlpapi)
endif()

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

    # ── Integration tests (requires root + Linux network namespaces) ─────
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(
            NAME integration_tunnel
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_tunnel.sh
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(integration_tunnel PROPERTIES
            LABELS "integration"
            TIMEOUT 180
            ENVIRONMENT "BUILD_DIR=${CMAKE_BINARY_DIR}"
        )
    endif()
endif()

# ── Convenience targets ──────────────────────────────────────────────────────
if(KIRDI_BUILD_TESTS)
    # Run unit tests only
    add_custom_target(unit_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L unit
        DEPENDS kirdi_tests
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running unit tests"
    )

    # Run integration tests only (requires root)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_custom_target(integration_tests
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_tunnel.sh
            DEPENDS kirdi-server kirdi-client
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Running integration tests (requires root)"
        )
    endif()

    # Run all tests
    add_custom_target(test_all
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS kirdi_tests
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running all tests"
    )
endif()
