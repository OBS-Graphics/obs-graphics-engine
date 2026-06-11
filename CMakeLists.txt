cmake_minimum_required(VERSION 3.16)
project(obs-graphics-engine)

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
    set(PANGOCAIRO_INCLUDE_DIRS ${PANGO_INCLUDE_DIR} ${GLIB_INCLUDE_DIR} ${GLIBCONFIG_INCLUDE_DIR})
    set(PANGOCAIRO_LIBRARIES ${PANGOCAIRO_LIB})

    find_package(Lua REQUIRED)
    set(LUA_INCLUDE_DIRS ${LUA_INCLUDE_DIR})
else()
    # macOS / Linux: use pkg-config via IMPORTED_TARGET so keg-only Homebrew
    # packages (lua@5.4) get their library search dirs automatically.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)
    pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
    pkg_check_modules(LUA IMPORTED_TARGET lua5.4)
    if(NOT LUA_FOUND)
        pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua)
    endif()
endif()

CPMAddPackage(
    NAME sol2
    GITHUB_REPOSITORY ThePhD/sol2
    GIT_TAG v3.3.0
    OPTIONS "SOL2_ENABLE_INSTALL OFF"
)

add_library(engine STATIC
    element.cpp
    animation.cpp
    graphic.cpp
    scene.cpp
    data-source.cpp
    script.cpp
)

# Common include dirs for all platforms
target_include_directories(engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${sol2_SOURCE_DIR}/include
)

if(WIN32)
    target_include_directories(engine PUBLIC
        ${PANGOCAIRO_INCLUDE_DIRS}
        ${CAIRO_INCLUDE_DIRS}
        ${LUA_INCLUDE_DIRS}
    )
    target_link_libraries(engine PUBLIC
        ${PANGOCAIRO_LIBRARIES}
        ${CAIRO_LIBRARIES}
        ${LUA_LIBRARIES}
    )
else()
    # IMPORTED_TARGET handles include dirs, library dirs, and link flags
    target_link_libraries(engine PUBLIC
        PkgConfig::PANGOCAIRO
        PkgConfig::CAIRO
        PkgConfig::LUA
    )
    target_compile_options(engine PRIVATE
        ${PANGOCAIRO_CFLAGS_OTHER}
        ${CAIRO_CFLAGS_OTHER}
        -fPIC
    )
endif()

target_compile_definitions(engine PRIVATE SOL_NO_THREAD_LOCAL)

target_compile_features(engine PUBLIC cxx_std_20)
