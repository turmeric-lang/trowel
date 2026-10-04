cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" TROWEL_VERSION_RAW)
string(STRIP "${TROWEL_VERSION_RAW}" TROWEL_VERSION)

project(trowel
    VERSION ${TROWEL_VERSION}
    DESCRIPTION "Trowel — a native editor for turmeric"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

qt_standard_project_setup()

include(${CMAKE_SOURCE_DIR}/cmake/scintilla_qt.cmake)

qt_add_resources(TROWEL_RESOURCES resources/resources.qrc)

add_library(trowel_lib STATIC
    src/app/directory_view.cpp
    src/app/directory_view.h
    src/app/icon_font.cpp
    src/app/icon_font.h
    src/app/main_window.cpp
    src/app/main_window.h
    src/app/preferences_view.cpp
    src/app/preferences_view.h
    src/app/repl_pane.cpp
    src/app/repl_pane.h
    src/app/single_instance.cpp
    src/app/single_instance.h
    src/app/tab_bar.cpp
    src/app/tab_bar.h
    src/app/tab_content.h
    src/app/trowel_application.cpp
    src/app/trowel_application.h
    src/app/window_manager.cpp
    src/app/window_manager.h
    src/editor/editor_view.cpp
    src/editor/editor_view.h
    src/editor/lexers.h
    src/editor/lexer_adapter.cpp
    src/editor/scanner.h
    src/editor/scanner_c.cpp
    src/editor/scanner_cmake.cpp
    src/editor/scanner_json.cpp
    src/editor/scanner_just.cpp
    src/editor/scanner_markdown.cpp
    src/editor/scanner_python.cpp
    src/editor/scanner_sh.cpp
    src/editor/scanner_toml.cpp
    src/editor/dialect.cpp
    src/editor/scanner_turmeric.cpp
    src/editor/theme_loader.cpp
    src/editor/theme_loader.h
    src/lsp/lsp_client.cpp
    src/lsp/lsp_client.h
    src/lsp/lsp_location.h
    src/lsp/lsp_symbol.h
    src/lsp/lsp_manager.cpp
    src/lsp/lsp_manager.h
    src/lsp/lsp_position.cpp
    src/lsp/lsp_position.h
    src/lsp/lsp_transport.cpp
    src/lsp/lsp_transport.h
    src/repl/project_runner.cpp
    src/repl/project_runner.h
    src/trace/trace_runner.cpp
    src/trace/trace_runner.h
    src/repl/pty_session.h
    src/repl/terminal_view.cpp
    src/repl/terminal_view.h
    src/repl/repl_session.cpp
    src/repl/repl_session.h
    src/repl/run_buffer.cpp
    src/repl/run_buffer.h
    src/control/control_server.cpp
    src/control/control_server.h
    src/control/control_connection.cpp
    src/control/control_connection.h
    src/control/control_handlers.cpp
    src/control/control_handlers.h
    src/debug/dap_client.cpp
    src/debug/dap_client.h
    src/debug/breakpoint_model.cpp
    src/debug/breakpoint_model.h
    src/debug/debug_session.cpp
    src/debug/debug_session.h
    src/debug/timeline_strip.cpp
    src/debug/timeline_strip.h
    src/debug/debugger_view.cpp
    src/debug/debugger_view.h
    ${TROWEL_RESOURCES}
)
target_include_directories(trowel_lib PUBLIC src)
# The pty backend is per-platform: forkpty on POSIX, ConPTY on Windows.
# Two files rather than one with #ifdefs -- they share only the interface,
# and the Windows one needs windows.h at the top of the translation unit.
if(WIN32)
    target_sources(trowel_lib PRIVATE src/repl/pty_session_win.cpp)
else()
    target_sources(trowel_lib PRIVATE src/repl/pty_session.cpp)
endif()

target_link_libraries(trowel_lib PUBLIC Qt6::Widgets Qt6::Network scintilla_qt)
target_compile_options(trowel_lib PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

set(TROWEL_APP_ICON "${CMAKE_SOURCE_DIR}/resources/trowel.icns")
set_source_files_properties("${TROWEL_APP_ICON}" PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

qt_add_executable(trowel
    src/main.cpp
    ${TROWEL_APP_ICON}
)
target_link_libraries(trowel PRIVATE trowel_lib)

# Windows: a GUI subsystem binary, not a console one.
#
# Two reasons, and the second is not obvious.  The visible one is that a CUI
# binary pops a console window when launched from Explorer, behind the editor.
#
# The load-bearing one is the pseudo-console.  A CUI process launched from a
# shell is handed INHERITABLE std handles, and a child created with
# PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE picks those up in preference to the
# pty -- so the REPL child would write to whatever launched Trowel instead of
# to the terminal view, and the pty would carry only ConPTY's own init
# sequences.  Measured: 16 bytes through the pty with inheritable handles
# present, 134 bytes (program output plus cursor/clear/title escapes) with
# them cleared.  A GUI process has NULL std handles and is in the second
# situation by construction.  See src/repl/pty_session_win.cpp.
if(WIN32)
    set_target_properties(trowel PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# ---- Bundle a prebuilt Turmeric toolchain next to the editor ----------------
# Trowel ships a pinned `tur` so a fresh install has a working REPL / run-file
# without the user installing Turmeric separately. The archive is staged
# verbatim, so its internal layout is whatever the pinned version published;
# bundledTurPath() (src/repl/repl_session.cpp) probes for either shape under:
#   macOS: Trowel.app/Contents/Resources/turmeric/
#   Linux: <dir-of-trowel-binary>/turmeric/
# `tur` locates its stdlib by walking up from its own directory, so no env var
# is needed. When no prebuilt matches the host, we skip bundling and fall back
# to `tur` on PATH at runtime.
# Intentionally a normal (non-cache) variable so bumping it here is the single
# source of truth: the per-arch SHA-256s below are pinned to this exact tag, so
# the version must not be independently overridable via -D (which would leave a
# stale cached value pointing at mismatched hashes).
set(TROWEL_TURMERIC_VERSION "v0.62.0")

# Select the release asset + its SHA-256 for this platform/arch. Hashes are from
# the release's sha256sums.txt.
#
# `_turmeric_ext` exists because the Windows asset is a .zip where the others
# are .tar.gz. The archive LAYOUT no longer needs a per-platform branch here:
# Windows has always shipped the PREFIX layout (bin/ include/ lib/ share/) and
# the .tar.gz targets were flat through v0.46.0 before being unified onto it.
# The pin is past the unification now, so every target ships the prefix layout
# and that is the branch bundledTurPath() (src/repl/repl_session.cpp) takes --
# but it still accepts either, which is what lets this pin move backwards as
# well as forwards. See the turmeric-side report `unify-release-archive-layout`.
set(_turmeric_key "")
set(_turmeric_ext "tar.gz")
if(APPLE)
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64" OR
       CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        set(_turmeric_key "macos-arm64")
        set(_turmeric_sha "643ffcee2c5434d28b3e2862b1afc759cc62929beeb936de7a22af0ce8b67342")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
        set(_turmeric_key "linux-x86_64")
        set(_turmeric_sha "5fbb8ba6893a20ad3b38b5eca5d2e657fcbd26488d473c58f55e5bd103922878")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(_turmeric_key "linux-aarch64")
        set(_turmeric_sha "0baf34cb664d8531d760a325c16ae350acf637637032f94ba99103196f4284a5")
    endif()
elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^([Aa][Mm][Dd]64|x86_64|X86)$" OR
       CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_turmeric_key "windows-x86_64")
        set(_turmeric_ext "zip")
        set(_turmeric_sha "06fcb6084e9f71fda7e7ee90041873909018c31d8736b759de0bee9abc501ae2")
    endif()
endif()

# Regenerated whenever the version above changes, which is what forces the
# `trowel` target to relink and therefore reruns the POST_BUILD staging below.
# See the comment in cmake/build_info.h.in — without this, bumping the toolchain
# leaves the previous `tur` staged in the bundle and the build says it has
# nothing to do.
if(_turmeric_key)
    set(TROWEL_BUNDLED_TURMERIC "${TROWEL_TURMERIC_VERSION}")
else()
    set(TROWEL_BUNDLED_TURMERIC "")
endif()
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/build_info.h.in"
    "${CMAKE_BINARY_DIR}/generated/trowel/build_info.h"
    @ONLY
)
target_include_directories(trowel PRIVATE "${CMAKE_BINARY_DIR}/generated")

if(_turmeric_key)
    include(FetchContent)
    FetchContent_Declare(turmeric_prebuilt
        URL      "https://github.com/turmeric-lang/turmeric/releases/download/${TROWEL_TURMERIC_VERSION}/turmeric-${TROWEL_TURMERIC_VERSION}-${_turmeric_key}.${_turmeric_ext}"
        URL_HASH SHA256=${_turmeric_sha}
    )
    FetchContent_MakeAvailable(turmeric_prebuilt)

    # Stage next to the built binary. On macOS this lands inside the app bundle
    # BEFORE codesigning (POST_BUILD commands run in the order added, and the
    # codesign command below is added later), so the signature seals it in.
    if(APPLE)
        set(_turmeric_stage_dir "$<TARGET_BUNDLE_CONTENT_DIR:trowel>/Resources/turmeric")
    else()
        set(_turmeric_stage_dir "$<TARGET_FILE_DIR:trowel>/turmeric")
    endif()

    # REMOVE the stage directory before copying into it. `copy_directory` merges
    # rather than replaces, so without this every file a PREVIOUS pin staged
    # survives the bump. That is not theoretical: bumping v0.46.0 -> v0.60.1 on a
    # tree that had once been pinned to v0.42.2 left a v0.42.2 `tur` and its
    # stdlib at the archive root (the flat layout) beside the v0.60.1 one under
    # `bin/` (the prefix layout), in the same bundle, both codesigned in.
    # bundledTurPath() probes `bin/` first so the right one still ran, which is
    # exactly why this went unnoticed -- the wrong binary was reachable, merely
    # not reached.
    add_custom_command(TARGET trowel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_turmeric_stage_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${turmeric_prebuilt_SOURCE_DIR}" "${_turmeric_stage_dir}"
        COMMENT "Staging Turmeric ${TROWEL_TURMERIC_VERSION} (${_turmeric_key}) next to trowel"
        VERBATIM
    )
else()
    message(WARNING
        "No prebuilt Turmeric for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}; "
        "Trowel will fall back to `tur` on PATH at runtime.")
endif()

if(APPLE)
    set_target_properties(trowel PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Trowel"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.turmeric-lang.TrowelEditor"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "trowel.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/resources/Info.plist.in"
    )

    # Ship the `trowel` CLI shim inside the bundle so Homebrew's cask can
    # symlink it onto PATH. This must run BEFORE codesigning below — the
    # bundle signature seals Contents/Resources, so staging afterwards would
    # invalidate the signature. POST_BUILD commands run in the order added.
    add_custom_command(TARGET trowel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/resources/trowel-cli.sh"
            "$<TARGET_BUNDLE_CONTENT_DIR:trowel>/Resources/trowel"
        COMMAND chmod +x "$<TARGET_BUNDLE_CONTENT_DIR:trowel>/Resources/trowel"
        COMMENT "Staging trowel CLI shim into app bundle"
        VERBATIM
    )

    set(TROWEL_CODESIGN_IDENTITY "" CACHE STRING
        "Developer ID Application identity (SHA1 or name) used to codesign Trowel.app. Empty disables signing.")
    set(TROWEL_CODESIGN_ENTITLEMENTS
        "${CMAKE_SOURCE_DIR}/resources/trowel.entitlements"
        CACHE FILEPATH
        "Path to the entitlements plist used when codesigning Trowel.app.")

    if(NOT TROWEL_CODESIGN_IDENTITY STREQUAL "")
        # Bundle the Qt frameworks (and platform/style plugins) INTO the app so
        # it is self-contained. Without this the notarized binary keeps its
        # build-machine LC_LOAD_DYLIB paths into Homebrew's /opt/homebrew Qt,
        # which a Finder launch either can't find or — because Homebrew's Qt is
        # ad-hoc signed with a different Team ID — is refused from loading under
        # the hardened runtime, aborting before main(). macdeployqt copies the
        # frameworks under Contents/Frameworks + Contents/PlugIns and rewrites
        # the install names to @rpath. It runs BEFORE codesign below (POST_BUILD
        # commands run in the order added) so codesign-app.sh re-signs the
        # freshly bundled frameworks and plugins with our Developer ID.
        #
        # Locate macdeployqt next to qmake rather than trusting PATH, so a build
        # that found Qt via CMAKE_PREFIX_PATH uses that same Qt's tool.
        get_target_property(_trowel_qmake_loc Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(_trowel_qt_bin_dir "${_trowel_qmake_loc}" DIRECTORY)
        find_program(MACDEPLOYQT_EXECUTABLE
            NAMES macdeployqt
            HINTS "${_trowel_qt_bin_dir}"
            REQUIRED)

        add_custom_command(TARGET trowel POST_BUILD
            COMMAND "${MACDEPLOYQT_EXECUTABLE}"
                "$<TARGET_BUNDLE_DIR:trowel>"
                -no-strip
            COMMENT "Bundling Qt frameworks into Trowel.app with macdeployqt"
            VERBATIM
        )

        add_custom_command(TARGET trowel POST_BUILD
            COMMAND "${CMAKE_SOURCE_DIR}/scripts/codesign-app.sh"
                "${TROWEL_CODESIGN_IDENTITY}"
                "${TROWEL_CODESIGN_ENTITLEMENTS}"
                "$<TARGET_BUNDLE_DIR:trowel>"
            COMMENT "Codesigning Trowel.app with ${TROWEL_CODESIGN_IDENTITY}"
            VERBATIM
        )
    endif()
endif()

# ---- Linux install layout ---------------------------------------------------
# Install the binary with the bundled Turmeric toolchain as a sibling `turmeric/`
# directory, matching the runtime probe in src/repl/repl_session.cpp. That probe
# accepts EITHER published archive layout -- `turmeric/bin/tur` for the prefix
# layout the releases have used since Turmeric v0.47.0, and `turmeric/tur` for
# the flat one before it -- and the staged tree is whichever shape the pin
# fetched, so neither path can be assumed here.
#
# This comment used to name the flat path alone, which is the assumption that
# broke every AppImage build from v0.3.0 to v0.3.1: scripts/build-appimage.sh
# hardcoded it for its `ldd` lookup. Keep both spellings in mind when touching
# anything that reaches into the staged toolchain.
#
# This is what the AppImage recipe (docs/guides) stages into the AppDir.
# Desktop-file, icon, and MIME integration are added in a later step.
if(NOT APPLE)
    include(GNUInstallDirs)
    install(TARGETS trowel RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(_turmeric_key)
        install(DIRECTORY "${turmeric_prebuilt_SOURCE_DIR}/"
                DESTINATION "${CMAKE_INSTALL_BINDIR}/turmeric"
                USE_SOURCE_PERMISSIONS)
    endif()

    # Desktop integration: .desktop launcher, XDG MIME type for .tur/.sweet, and
    # the hicolor icon set. These land in the standard XDG data locations; the
    # AppImage recipe (scripts/build-appimage.sh) installs into an AppDir and
    # linuxdeploy picks them up from here.
    install(FILES "${CMAKE_SOURCE_DIR}/resources/linux/trowel.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${CMAKE_SOURCE_DIR}/resources/linux/trowel.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages")
    install(FILES "${CMAKE_SOURCE_DIR}/resources/linux/com.turmeric_lang.Trowel.metainfo.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
    foreach(_sz 32 48 64 128 256 512)
        install(FILES
            "${CMAKE_SOURCE_DIR}/resources/linux/icons/hicolor/${_sz}x${_sz}/apps/trowel.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_sz}x${_sz}/apps")
    endforeach()
endif()
