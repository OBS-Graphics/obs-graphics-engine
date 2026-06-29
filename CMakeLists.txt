cmake_minimum_required(VERSION 3.16)
project(obs-graphics-engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)

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

add_library(engine STATIC
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
    script.cpp
    qr.cpp
    stb.cpp
)

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
    sol2
    nlohmann_json
    lua
)

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

target_compile_definitions(engine PRIVATE SOL_NO_THREAD_LOCAL)

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
endif()
