cmake_minimum_required(VERSION 3.16)
project(obs-graphics-engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS ON)

option(ENABLE_LUA_SCRIPTING "Build Lua/sol2 scripting support (ScriptDataSource)" ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)

if(ENABLE_LUA_SCRIPTING)
    # Lua: fetch official source and define a library target
    CPMAddPackage(
        NAME lua
        GITHUB_REPOSITORY lua/lua
        GIT_TAG v5.4.6
        DOWNLOAD_ONLY YES
    )
    if(lua_ADDED)
        FILE(GLOB lua_sources ${lua_SOURCE_DIR}/*.c)
        list(REMOVE_ITEM lua_sources "${lua_SOURCE_DIR}/lua.c" "${lua_SOURCE_DIR}/luac.c")
        add_library(lua STATIC ${lua_sources})

        target_include_directories(lua
            PUBLIC
                $<BUILD_INTERFACE:${lua_SOURCE_DIR}>
        )
        set_target_properties(lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()

    CPMAddPackage(
        NAME sol2
        GITHUB_REPOSITORY ThePhD/sol2
        GIT_TAG v3.5.0
        DOWNLOAD_ONLY YES
    )

    if (sol2_ADDED)
      add_library(sol2 INTERFACE IMPORTED)
      target_include_directories(sol2 INTERFACE ${sol2_SOURCE_DIR}/include)
      target_link_libraries(sol2 INTERFACE lua)
    endif()

    find_package(Threads REQUIRED)

    # mbedTLS: TLS backend for IXWebSocket's HttpClient, source-fetched via
    # CPM (same pattern as lua/sol2 above) so no system SSL package is needed
    # on any platform.
    #
    # BUILD_SHARED_LIBS OFF is required for the same reason as ixwebsocket's
    # below: mbedtls's vendored 3rdparty helper libs (everest, p256-m) are
    # plain add_library() calls with no Windows dllexport annotations, so left
    # to inherit the project-wide `BUILD_SHARED_LIBS ON` they build as DLLs
    # with zero exported symbols — MSVC then emits no import .lib for them,
    # and the link against e.g. everest.lib fails with LNK1181 on Windows.
    CPMAddPackage(
        NAME mbedtls
        GITHUB_REPOSITORY Mbed-TLS/mbedtls
        GIT_TAG v3.6.2
        OPTIONS
            "ENABLE_PROGRAMS OFF"
            "ENABLE_TESTING OFF"
            "BUILD_SHARED_LIBS OFF"
    )

    # IXWebSocket: only HttpClient is used today; WebSocket support is unused
    # but comes along for a possible future feature (see CLAUDE.md).
    #
    # BUILD_SHARED_LIBS is forced OFF here (scoped to this add_subdirectory
    # call only, via CPM's OPTIONS mechanism) because it would otherwise
    # inherit the project-wide `set(BUILD_SHARED_LIBS ON)` above and build
    # ixwebsocket as a .so — breaking static linkage into `engine`.
    CPMAddPackage(
        NAME ixwebsocket
        GITHUB_REPOSITORY machinezone/IXWebSocket
        GIT_TAG v11.4.6
        OPTIONS
            "USE_TLS ON"
            "USE_MBED_TLS ON"
            "BUILD_SHARED_LIBS OFF"
            "IXWEBSOCKET_INSTALL OFF"
    )

    # IXWebSocket's own CMakeLists resolves mbedTLS via `find_package(MbedTLS
    # REQUIRED)` and its bundled CMake/FindMbedTLS.cmake module, expecting
    # MBEDTLS_INCLUDE_DIRS/MBEDTLS_LIBRARIES to get populated by that module.
    # Since CMake >= 3.24 transparently redirects find_package(MbedTLS) to the
    # CPM-fetched `mbedtls` package above (bypassing the Module entirely), that
    # find_package call succeeds without ever setting those variables, leaving
    # `ixwebsocket` with no mbedtls include path or libraries at all. Wire them
    # up explicitly so the target actually builds against our fetched mbedtls.
    if (TARGET ixwebsocket)
        target_include_directories(ixwebsocket PRIVATE ${mbedtls_SOURCE_DIR}/include)
        target_link_libraries(ixwebsocket PRIVATE mbedtls mbedx509 mbedcrypto)
        # mbedtls 3.x header layout; IXSocketMbedTLS.cpp guards on this macro.
        target_compile_definitions(ixwebsocket PRIVATE IXWEBSOCKET_USE_MBED_TLS_MIN_VERSION_3)
    endif()

    foreach(pic_target ixwebsocket mbedtls mbedcrypto mbedx509 everest p256m)
        if(TARGET ${pic_target})
            set_target_properties(${pic_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        endif()
    endforeach()
endif()

CPMAddPackage(
  NAME nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip
  DOWNLOAD_ONLY YES
)

if (nlohmann_json_ADDED)
  add_library(nlohmann_json INTERFACE IMPORTED)
  target_include_directories(nlohmann_json INTERFACE ${nlohmann_json_SOURCE_DIR}/include)
endif()

CPMAddPackage(
    NAME stb
    GITHUB_REPOSITORY nothings/stb
    GIT_TAG master
    DOWNLOAD_ONLY YES
)

CPMAddPackage(
    NAME cpp_zipper
    GITHUB_REPOSITORY yhirose/cpp-zipper
    GIT_TAG master
    DOWNLOAD_ONLY YES
)

set(engine_sources
    element.cpp
    spatial.cpp
    visual_element.cpp
    element_rectangle.cpp
    element_text.cpp
    element_image.cpp
    element_qr.cpp
    render_util.cpp
    animation.cpp
    title.cpp
    data-source.cpp
    qr.cpp
    stb.cpp
)

if(ENABLE_LUA_SCRIPTING)
    list(APPEND engine_sources script.cpp lua_http.cpp)
endif()

add_library(engine STATIC ${engine_sources})

target_include_directories(engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if (stb_ADDED)
    target_include_directories(engine PRIVATE ${stb_SOURCE_DIR})
endif()

pkg_check_modules(MINIZIP REQUIRED IMPORTED_TARGET minizip)

if (cpp_zipper_ADDED)
    add_library(cpp_zipper INTERFACE IMPORTED)
    target_include_directories(cpp_zipper INTERFACE ${cpp_zipper_SOURCE_DIR})
    target_link_libraries(engine PUBLIC cpp_zipper PkgConfig::MINIZIP)
endif()

target_link_libraries(engine PUBLIC
    nlohmann_json
)

if(ENABLE_LUA_SCRIPTING)
    target_link_libraries(engine PUBLIC
        sol2
        lua
        ixwebsocket
        Threads::Threads
    )
endif()

target_link_libraries(engine PUBLIC
    PkgConfig::PANGOCAIRO
    PkgConfig::CAIRO
)
if(NOT WIN32)
    target_compile_options(engine PRIVATE
        ${PANGOCAIRO_CFLAGS_OTHER}
        ${CAIRO_CFLAGS_OTHER}
        -fPIC
    )
endif()

if(ENABLE_LUA_SCRIPTING)
    target_compile_definitions(engine PRIVATE SOL_NO_THREAD_LOCAL)
    target_compile_definitions(engine PUBLIC ENGINE_HAS_LUA_SCRIPTING)
endif()

# ── Test applications (SDL3) ──────────────────────────────────────────────────

option(BUILD_TESTS "Build SDL3 visual test applications" OFF)

if(BUILD_TESTS)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

    function(add_engine_test name)
        add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE engine PkgConfig::SDL3)
        # expose project root so tests can #include "title.h" etc. directly
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    endfunction()

    add_engine_test(test_shadow_shear)
    add_engine_test(test_mask)
    add_engine_test(test_animation)
    add_engine_test(test_slide)
    add_engine_test(test_wipe_shear)
    add_engine_test(test_data_anim)
    if(ENABLE_LUA_SCRIPTING)
        add_engine_test(test_script_trigger)
        add_engine_test(test_script_http)
    endif()
endif()
