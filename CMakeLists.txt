cmake_minimum_required(VERSION 3.24)

# Static CRT so the staged folder runs on a machine without the VC++
# redistributable. CMP0091 is NEW from 3.15, and this must be set before the
# first project() so the FetchContent subprojects inherit it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

project(davesplorer LANGUAGES C CXX)
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The macOS platform layer is Objective-C++: Foundation and AppKit are what
# the desktop's file APIs are, the way GIO is on Linux.
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Everything the app needs at runtime lands here; copy the folder, run the exe.
set(DSP_STAGE_DIR "${CMAKE_BINARY_DIR}/stage")

include(FetchContent)

# ---------------------------------------------------------------------------
# SDL3, static.
# ---------------------------------------------------------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.8
    GIT_SHALLOW    TRUE)

# ---------------------------------------------------------------------------
# Dear ImGui (docking branch) and the Test Engine. The two must come from the
# same release: the engine reaches into imgui_internal.h.
# ---------------------------------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9-docking
    GIT_SHALLOW    TRUE)
FetchContent_Declare(imgui_test_engine
    GIT_REPOSITORY https://github.com/ocornut/imgui_test_engine.git
    GIT_TAG        v1.92.9b
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(SDL3 imgui imgui_test_engine)

set(IMTE_DIR "${imgui_test_engine_SOURCE_DIR}/imgui_test_engine")

# imgui.cpp is compiled with the test-engine hooks enabled so the e2e binary
# can drive the real UI. IMGUI_USER_CONFIG carries that define (and the
# engine's own imconfig) into every translation unit that includes imgui.h.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${IMTE_DIR}
    ${CMAKE_SOURCE_DIR}/cmake)
target_compile_definitions(imgui PUBLIC IMGUI_USER_CONFIG="dsp_imconfig.h")

add_library(imgui_backends STATIC
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_link_libraries(imgui_backends PUBLIC imgui SDL3::SDL3-static)

add_library(imgui_test_engine STATIC
    ${IMTE_DIR}/imgui_capture_tool.cpp
    ${IMTE_DIR}/imgui_te_context.cpp
    ${IMTE_DIR}/imgui_te_coroutine.cpp
    ${IMTE_DIR}/imgui_te_engine.cpp
    ${IMTE_DIR}/imgui_te_exporters.cpp
    ${IMTE_DIR}/imgui_te_perftool.cpp
    ${IMTE_DIR}/imgui_te_ui.cpp
    ${IMTE_DIR}/imgui_te_utils.cpp)
target_include_directories(imgui_test_engine SYSTEM PUBLIC ${imgui_test_engine_SOURCE_DIR})
target_link_libraries(imgui_test_engine PUBLIC imgui)
# The enabled hooks make these static libraries mutually dependent.
target_link_libraries(imgui PUBLIC imgui_test_engine)

# Third-party code compiles at its own warning level; the strict flags below
# apply only to project targets, through dsp_strict.
add_library(dsp_strict INTERFACE)
if(MSVC)
    # SYSTEM include dirs become /external:I; W0 there keeps ImGui's and the
    # SDK's own warnings out of a build that treats ours as errors.
    target_compile_options(dsp_strict INTERFACE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /external:W0)
    target_compile_definitions(dsp_strict INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endif()

# stb_image, for the built-in image viewer. Vendored: one public-domain
# header, pinned by being in the tree. Its own target so the strict flags
# stay off it.
add_library(stb_image STATIC third_party/stb/stb_image_impl.c)
target_include_directories(stb_image SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/stb)

# ---------------------------------------------------------------------------
# Application libraries.
#   dsp_app : state and logic, plus the OS layer it calls. No ImGui.
#   dsp_ui  : drawing. Reads dsp_app state, owns none of it.
# ---------------------------------------------------------------------------
if(WIN32)
    set(DSP_PLATFORM_DIR src/platform)
    set(DSP_PLATFORM_EXT cpp)
elseif(APPLE)
    set(DSP_PLATFORM_DIR src/platform/macos)
    set(DSP_PLATFORM_EXT mm)
else()
    set(DSP_PLATFORM_DIR src/platform/linux)
    set(DSP_PLATFORM_EXT cpp)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
endif()

add_library(dsp_app STATIC
    src/app/AppState.cpp
    src/app/Format.cpp
    src/app/PathUtil.cpp
    src/app/Listing.cpp
    src/app/TreeScan.cpp
    src/app/Navigation.cpp
    src/app/Selection.cpp
    src/app/Settings.cpp
    src/app/FileOps.cpp
    src/app/Shortcuts.cpp
    src/app/Image.cpp
    src/app/Layout.cpp
    ${DSP_PLATFORM_DIR}/Strings.${DSP_PLATFORM_EXT}
    ${DSP_PLATFORM_DIR}/Paths.${DSP_PLATFORM_EXT}
    ${DSP_PLATFORM_DIR}/FileSystem.${DSP_PLATFORM_EXT}
    ${DSP_PLATFORM_DIR}/Clipboard.${DSP_PLATFORM_EXT}
    ${DSP_PLATFORM_DIR}/DragDrop.${DSP_PLATFORM_EXT}
    ${DSP_PLATFORM_DIR}/Shell.${DSP_PLATFORM_EXT})
target_include_directories(dsp_app PUBLIC src)
target_link_libraries(dsp_app PUBLIC dsp_strict stb_image)
if(WIN32)
    target_link_libraries(dsp_app PUBLIC shell32 ole32 shlwapi gdi32)
elseif(APPLE)
    target_link_libraries(dsp_app PUBLIC SDL3::SDL3-static
        "-framework Foundation" "-framework AppKit" "-framework UniformTypeIdentifiers"
        "-framework QuickLookThumbnailing")
    target_compile_options(dsp_strict INTERFACE -Wall -Wextra -Wpedantic -Werror)
else()
    target_link_libraries(dsp_app PUBLIC PkgConfig::GIO SDL3::SDL3-static)
    target_compile_options(dsp_strict INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(dsp_ui STATIC
    src/ui/Theme.cpp
    src/ui/Fonts.cpp
    src/ui/Widgets.cpp
    src/ui/DragDrop.cpp
    src/ui/ShortcutBar.cpp
    src/ui/MainWindow.cpp
    src/ui/NavPane.cpp
    src/ui/FilesPane.cpp
    src/ui/FileGrid.cpp
    src/ui/ImageView.cpp
    src/ui/Layouts.cpp
    src/ui/Textures.cpp
    src/ui/Thumbnails.cpp
    src/ui/Dialogs.cpp)
target_link_libraries(dsp_ui PUBLIC dsp_app imgui)

# ---------------------------------------------------------------------------
# The application.
# ---------------------------------------------------------------------------
# MACOSX_BUNDLE is what gives the app an icon, a name in the Dock and the
# menu bar, and a Finder identity: a bare executable has no bundle for
# LaunchServices to read those from, and shows up borrowing the icon of
# whatever launched it.
add_executable(davesplorer WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/platform/Host.cpp)
if(WIN32)
    target_sources(davesplorer PRIVATE src/platform/app.rc)
endif()
target_link_libraries(davesplorer PRIVATE dsp_ui imgui_backends imgui_test_engine SDL3::SDL3-static)
set_target_properties(davesplorer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${DSP_STAGE_DIR}")

if(APPLE)
    # The .icns is built from the same 256px PNG the other platforms use, so
    # the icon has one source; sips and iconutil ship with macOS. 256px is
    # the ceiling that PNG can fill, which covers the Dock at Retina.
    set(DSP_ICONSET "${CMAKE_BINARY_DIR}/davesplorer.iconset")
    set(DSP_ICNS "${CMAKE_BINARY_DIR}/davesplorer.icns")
    set(DSP_ICON_PNG "${CMAKE_SOURCE_DIR}/assets/icon/davesplorer.png")
    add_custom_command(OUTPUT "${DSP_ICNS}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${DSP_ICONSET}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DSP_ICONSET}"
        COMMAND sips -z 16 16    "${DSP_ICON_PNG}" --out "${DSP_ICONSET}/icon_16x16.png" > /dev/null
        COMMAND sips -z 32 32    "${DSP_ICON_PNG}" --out "${DSP_ICONSET}/icon_16x16@2x.png" > /dev/null
        COMMAND sips -z 32 32    "${DSP_ICON_PNG}" --out "${DSP_ICONSET}/icon_32x32.png" > /dev/null
        COMMAND sips -z 64 64    "${DSP_ICON_PNG}" --out "${DSP_ICONSET}/icon_32x32@2x.png" > /dev/null
        COMMAND sips -z 128 128  "${DSP_ICON_PNG}" --out "${DSP_ICONSET}/icon_128x128.png" > /dev/null
        COMMAND ${CMAKE_COMMAND} -E copy "${DSP_ICON_PNG}" "${DSP_ICONSET}/icon_128x128@2x.png"
        COMMAND ${CMAKE_COMMAND} -E copy "${DSP_ICON_PNG}" "${DSP_ICONSET}/icon_256x256.png"
        COMMAND iconutil -c icns -o "${DSP_ICNS}" "${DSP_ICONSET}"
        DEPENDS "${DSP_ICON_PNG}"
        COMMENT "Building davesplorer.icns from the app icon")
    target_sources(davesplorer PRIVATE "${DSP_ICNS}")
    set_source_files_properties("${DSP_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(davesplorer PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Davesplorer"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.davesplorer.Davesplorer"
        MACOSX_BUNDLE_BUNDLE_VERSION "0.1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1.0"
        MACOSX_BUNDLE_COPYRIGHT "MIT"
        MACOSX_BUNDLE_ICON_FILE "davesplorer"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in")
    set(DSP_ASSET_DEST "$<TARGET_BUNDLE_CONTENT_DIR:davesplorer>/Resources/assets")
else()
    set(DSP_ASSET_DEST "${DSP_STAGE_DIR}/assets")
endif()

add_custom_command(TARGET davesplorer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets" "${DSP_ASSET_DEST}"
    COMMENT "Staging assets for davesplorer")

# The Explorer interceptor: a standalone, dependency-free Win32 exe that
# Windows runs in place of explorer.exe when register.ps1 -Interceptor is
# used. Kept tiny and separate from the app because it runs on the shell's
# startup path.
if(WIN32)
add_executable(explorer_shim WIN32 src/platform/explorer_shim.cpp)
target_link_libraries(explorer_shim PRIVATE shlwapi shell32)
set_target_properties(explorer_shim PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${DSP_STAGE_DIR}"
    OUTPUT_NAME explorer_shim)

# The registration script is staged through a real dependency rather than a
# POST_BUILD step: POST_BUILD only runs when the exe relinks, and a script
# edit alone must still refresh the staged copy.
add_custom_command(OUTPUT "${DSP_STAGE_DIR}/register.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/tools/register.ps1" "${DSP_STAGE_DIR}/register.ps1"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/register.ps1"
    COMMENT "Staging register.ps1")
add_custom_target(dsp_stage_tools ALL DEPENDS "${DSP_STAGE_DIR}/register.ps1")

endif() # Windows shell registration

# ---------------------------------------------------------------------------
# Tests: console binaries.
# ---------------------------------------------------------------------------

enable_testing()
set(DSP_TEST_DIR "${CMAKE_BINARY_DIR}/tests")

add_executable(davesplorer_unit_tests tests/unit_tests.cpp)
target_link_libraries(davesplorer_unit_tests PRIVATE dsp_app)
set_target_properties(davesplorer_unit_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${DSP_TEST_DIR}")
add_test(NAME unit_tests COMMAND davesplorer_unit_tests)

add_executable(davesplorer_e2e_tests tests/e2e_tests.cpp)
target_link_libraries(davesplorer_e2e_tests PRIVATE dsp_ui imgui_test_engine)
target_compile_definitions(davesplorer_e2e_tests PRIVATE DSP_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
set_target_properties(davesplorer_e2e_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${DSP_TEST_DIR}")
add_test(NAME e2e_tests COMMAND davesplorer_e2e_tests)

if(NOT WIN32)
    # A memory GSettings backend and a private XDG data directory: the dash
    # tests pin and unpin for real, and must not touch the developer's own
    # dash or install launchers into their home. CTest sets both before the
    # process starts, which is what GLib's one-time caching of the user data
    # directory requires.
    set_tests_properties(unit_tests e2e_tests PROPERTIES ENVIRONMENT
        "SDL_VIDEODRIVER=dummy;GSETTINGS_BACKEND=memory;XDG_DATA_HOME=${CMAKE_BINARY_DIR}/test-xdg-data")
endif()
