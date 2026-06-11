cmake_minimum_required(VERSION 3.16)
project(obs-graphics-engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)

if(WIN32)
    # Windows: vcpkg toolchain populates CMAKE_PREFIX_PATH; find libraries directly.
    # cairo headers live at include/cairo/cairo.h; pango includes <cairo.h> so we
    # need both include/ and include/cairo/ on the search path.
    find_path(CAIRO_INCLUDE_BASE NAMES cairo/cairo.h REQUIRED)
    find_library(CAIRO_LIB NAMES cairo REQUIRED)
    set(CAIRO_INCLUDE_DIRS ${CAIRO_INCLUDE_BASE} "${CAIRO_INCLUDE_BASE}/cairo")
    set(CAIRO_LIBRARIES ${CAIRO_LIB})

    find_path(PANGO_INCLUDE_DIR NAMES pango/pangocairo.h PATH_SUFFIXES pango-1.0 REQUIRED)
    find_path(GLIB_INCLUDE_DIR NAMES glib.h PATH_SUFFIXES glib-2.0 REQUIRED)
    # glibconfig.h is arch-specific and lives under lib/, not include/
    find_path(GLIBCONFIG_INCLUDE_DIR NAMES glibconfig.h PATH_SUFFIXES lib/glib-2.0/include REQUIRED)
    find_library(PANGOCAIRO_LIB NAMES pangocairo-1.0 REQUIRED)
    find_library(HARFBUZZ_LIB NAMES harfbuzz REQUIRED)
    set(PANGOCAIRO_INCLUDE_DIRS ${PANGO_INCLUDE_DIR} ${GLIB_INCLUDE_DIR} ${GLIBCONFIG_INCLUDE_DIR})
    set(PANGOCAIRO_LIBRARIES ${PANGOCAIRO_LIB} ${HARFBUZZ_LIB})
else()
    # macOS / Linux: use pkg-config via IMPORTED_TARGET so keg-only Homebrew
    # packages get their library search dirs automatically.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
    pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
endif()

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

add_library(engine STATIC
    element.cpp
    animation.cpp
    graphic.cpp
    scene.cpp
    data-source.cpp
    script.cpp
)

target_include_directories(engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(engine PUBLIC
    sol2
    nlohmann_json
    lua
)

if(WIN32)
    target_include_directories(engine PUBLIC
        ${PANGOCAIRO_INCLUDE_DIRS}
        ${CAIRO_INCLUDE_DIRS}
    )
    target_link_libraries(engine PUBLIC
        ${PANGOCAIRO_LIBRARIES}
        ${CAIRO_LIBRARIES}
    )
else()
    target_link_libraries(engine PUBLIC
        PkgConfig::PANGOCAIRO
        PkgConfig::CAIRO
    )
    target_compile_options(engine PRIVATE
        ${PANGOCAIRO_CFLAGS_OTHER}
        ${CAIRO_CFLAGS_OTHER}
        -fPIC
    )
endif()

target_compile_definitions(engine PRIVATE SOL_NO_THREAD_LOCAL)
