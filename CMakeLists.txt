cmake_minimum_required(VERSION 3.16)
project(obs-graphics-engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS ON)

option(ENABLE_LUA_SCRIPTING "Build Lua/sol2 scripting support (ScriptDataSource)" ON)
option(BUILD_TESTS "Build SDL3 visual test applications" OFF)

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

    # OpenSSL: TLS backend for libcurl's HttpClient. System-installed on
    # Linux/macOS; on Windows, install via vcpkg and pass
    # -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake.
    find_package(OpenSSL 3.0 REQUIRED)

    # libcurl: production HTTP client for the Lua `http.*` table
    # (lua_http.cpp). Source-fetched via CPM (same self-contained-static
    # pattern this repo already uses for lua/sol2 above), statically linked,
    # OpenSSL as TLS backend. Only HTTP(S) is needed, so the other protocols
    # curl supports are disabled to trim build time/surface.
    CPMAddPackage(
        NAME curl
        GITHUB_REPOSITORY curl/curl
        GIT_TAG curl-8_21_0
        OPTIONS
            "CURL_USE_OPENSSL ON"
            "CURL_USE_LIBPSL OFF"
            "BUILD_SHARED_LIBS OFF"
            "BUILD_CURL_EXE OFF"
            "BUILD_TESTING OFF"
            "CURL_DISABLE_LDAP ON"
            "CURL_DISABLE_TELNET ON"
            "CURL_DISABLE_DICT ON"
            "CURL_DISABLE_GOPHER ON"
            "CURL_DISABLE_SMTP ON"
            "CURL_DISABLE_POP3 ON"
            "CURL_DISABLE_IMAP ON"
            "CURL_DISABLE_FTP ON"
            "CURL_DISABLE_TFTP ON"
            "CURL_DISABLE_RTSP ON"
    )
    if(TARGET libcurl_static)
        set_target_properties(libcurl_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()

    # pugixml: XML parser behind the Lua `xml.*` table (lua_xml.cpp). Built
    # from source as a hand-rolled static target rather than via pugixml's own
    # CMakeLists — it is a single translation unit, and this repo has already
    # been bitten both by a dependency inheriting BUILD_SHARED_LIBS and by a
    # static lib missing PIC when linked into the OBS shared module.
    CPMAddPackage(
        NAME pugixml
        GITHUB_REPOSITORY zeux/pugixml
        GIT_TAG v1.15
        DOWNLOAD_ONLY YES
    )
    if(pugixml_ADDED)
        add_library(pugixml STATIC ${pugixml_SOURCE_DIR}/src/pugixml.cpp)
        target_include_directories(pugixml PUBLIC
            $<BUILD_INTERFACE:${pugixml_SOURCE_DIR}/src>
        )
        set_target_properties(pugixml PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()

    # cpp-httplib: header-only, MIT, test-only. Provides the local mock HTTP
    # server tests/test_script_http.cpp needs — libcurl has no server-side
    # API. Not linked into `engine`, only into that one test binary.
    if(BUILD_TESTS)
        CPMAddPackage(
            NAME httplib
            GITHUB_REPOSITORY yhirose/cpp-httplib
            GIT_TAG v0.49.0
            DOWNLOAD_ONLY YES
        )
        if(httplib_ADDED)
            add_library(cpp_httplib INTERFACE IMPORTED)
            target_include_directories(cpp_httplib INTERFACE ${httplib_SOURCE_DIR})
        endif()
    endif()
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
    scene.cpp
    uuid.cpp
    data-source.cpp
    data-pool.cpp
    qr.cpp
    stb.cpp
)

if(ENABLE_LUA_SCRIPTING)
    list(APPEND engine_sources script.cpp lua_http.cpp lua_json.cpp lua_xml.cpp)
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
        Threads::Threads
    )
    target_link_libraries(engine PRIVATE
        CURL::libcurl
        pugixml
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
    # Headless, no Lua required (fake IDataSource implementations stand in
    # for a slow/throwing/scripted source) -- built and run regardless of
    # ENABLE_LUA_SCRIPTING.
    add_engine_test(test_data_pool)
    add_engine_test(test_scene)
    if(ENABLE_LUA_SCRIPTING)
        add_engine_test(test_script_trigger)
        add_engine_test(test_script_data_parse)
        add_engine_test(test_script_http)
        target_link_libraries(test_script_http PRIVATE cpp_httplib)
    endif()

    # ── ctest registration ────────────────────────────────────────────────────
    # Only the headless console tests (no SDL window, no display needed) are
    # registered with ctest: test_shadow_shear/test_mask/test_animation/
    # test_slide/test_wipe_shear/test_data_anim open a real SDL3 window and are
    # meant for manual visual inspection (see each file's own "Visual test"
    # header comment), not unattended CI. Each headless binary's main() already
    # returns 0 on success / 1 with a "FAILED" message on failure, so ctest's
    # pass/fail detection needs nothing beyond the exit code.
    enable_testing()
    add_test(NAME test_data_pool COMMAND test_data_pool)
    add_test(NAME test_scene COMMAND test_scene)
    if(ENABLE_LUA_SCRIPTING)
        add_test(NAME test_script_trigger COMMAND test_script_trigger)
        add_test(NAME test_script_data_parse COMMAND test_script_data_parse)
        add_test(NAME test_script_http COMMAND test_script_http)
    endif()
endif()
