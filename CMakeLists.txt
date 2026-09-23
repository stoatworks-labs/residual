cmake_minimum_required(VERSION 3.15)

project(Residual
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal by default so one bundle loads in both the Apple Silicon and the
# Intel build of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64 for a
# faster dev build.
#
# This must sit immediately after project() and before any target exists. CMake
# latches the architecture list when the first target is created; setting it
# later is silently ignored and you get an arm64-only binary that the build log
# calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h. Set
# globally rather than per-file: the next file to include windows.h hits it too,
# and the error gives no hint what happened.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(RESIDUAL_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule pinned to b1afaf9 -- the rev the
# rest of the fleet pins.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin is, as an OBJECT library so both the bundle and the
# offline harness are built from exactly the same code.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor in Residual.cpp and nothing ever references that
# object by name, so in an archive the linker is entitled to drop the whole
# translation unit -- and the result is a bundle that loads, exports plugMain,
# and reports that it contains no plugins.
# ---------------------------------------------------------------------------
add_library(residual_core OBJECT
    source/Buffer.h
    source/Buffer.cpp
    source/Codec.h
    source/Codec.cpp
    source/Controls.h
    source/Controls.cpp
    source/Diag.h
    source/Diag.cpp
    source/Onset.h
    source/Onset.cpp
    source/Shaders.h
    source/Shaders.cpp
    source/StoatworksAbout.h
    source/StoatworksAboutLinks.h
    source/StoatworksAboutParams.h
    source/Residual.h
    source/Residual.cpp
)

target_include_directories(residual_core PUBLIC source)
target_link_libraries(residual_core PUBLIC ffgl::sdk)
target_compile_definitions(residual_core PUBLIC RESIDUAL_VERSION="${PROJECT_VERSION}")
set_target_properties(residual_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers we include pull in <GL/glew.h>. The SDK
# links GLEW privately, so consumers need it too -- for the include path and
# for the GLEW_STATIC define the static vcpkg triplet propagates. macOS uses
# the system OpenGL framework and needs none of this. GLEW arrives through
# vcpkg.json, which nothing in this file points at: lose it and only Windows
# breaks, and only at configure time.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(residual_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin. FFGL resolves one plugMain per binary, and a codec has no source
# variant to want: with no clip there is nothing to code.
# ---------------------------------------------------------------------------
function(residual_add_plugin target entry bundle_name identifier)
    add_library(${target} MODULE ${entry})
    target_link_libraries(${target} PRIVATE residual_core)

    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${bundle_name}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL"
            "-framework Carbon"
            "-framework AppKit"
        )
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${bundle_name}")
    endif()
endfunction()

residual_add_plugin(Residual source/PluginEntry.cpp
    "Residual" "com.stoatworks.ffgl.residual")

# ---------------------------------------------------------------------------
# `cmake --install build` drops the bundle where Arena reads it. Avenue reads
# ~/Documents/Resolume Avenue/Extra Effects instead; pass --prefix for that.
# ---------------------------------------------------------------------------
if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()
install(TARGETS Residual
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)

# ---------------------------------------------------------------------------
# The harness: the real plugin class in a real GL context, with no host.
#
# macOS only, because it stands up a CGL context directly. What a decoder
# does with a vector field and a residual is integer arithmetic with a closed
# form, so this is where the codec is verified rather than merely previewed.
# See tools/rstest.
# ---------------------------------------------------------------------------
if(RESIDUAL_BUILD_TOOLS AND APPLE)
    enable_testing()

    add_executable(rstest tools/rstest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines here
    # rather than a vendored dependency.
    target_link_libraries(rstest PRIVATE residual_core z
        "-framework OpenGL"
        "-framework AppKit"
    )

    add_test(NAME transform COMMAND rstest --transform)
    add_test(NAME vectors   COMMAND rstest --vectors)
    add_test(NAME lossless  COMMAND rstest --lossless)
    add_test(NAME mosh      COMMAND rstest --mosh)
    add_test(NAME gop       COMMAND rstest --gop)
    add_test(NAME drift     COMMAND rstest --drift)
    add_test(NAME resize    COMMAND rstest --resize)
    add_test(NAME onset     COMMAND rstest --onset)
    add_test(NAME negative  COMMAND rstest --negative)
endif()
