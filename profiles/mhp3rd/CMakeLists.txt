set(MHP3RD_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
option(MHP3RD_BASELINE_B0 "Seal the observed B0 reference against native replacements" OFF)
set(MHP3RD_GAMEPLAY_SOURCE_COMMIT "" CACHE STRING "Gameplay source revision for paired build provenance")
set(MHP3RD_BASELINE_PROVENANCE_SHA256 "" CACHE STRING "Verified observed-B0 source-content digest")
if(MHP3RD_GAMEPLAY_SOURCE_COMMIT STREQUAL "")
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE MHP3RD_EFFECTIVE_GAMEPLAY_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(MHP3RD_EFFECTIVE_GAMEPLAY_COMMIT STREQUAL "")
        # Source archives can still build normally. Paired packaging rejects
        # unknown provenance until an explicit full source commit is supplied.
        set(MHP3RD_EFFECTIVE_GAMEPLAY_COMMIT "unknown")
    endif()
else()
    set(MHP3RD_EFFECTIVE_GAMEPLAY_COMMIT "${MHP3RD_GAMEPLAY_SOURCE_COMMIT}")
endif()
string(LENGTH "${MHP3RD_EFFECTIVE_GAMEPLAY_COMMIT}" gameplay_commit_length)
if(NOT MHP3RD_EFFECTIVE_GAMEPLAY_COMMIT STREQUAL "unknown" AND
   (NOT gameplay_commit_length EQUAL 40 OR NOT MHP3RD_EFFECTIVE_GAMEPLAY_COMMIT MATCHES "^[0-9a-f]+$"))
    message(FATAL_ERROR "mhp3rd: a full lowercase gameplay source commit is required")
endif()
if(MHP3RD_BASELINE_B0)
    string(LENGTH "${MHP3RD_BASELINE_PROVENANCE_SHA256}" provenance_length)
    if(NOT MHP3RD_EFFECTIVE_GAMEPLAY_COMMIT STREQUAL "4292eb66ee66eab37c327575382d071addcf6249" OR
       NOT provenance_length EQUAL 64 OR NOT MHP3RD_BASELINE_PROVENANCE_SHA256 MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "mhp3rd: observed B0 requires the pinned source and verified provenance digest")
    endif()
endif()

# The AOT corpus is derived from the user's executable and is not committed.
# Until scripts/generate.sh has produced it, link an empty registry instead.
file(GLOB MHP3RD_GENERATED CONFIGURE_DEPENDS "${MHP3RD_PROFILE_DIR}/generated/*.cpp")
if(MHP3RD_GENERATED)
    list(LENGTH MHP3RD_GENERATED MHP3RD_GENERATED_COUNT)
    message(STATUS "mhp3rd: ${MHP3RD_GENERATED_COUNT} generated AOT units")
    if(MSVC)
        if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
            set(MHP3RD_GENERATED_OPTIONS "/Od;/bigobj")
        else()
            set(MHP3RD_GENERATED_OPTIONS "/O${PSPRECOMP_GENERATED_OPT_LEVEL};/Ob0;/bigobj")
        endif()
    else()
        set(MHP3RD_GENERATED_OPTIONS "-O${PSPRECOMP_GENERATED_OPT_LEVEL};-g0")
    endif()
    set_source_files_properties(${MHP3RD_GENERATED} PROPERTIES COMPILE_OPTIONS "${MHP3RD_GENERATED_OPTIONS}")

    # The analog camera wraps the game's camera rotation helper, and main.cpp
    # has to hand it the generated unit that helper was compiled into. Which
    # unit that is depends on how the generator partitions the code, so read it
    # from the corpus instead of naming it: a new partition then finds its own
    # unit, and a corpus without the helper stops the build here rather than
    # letting the camera call the wrong code in game.
    set(MHP3RD_CAMERA_HELPER_UNIT "")
    foreach(unit IN LISTS MHP3RD_GENERATED)
        file(STRINGS "${unit}" hit REGEX "register_function\\(0x08878B70u, &recomp_unit_[0-9]+," LIMIT_COUNT 1)
        if(hit)
            string(REGEX MATCH "recomp_unit_[0-9]+" MHP3RD_CAMERA_HELPER_UNIT "${hit}")
            break()
        endif()
    endforeach()
    if(NOT MHP3RD_CAMERA_HELPER_UNIT)
        message(FATAL_ERROR "mhp3rd: no generated unit registers the camera rotation helper 0x08878B70; "
                            "regenerate the corpus from the supported executable, or update "
                            "host/camera/game_camera.cpp if the helper has moved")
    endif()
    message(STATUS "mhp3rd: camera rotation helper in ${MHP3RD_CAMERA_HELPER_UNIT}")
    # A regenerated corpus with the same file names must be read again.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MHP3RD_GENERATED})
else()
    message(STATUS "mhp3rd: no generated AOT units; run profiles/mhp3rd/scripts/generate.sh")
    set(MHP3RD_GENERATED host/generated_stub.cpp)
endif()

# Resolve the camera helper from the original corpus before substituting copies.
include("${MHP3RD_PROFILE_DIR}/cmake/ProbeInstrumentation.cmake")

# Renderer: SDL3 for the window and Vulkan for drawing. Both are optional so the
# profile still builds headless on a machine without them.
option(MHP3RD_RENDERER "Build the Vulkan renderer" ON)
set(MHP3RD_RENDERER_SOURCES)
if(MHP3RD_RENDERER)
    find_package(SDL3 QUIET)
    find_package(Vulkan QUIET)
    find_program(MHP3RD_GLSLANG NAMES glslangValidator glslang)
    if(SDL3_FOUND AND Vulkan_FOUND AND MHP3RD_GLSLANG)
        set(MHP3RD_SHADER_INC "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders/ge_shaders.inc")
        set(MHP3RD_SHADERS
            "kGeVertexShader=${MHP3RD_PROFILE_DIR}/host/gpu/shaders/ge.vert"
            "kGeRawVertexShader=GE_RAW_VERTICES@${MHP3RD_PROFILE_DIR}/host/gpu/shaders/ge.vert"
            "kGeCheckVertexShader=GE_RAW_VERTICES,GE_CHECK_DECODE@${MHP3RD_PROFILE_DIR}/host/gpu/shaders/ge.vert"
            "kGeFragmentShader=${MHP3RD_PROFILE_DIR}/host/gpu/shaders/ge.frag")
        if(ANDROID)
            # Pre-rotation of the finished frame for a display turned sideways.
            list(APPEND MHP3RD_SHADERS
                "kRotateVertexShader=${MHP3RD_PROFILE_DIR}/host/gpu/shaders/rotate.vert"
                "kRotateFragmentShader=${MHP3RD_PROFILE_DIR}/host/gpu/shaders/rotate.frag")
        endif()
        set(MHP3RD_SHADER_SOURCES ${MHP3RD_SHADERS})
        list(TRANSFORM MHP3RD_SHADER_SOURCES REPLACE "^[A-Za-z]+=([A-Z_,]+@)?" "")
        list(REMOVE_DUPLICATES MHP3RD_SHADER_SOURCES)
        add_custom_command(
            OUTPUT "${MHP3RD_SHADER_INC}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
            COMMAND python3 "${MHP3RD_PROFILE_DIR}/tools/embed_shaders.py" "${MHP3RD_GLSLANG}" "${MHP3RD_SHADER_INC}"
                    ${MHP3RD_SHADERS}
            DEPENDS ${MHP3RD_SHADER_SOURCES}
                    "${MHP3RD_PROFILE_DIR}/tools/embed_shaders.py"
            COMMENT "Compiling GE shaders to SPIR-V")
        add_custom_target(mhp3rd_shaders DEPENDS "${MHP3RD_SHADER_INC}")
        # Dear ImGui (third_party/imgui, unmodified) draws the port's own
        # interface over the game.
        set(MHP3RD_RENDERER_SOURCES
            host/testing/sdl_observers.cpp
            host/gpu/frame_interpolation.cpp
            host/gpu/frame_pacing.cpp
            host/gpu/replacement_textures.cpp
            host/gpu/vulkan_renderer.cpp
            host/ui/file_browser.cpp
            host/ui/font_menu.cpp
            host/ui/fonts.cpp
            host/ui/input_script.cpp
            host/ui/layer.cpp
            host/ui/localization.cpp
            host/ui/menu.cpp
            host/ui/mods_screen.cpp
            host/ui/save_screen.cpp
            host/ui/setup_screens.cpp
            host/ui/test_session_screen.cpp
            host/ui/text_input.cpp
            host/ui/touch_overlay.cpp
            host/ui/texture_pack_screen.cpp
            host/ui/widgets.cpp
            third_party/imgui/imgui.cpp
            third_party/imgui/imgui_draw.cpp
            third_party/imgui/imgui_tables.cpp
            third_party/imgui/imgui_widgets.cpp
            third_party/imgui/backends/imgui_impl_sdl3.cpp
            third_party/imgui/backends/imgui_impl_vulkan.cpp)
        message(STATUS "mhp3rd: Vulkan renderer enabled")
    else()
        message(STATUS "mhp3rd: renderer disabled (SDL3, Vulkan or glslang not found)")
        set(MHP3RD_RENDERER OFF)
    endif()
endif()

# The build the in-game menu shows: `git describe` of the checkout, refreshed
# on every build but rewritten only when it changes, so only the menu
# recompiles after a commit.
set(MHP3RD_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated_version/yakumo_version.hpp")
add_custom_target(mhp3rd_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUTPUT=${MHP3RD_VERSION_HEADER}
            -P "${MHP3RD_PROFILE_DIR}/tools/write_version.cmake"
    BYPRODUCTS "${MHP3RD_VERSION_HEADER}"
    COMMENT "Checking the build version")

# FFmpeg decodes the game's music and its movies; by default it is built with
# the project (see cmake/FFmpeg.cmake).
include("${MHP3RD_PROFILE_DIR}/cmake/FFmpeg.cmake")

# GE parsing, texture decoding and the audio mixer are backend independent and
# always built; only the device the sink opens depends on SDL.
set(MHP3RD_HOST_SOURCES
    host/app_paths.cpp
    host/audio/atrac_decoder.cpp
    host/movie/avc_decoder.cpp
    host/movie/psmf_demuxer.cpp
    host/audio/audio_sink.cpp
    host/audio/sas_core.cpp
    host/camera_probe.cpp
    host/camera/camera_input.cpp
    host/camera/game_aspect.cpp
    host/camera/game_camera.cpp
    host/native/angle_step.cpp
    host/native/angle_step_bridge.cpp
    host/native/scale_matrix.cpp
    host/native/scale_matrix_bridge.cpp
    host/native/translation_matrix.cpp
    host/native/translation_matrix_bridge.cpp
    host/native/vector_construct.cpp
    host/native/vector_construct_bridge.cpp
    host/native/matrix_copy.cpp
    host/native/matrix_copy_bridge.cpp
    host/gpu/ge_state.cpp
    host/gpu/texture_decode.cpp
    host/input/bindings.cpp
    host/input/touch_controls.cpp
    host/gpu/texture_pack.cpp
    host/gpu/texture_pack_import.cpp
    host/perf/frame_stats.cpp
    host/perf/perf_overlay.cpp
    host/overlays.cpp
    host/mhp3rd_profile.cpp
    host/kernel/kernel.cpp
    host/kernel/load_trace.cpp
    host/kernel/fast_loading.cpp
    host/kernel/load_detector.cpp
    host/kernel/iso_image.cpp
    host/hle/hle_common.cpp
    host/hle/hle_threadman.cpp
    host/hle/hle_sysmem.cpp
    host/hle/hle_io.cpp
    host/hle/hle_system.cpp
    host/hle/hle_media.cpp
    host/hle/hle_atrac.cpp
    host/hle/hle_mpeg.cpp
    host/fonts/game_font.cpp
    host/hle/hle_font.cpp
    host/hle/hle_utility.cpp
    host/hle/hle_savedata.cpp
    host/hle/hle_adhoc.cpp
    host/adhoc/client.cpp
    host/adhoc/discovery.cpp
    host/adhoc/host.cpp
    host/adhoc/server.cpp
    host/save_data/aes128.cpp
    host/save_data/param_sfo.cpp
    host/save_data/savedata_crypto.cpp
    host/save_data/savedata_store.cpp
    host/save_data/save_transfer.cpp
    host/install/executable_preparation.cpp
    host/install/front_end.cpp
    host/install/installer.cpp
    host/install/installer_dialogs.cpp
    host/install/user_data.cpp
    host/settings/settings.cpp
    host/mods/file_overlay.cpp
    host/mods/mod_import.cpp
    host/mods/mod_ini.cpp
    host/mods/mod_library.cpp
    host/mods/mod_session.cpp
    host/mods/mhp3rd_data_bin.cpp
    host/mods/mhp3rd_mod_format.cpp
    host/mods/mhp3rd_mods.cpp
    third_party/tiny_aes/aes.c
)
if(MHP3RD_BASELINE_B0)
    list(REMOVE_ITEM MHP3RD_HOST_SOURCES
        host/native/translation_matrix.cpp host/native/translation_matrix_bridge.cpp
        host/native/vector_construct.cpp host/native/vector_construct_bridge.cpp
        host/native/matrix_copy.cpp host/native/matrix_copy_bridge.cpp)
endif()
# tiny-AES-c is C; the project only enables C++, which compiles it unchanged.
set_source_files_properties(third_party/tiny_aes/aes.c PROPERTIES LANGUAGE CXX)

# The recompiled game code gets a target of its own. It needs only the
# psprecomp headers, so definitions and include paths the host adds to
# Yakumo (a renderer switch, a new library's headers) do not change its
# compile commands and rebuild all of it. Its objects still link straight into
# the executable, which exports their symbols to the overlay libraries.
add_library(mhp3rd_generated OBJECT ${MHP3RD_GENERATED})
target_include_directories(mhp3rd_generated PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${MHP3RD_PROFILE_DIR}/generated")
set_target_properties(mhp3rd_generated PROPERTIES JOB_POOL_COMPILE psprecomp_generated)

# An Android app is a shared library, libmain.so, that SDL's Java activity
# loads and calls; the command-line executable still builds for Android without
# this and runs from adb shell.
option(MHP3RD_ANDROID_APP "Build the Android app's libmain.so instead of an executable" OFF)
if(MHP3RD_ANDROID_APP AND NOT (ANDROID AND MHP3RD_RENDERER))
    message(FATAL_ERROR "MHP3RD_ANDROID_APP needs the Android NDK toolchain and the renderer (SDL3 and Vulkan)")
endif()
set(MHP3RD_PROGRAM_SOURCES
    host/main.cpp
    ${MHP3RD_HOST_SOURCES}
    ${MHP3RD_RENDERER_SOURCES}
    $<TARGET_OBJECTS:mhp3rd_generated>)
if(MHP3RD_ANDROID_APP)
    add_library(Yakumo SHARED ${MHP3RD_PROGRAM_SOURCES} host/platform/android_app.cpp
        host/platform/android_jni.cpp host/platform/android_documents.cpp host/platform/android_performance.cpp)
    target_compile_definitions(Yakumo PRIVATE MHP3RD_ANDROID_APP=1)
    set_target_properties(Yakumo PROPERTIES
        OUTPUT_NAME main
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
else()
    add_executable(Yakumo ${MHP3RD_PROGRAM_SOURCES})
endif()
if(WIN32)
    set(MHP3RD_WINDOWS_ICON "${MHP3RD_PROFILE_DIR}/packaging/windows/emblem.ico")
    configure_file(packaging/windows/Yakumo.rc.in Yakumo.rc @ONLY)
    target_sources(Yakumo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Yakumo.rc")
endif()
if(MHP3RD_RENDERER)
    add_dependencies(Yakumo mhp3rd_shaders mhp3rd_version)
    # The sink opens its own SDL audio subsystem, so it only needs SDL to exist.
    target_compile_definitions(Yakumo PRIVATE MHP3RD_HAS_RENDERER=1 MHP3RD_HAS_SDL_AUDIO=1 MHP3RD_HAS_SDL=1)
    target_include_directories(Yakumo PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
        "${CMAKE_CURRENT_BINARY_DIR}/generated_version"
        "${MHP3RD_PROFILE_DIR}/third_party/imgui")
    target_link_libraries(Yakumo PRIVATE SDL3::SDL3 Vulkan::Vulkan)
endif()
# The host only: the generated code never sees FFmpeg.
mhp3rd_use_ffmpeg(Yakumo)
target_include_directories(Yakumo PRIVATE
    "${MHP3RD_PROFILE_DIR}/host"
    "${MHP3RD_PROFILE_DIR}/third_party"
    "${MHP3RD_PROFILE_DIR}/generated")
target_link_libraries(Yakumo PRIVATE psprecomp_core ${CMAKE_DL_LIBS})
if(WIN32)
    # The ad hoc client's and server's sockets, and the list of interfaces.
    target_link_libraries(Yakumo PRIVATE ws2_32 iphlpapi)
endif()
find_package(Threads REQUIRED)
target_link_libraries(Yakumo PRIVATE Threads::Threads)
# The overlay libraries resolve psprecomp and runtime symbols against the
# executable that loads them, so it has to export them.
set_target_properties(Yakumo PROPERTIES
    ENABLE_EXPORTS TRUE
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
# Only main.cpp needs the camera helper's unit; keeping it off the target keeps
# it off every other unit's command line.
if(MHP3RD_CAMERA_HELPER_UNIT)
    set_property(SOURCE host/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        MHP3RD_CAMERA_HELPER_UNIT=${MHP3RD_CAMERA_HELPER_UNIT})
endif()
# The NID table is compiled in, so the program reads nothing from the checkout
# it was built in.
set(MHP3RD_NIDS_CSV "${PROJECT_SOURCE_DIR}/configs/nids.csv")
set(MHP3RD_NIDS_INC "${CMAKE_CURRENT_BINARY_DIR}/generated_nids/nid_table.inc")
add_custom_command(
    OUTPUT "${MHP3RD_NIDS_INC}"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${MHP3RD_NIDS_CSV} -DOUTPUT=${MHP3RD_NIDS_INC}
            -P "${MHP3RD_PROFILE_DIR}/tools/embed_nids.cmake"
    DEPENDS "${MHP3RD_NIDS_CSV}" "${MHP3RD_PROFILE_DIR}/tools/embed_nids.cmake"
    COMMENT "Embedding the NID table")
target_sources(Yakumo PRIVATE "${MHP3RD_NIDS_INC}")
target_include_directories(Yakumo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_nids")

# A release build runs from wherever it is unpacked: it has no fallback to
# this checkout's game directory, and on Linux it finds the libraries it ships
# in lib/ next to the executable and nothing else.
option(MHP3RD_RELEASE "Build for distribution: no checkout paths, libraries from lib/ next to the executable" OFF)
if(NOT MHP3RD_RELEASE)
    # The checkout's profiles/mhp3rd/game, a developer fallback after the
    # per-user data directory. Only main.cpp reads it; on the target it would be
    # on every generated unit's command line, so moving the checkout would
    # rebuild all of them and no other checkout could reuse their cache entries.
    set_property(SOURCE host/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        MHP3RD_DEFAULT_GAME_DIR="${MHP3RD_PROFILE_DIR}/game")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(Yakumo PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/lib")
    # The C++ runtime is linked in statically, so a build made against an old
    # glibc runs wherever that glibc does, and its symbols stay private: the
    # executable exports its own symbols to the overlay libraries, and a
    # second, exported libstdc++ would clash with the system's one that the
    # graphics driver loads.
    set(MHP3RD_RELEASE_LINK_OPTIONS -static-libstdc++ "LINKER:--exclude-libs,libstdc++.a:libsupc++.a")
    target_link_options(Yakumo PRIVATE ${MHP3RD_RELEASE_LINK_OPTIONS})
endif()
set_target_properties(Yakumo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    JOB_POOL_COMPILE psprecomp_generated)

# Native entry point for local user-operated test sessions; no game or SDL link.
# The same launcher is copied into both bundles by the candidate's packager.
if(APPLE AND NOT MHP3RD_BASELINE_B0)
    add_executable(mhp3rd_test_launcher tools/test_launcher.cpp
        "${PROJECT_SOURCE_DIR}/src/sha256.cpp")
    target_include_directories(mhp3rd_test_launcher PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(mhp3rd_test_launcher PRIVATE
        "-framework CoreFoundation" "-framework CoreServices")
    set_target_properties(mhp3rd_test_launcher PROPERTIES
        OUTPUT_NAME YakumoTestLauncher RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# Developer tools (host/debug): the menu's Debug page, with cheats for testing,
# and the command file that drives it from a script. Never in a release build:
# MHP3RD_RELEASE compiles all of it out whatever this option says. A developer
# build still shows it only when MHP3RD_DEBUG_MENU=1 is set at run time.
option(MHP3RD_DEBUG_MENU "Compile the developer tools (Debug page) into non-release builds" ON)
if(MHP3RD_DEBUG_MENU AND NOT MHP3RD_RELEASE)
    target_sources(Yakumo PRIVATE
        host/debug/debug_console.cpp
        host/debug/debug_tools.cpp
        host/debug/game_state.cpp)
    if(MHP3RD_RENDERER)
        target_sources(Yakumo PRIVATE host/ui/debug_screen.cpp)
    endif()
    target_compile_definitions(Yakumo PRIVATE MHP3RD_DEBUG_MENU=1)
    message(STATUS "mhp3rd: developer tools compiled in (shown with MHP3RD_DEBUG_MENU=1)")

    # The game's structures as the tools read and write them, on a buffer.
    add_executable(mhp3rd_debug_tools_tests
        tests/debug_tools_tests.cpp
        host/debug/game_state.cpp)
    target_include_directories(mhp3rd_debug_tools_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
    set_target_properties(mhp3rd_debug_tools_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(PSPRECOMP_BUILD_TESTS)
        add_test(NAME mhp3rd_debug_tools_tests COMMAND mhp3rd_debug_tools_tests)
    endif()
else()
    message(STATUS "mhp3rd: developer tools not compiled in")
endif()

# Shared observation/build settings deliberately exclude the gameplay revision
# and baseline seal, which are independently exposed by preflight.
string(SHA256 MHP3RD_BUILD_CONFIG_SHA256
    "paired-build-v1;${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_BUILD_TYPE};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_RELEASE};${CMAKE_CXX_FLAGS_DEBUG};${CMAKE_OSX_ARCHITECTURES};${CMAKE_OSX_DEPLOYMENT_TARGET};${MHP3RD_RENDERER};${MHP3RD_FFMPEG};${MHP3RD_LIBAV_libavcodec_VERSION};${MHP3RD_LIBAV_libavutil_VERSION};${MHP3RD_RELEASE};${MHP3RD_DEBUG_MENU};${MHP3RD_CERTIFIED_PROBES};${PSPRECOMP_GENERATED_OPT_LEVEL};${PSPRECOMP_AOT_PRODUCTION_FASTPATHS};${PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH};${PSPRECOMP_LTO}")
set_property(SOURCE host/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    MHP3RD_GAMEPLAY_SOURCE_COMMIT="${MHP3RD_EFFECTIVE_GAMEPLAY_COMMIT}"
    MHP3RD_BUILD_CONFIG_SHA256="${MHP3RD_BUILD_CONFIG_SHA256}"
    MHP3RD_BASELINE_PROVENANCE_SHA256="${MHP3RD_BASELINE_PROVENANCE_SHA256}")
if(MHP3RD_BASELINE_B0)
    set_property(SOURCE host/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS MHP3RD_BASELINE_B0=1)
endif()

# Offline byte comparison against the runtime's DATA.BIN implementation.
# The CLI requires a separately validated local ISO and prepared raw entries.
add_executable(mhp3rd_resource_crosscheck
    tools/resource_crosscheck.cpp
    host/mods/mhp3rd_data_bin.cpp
    host/mods/file_overlay.cpp
    host/mods/mod_ini.cpp)
target_include_directories(mhp3rd_resource_crosscheck PRIVATE "${MHP3RD_PROFILE_DIR}/host")
set_target_properties(mhp3rd_resource_crosscheck PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Synthetic preparation tests never require or launch the game.
if(PSPRECOMP_BUILD_TESTS)
    find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        foreach(suite baseline_registration resource_preparation offline_gate probe_instrumentation run_package run_cases run_comparison)
            add_test(NAME mhp3rd_${suite}_tests
                COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                    -s "${MHP3RD_PROFILE_DIR}/tests" -p "test_${suite}.py" -v)
            set_tests_properties(mhp3rd_${suite}_tests PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 60)
        endforeach()
        if(NOT MHP3RD_BASELINE_B0)
            foreach(suite observed_baseline package_test_pair test_launcher pair_readiness initial_case_pack)
                add_test(NAME mhp3rd_${suite}_tests
                    COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                        -s "${MHP3RD_PROFILE_DIR}/tests" -p "test_${suite}.py" -v)
                set_tests_properties(mhp3rd_${suite}_tests PROPERTIES
                    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 60)
            endforeach()
            add_test(NAME mhp3rd_test_supervisor_tests
                COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                    -s "${PROJECT_SOURCE_DIR}/tests" -p "test_test_supervisor.py" -v)
            set_tests_properties(mhp3rd_test_supervisor_tests PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 60)
        endif()
    endif()
endif()

add_executable(mhp3rd_iso_image_tests
    tests/iso_image_tests.cpp
    host/kernel/iso_image.cpp)
target_include_directories(mhp3rd_iso_image_tests PRIVATE
    "${MHP3RD_PROFILE_DIR}/host" "${CMAKE_SOURCE_DIR}/include")
set_target_properties(mhp3rd_iso_image_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(mhp3rd_psmf_demuxer_tests
    tests/psmf_demuxer_tests.cpp
    host/movie/psmf_demuxer.cpp)
target_include_directories(mhp3rd_psmf_demuxer_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
set_target_properties(mhp3rd_psmf_demuxer_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_iso_image_tests COMMAND mhp3rd_iso_image_tests)
    add_test(NAME mhp3rd_psmf_demuxer_tests COMMAND mhp3rd_psmf_demuxer_tests)
endif()

# Save-data checks that need no game data: crypto test vectors and round trips.
add_executable(mhp3rd_savedata_tests
    tests/savedata_tests.cpp
    host/save_data/aes128.cpp
    host/save_data/param_sfo.cpp
    host/save_data/savedata_crypto.cpp
    host/save_data/savedata_store.cpp
    host/save_data/save_transfer.cpp
    third_party/tiny_aes/aes.c)
target_include_directories(mhp3rd_savedata_tests PRIVATE
    "${MHP3RD_PROFILE_DIR}/host"
    "${MHP3RD_PROFILE_DIR}/third_party")
set_target_properties(mhp3rd_savedata_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_savedata_tests COMMAND mhp3rd_savedata_tests)
endif()

# Native leaf experiments: ordinary examples need no game data. Passing the
# user's ELF enables exhaustive differential checks against its instructions.
if(NOT MHP3RD_BASELINE_B0)
add_executable(mhp3rd_native_angle_tests
    tests/native_angle_tests.cpp
    host/native/angle_step.cpp
    host/native/angle_step_bridge.cpp)
target_include_directories(mhp3rd_native_angle_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_native_angle_tests PRIVATE psprecomp_core)
set_target_properties(mhp3rd_native_angle_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_native_angle_tests COMMAND mhp3rd_native_angle_tests)
endif()

add_executable(mhp3rd_native_scale_tests
    tests/native_scale_tests.cpp
    host/native/scale_matrix.cpp
    host/native/scale_matrix_bridge.cpp)
target_include_directories(mhp3rd_native_scale_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_native_scale_tests PRIVATE psprecomp_core)
set_target_properties(mhp3rd_native_scale_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_native_scale_tests COMMAND mhp3rd_native_scale_tests)
endif()

add_executable(mhp3rd_native_translation_matrix_tests
    tests/native_translation_matrix_tests.cpp
    host/native/translation_matrix.cpp
    host/native/translation_matrix_bridge.cpp)
target_include_directories(mhp3rd_native_translation_matrix_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_native_translation_matrix_tests PRIVATE psprecomp_core)
set_target_properties(mhp3rd_native_translation_matrix_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_native_translation_matrix_tests COMMAND mhp3rd_native_translation_matrix_tests)
endif()

add_executable(mhp3rd_native_vector_construct_tests
    tests/native_vector_construct_tests.cpp
    host/native/vector_construct.cpp
    host/native/vector_construct_bridge.cpp)
target_include_directories(mhp3rd_native_vector_construct_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_native_vector_construct_tests PRIVATE psprecomp_core)
set_target_properties(mhp3rd_native_vector_construct_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_native_vector_construct_tests COMMAND mhp3rd_native_vector_construct_tests)
endif()

add_executable(mhp3rd_native_matrix_copy_tests
    tests/native_matrix_copy_tests.cpp
    host/native/matrix_copy.cpp
    host/native/matrix_copy_bridge.cpp)
target_include_directories(mhp3rd_native_matrix_copy_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_native_matrix_copy_tests PRIVATE psprecomp_core)
set_target_properties(mhp3rd_native_matrix_copy_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_native_matrix_copy_tests COMMAND mhp3rd_native_matrix_copy_tests)
endif()
endif()

# Shared observational journal; no PSP or renderer dependency.
add_library(mhp3rd_session_recording STATIC
    host/testing/journal.cpp
    host/testing/session_recorder.cpp)
target_include_directories(mhp3rd_session_recording PUBLIC "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_session_recording PUBLIC Threads::Threads)
add_library(mhp3rd_game_observation STATIC
    host/testing/game_observers.cpp
    host/testing/runtime_recording.cpp)
file(GLOB MHP3RD_RECORDING_REVISION_INPUTS CONFIGURE_DEPENDS
    "${MHP3RD_PROFILE_DIR}/host/testing/*.cpp" "${MHP3RD_PROFILE_DIR}/host/testing/*.hpp")
list(APPEND MHP3RD_RECORDING_REVISION_INPUTS
    "${MHP3RD_PROFILE_DIR}/tools/instrument_probes.py"
    "${MHP3RD_PROFILE_DIR}/host/hle/control_delivery.hpp"
    "${MHP3RD_PROFILE_DIR}/host/hle/control_delivery.cpp"
    "${MHP3RD_PROFILE_DIR}/host/settings/settings.cpp"
    "${MHP3RD_PROFILE_DIR}/host/settings/settings.hpp"
    "${MHP3RD_PROFILE_DIR}/host/ui/test_session_screen.cpp"
    "${MHP3RD_PROFILE_DIR}/host/ui/test_session_screen.hpp"
    "${MHP3RD_PROFILE_DIR}/host/ui/translations/zh_cn.inc")
set(MHP3RD_RECORDING_DIGESTS "")
foreach(source IN LISTS MHP3RD_RECORDING_REVISION_INPUTS)
    file(SHA256 "${source}" digest)
    string(APPEND MHP3RD_RECORDING_DIGESTS "${digest};")
endforeach()
string(SHA256 MHP3RD_RECORDING_REVISION "${MHP3RD_RECORDING_DIGESTS}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MHP3RD_RECORDING_REVISION_INPUTS})
target_compile_definitions(mhp3rd_game_observation PRIVATE
    MHP3RD_RECORDING_REVISION="source-sha256:${MHP3RD_RECORDING_REVISION}")
target_link_libraries(mhp3rd_game_observation PUBLIC mhp3rd_session_recording)
if(NOT MHP3RD_BASELINE_B0)
    # Uses the production glyph rasterizer without starting SDL or the game.
    add_executable(mhp3rd_game_font_coverage tests/game_font_coverage.cpp
        host/fonts/game_font.cpp host/app_paths.cpp host/install/user_data.cpp
        host/settings/settings.cpp host/input/bindings.cpp)
    target_include_directories(mhp3rd_game_font_coverage PRIVATE
        "${MHP3RD_PROFILE_DIR}/host" "${MHP3RD_PROFILE_DIR}/third_party")
    target_link_libraries(mhp3rd_game_font_coverage PRIVATE psprecomp_core mhp3rd_game_observation)
    set_target_properties(mhp3rd_game_font_coverage PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
add_executable(mhp3rd_run_package_fixture tests/run_package_fixture.cpp)
target_link_libraries(mhp3rd_run_package_fixture PRIVATE mhp3rd_game_observation)
set_target_properties(mhp3rd_run_package_fixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME mhp3rd_run_pipeline_tests COMMAND "${Python3_EXECUTABLE}"
        "${MHP3RD_PROFILE_DIR}/tests/test_run_pipeline.py" "$<TARGET_FILE:mhp3rd_run_package_fixture>")
    set_tests_properties(mhp3rd_run_pipeline_tests PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 60)
endif()
add_library(mhp3rd_overlay_observation STATIC host/testing/overlay_observation.cpp)
target_include_directories(mhp3rd_overlay_observation PUBLIC "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_overlay_observation PUBLIC psprecomp_core)
add_library(mhp3rd_native_probes STATIC host/testing/probes.cpp)
target_link_libraries(mhp3rd_native_probes PUBLIC psprecomp_core mhp3rd_game_observation)
add_library(mhp3rd_state_observation STATIC host/testing/state_observation.cpp)
target_link_libraries(mhp3rd_state_observation PUBLIC mhp3rd_overlay_observation mhp3rd_game_observation)
add_library(mhp3rd_runtime_diagnostics STATIC host/testing/runtime_diagnostics.cpp)
target_link_libraries(mhp3rd_runtime_diagnostics PUBLIC mhp3rd_native_probes mhp3rd_state_observation)
add_library(mhp3rd_case_catalog STATIC host/testing/case_catalog.cpp)
target_include_directories(mhp3rd_case_catalog PUBLIC "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_case_catalog PUBLIC psprecomp_core)
add_library(mhp3rd_case_controller STATIC host/testing/case_controller.cpp)
target_link_libraries(mhp3rd_case_controller PUBLIC mhp3rd_case_catalog mhp3rd_native_probes)
add_library(mhp3rd_case_runtime STATIC host/testing/case_runtime.cpp)
target_link_libraries(mhp3rd_case_runtime PUBLIC mhp3rd_case_controller mhp3rd_runtime_diagnostics)
target_link_libraries(Yakumo PRIVATE mhp3rd_case_runtime)
add_executable(mhp3rd_case_runtime_fixture tests/case_runtime_fixture.cpp
    host/settings/settings.cpp host/install/user_data.cpp host/input/bindings.cpp)
target_link_libraries(mhp3rd_case_runtime_fixture PRIVATE mhp3rd_case_runtime)
set_target_properties(mhp3rd_case_runtime_fixture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME mhp3rd_case_runtime_pipeline_tests COMMAND "${Python3_EXECUTABLE}"
        "${MHP3RD_PROFILE_DIR}/tests/test_case_runtime_pipeline.py" "$<TARGET_FILE:mhp3rd_case_runtime_fixture>")
    set_tests_properties(mhp3rd_case_runtime_pipeline_tests PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 60)
    if(NOT MHP3RD_BASELINE_B0)
        add_test(NAME mhp3rd_initial_case_pipeline_tests COMMAND "${Python3_EXECUTABLE}"
            "${MHP3RD_PROFILE_DIR}/tests/test_initial_case_pipeline.py"
            "$<TARGET_FILE:mhp3rd_case_runtime_fixture>"
            "${MHP3RD_PROFILE_DIR}/testing/cases/initial_batch.json")
        set_tests_properties(mhp3rd_initial_case_pipeline_tests PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 60)
    endif()
endif()
foreach(suite case_catalog case_controller)
    add_executable(mhp3rd_${suite}_tests tests/${suite}_tests.cpp)
    target_link_libraries(mhp3rd_${suite}_tests PRIVATE mhp3rd_${suite})
    set_target_properties(mhp3rd_${suite}_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(PSPRECOMP_BUILD_TESTS)
        add_test(NAME mhp3rd_${suite}_tests COMMAND mhp3rd_${suite}_tests)
        set_tests_properties(mhp3rd_${suite}_tests PROPERTIES TIMEOUT 30)
    endif()
endforeach()
add_executable(mhp3rd_test_session_screen_tests tests/test_session_screen_tests.cpp
    host/ui/test_session_screen.cpp host/ui/localization.cpp
    host/settings/settings.cpp host/install/user_data.cpp host/input/bindings.cpp
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp)
target_include_directories(mhp3rd_test_session_screen_tests PRIVATE "${MHP3RD_PROFILE_DIR}/third_party/imgui")
target_link_libraries(mhp3rd_test_session_screen_tests PRIVATE mhp3rd_case_controller)
set_target_properties(mhp3rd_test_session_screen_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_test_session_screen_tests COMMAND mhp3rd_test_session_screen_tests)
    set_tests_properties(mhp3rd_test_session_screen_tests PROPERTIES TIMEOUT 30)
    if(NOT MHP3RD_BASELINE_B0)
        add_test(NAME mhp3rd_initial_case_screen_tests COMMAND mhp3rd_test_session_screen_tests
            "${MHP3RD_PROFILE_DIR}/testing/cases/initial_batch.json")
        set_tests_properties(mhp3rd_initial_case_screen_tests PROPERTIES TIMEOUT 30)
    endif()
endif()
if(MHP3RD_PROBE_INSTRUMENTED_SOURCES)
    target_compile_definitions(Yakumo PRIVATE MHP3RD_CERTIFIED_AOT_PROBES=1)
endif()
foreach(leaf angle scale translation_matrix vector_construct matrix_copy)
    if(TARGET mhp3rd_native_${leaf}_tests)
        target_link_libraries(mhp3rd_native_${leaf}_tests PRIVATE mhp3rd_native_probes)
    endif()
endforeach()
foreach(suite probe state_observation runtime_diagnostics)
    add_executable(mhp3rd_${suite}_tests tests/${suite}_tests.cpp)
    target_link_libraries(mhp3rd_${suite}_tests PRIVATE mhp3rd_runtime_diagnostics)
    set_target_properties(mhp3rd_${suite}_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(PSPRECOMP_BUILD_TESTS)
        add_test(NAME mhp3rd_${suite}_tests COMMAND mhp3rd_${suite}_tests)
        set_tests_properties(mhp3rd_${suite}_tests PROPERTIES TIMEOUT 30)
    endif()
endforeach()
if(MHP3RD_PROBE_INSTRUMENTED_SOURCES)
    # Reuses the production objects. The optional local ELF test executes only
    # bounded leaf calls; it never starts the game, kernel, renderer, or audio.
    add_executable(mhp3rd_aot_probe_tests EXCLUDE_FROM_ALL tests/aot_probe_tests.cpp $<TARGET_OBJECTS:mhp3rd_generated>)
    if(MHP3RD_BASELINE_B0)
        target_compile_definitions(mhp3rd_aot_probe_tests PRIVATE MHP3RD_BASELINE_B0=1)
    else()
        target_sources(mhp3rd_aot_probe_tests PRIVATE
            host/native/angle_step.cpp host/native/angle_step_bridge.cpp
            host/native/scale_matrix.cpp host/native/scale_matrix_bridge.cpp
            host/native/translation_matrix.cpp host/native/translation_matrix_bridge.cpp
            host/native/vector_construct.cpp host/native/vector_construct_bridge.cpp
            host/native/matrix_copy.cpp host/native/matrix_copy_bridge.cpp)
    endif()
    target_include_directories(mhp3rd_aot_probe_tests PRIVATE "${MHP3RD_PROFILE_DIR}/generated")
    target_link_libraries(mhp3rd_aot_probe_tests PRIVATE mhp3rd_native_probes)
    set_target_properties(mhp3rd_aot_probe_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
add_library(mhp3rd_control_delivery STATIC host/hle/control_delivery.cpp)
target_link_libraries(mhp3rd_control_delivery PUBLIC psprecomp_core mhp3rd_game_observation)
add_executable(mhp3rd_control_delivery_tests tests/control_delivery_tests.cpp)
target_link_libraries(mhp3rd_control_delivery_tests PRIVATE mhp3rd_control_delivery)
target_link_libraries(Yakumo PRIVATE mhp3rd_control_delivery)
add_executable(mhp3rd_overlay_observation_tests tests/overlay_observation_tests.cpp)
target_link_libraries(mhp3rd_overlay_observation_tests PRIVATE mhp3rd_overlay_observation)
add_executable(mhp3rd_observation_hook_tests tests/observation_hook_tests.cpp
    host/camera/camera_input.cpp host/settings/settings.cpp host/install/user_data.cpp host/input/bindings.cpp)
target_link_libraries(mhp3rd_observation_hook_tests PRIVATE mhp3rd_game_observation psprecomp_core)
foreach(suite overlay_observation observation_hook control_delivery)
    set_target_properties(mhp3rd_${suite}_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(PSPRECOMP_BUILD_TESTS)
        add_test(NAME mhp3rd_${suite}_tests COMMAND mhp3rd_${suite}_tests)
        set_tests_properties(mhp3rd_${suite}_tests PROPERTIES TIMEOUT 30)
    endif()
endforeach()
foreach(suite game_observer runtime_recording)
    add_executable(mhp3rd_${suite}_tests tests/${suite}_tests.cpp)
    target_link_libraries(mhp3rd_${suite}_tests PRIVATE mhp3rd_game_observation)
    set_target_properties(mhp3rd_${suite}_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(PSPRECOMP_BUILD_TESTS)
        add_test(NAME mhp3rd_${suite}_tests COMMAND mhp3rd_${suite}_tests)
        set_tests_properties(mhp3rd_${suite}_tests PROPERTIES TIMEOUT 30)
    endif()
endforeach()
if(MHP3RD_RENDERER)
    add_executable(mhp3rd_sdl_observer_tests tests/sdl_observer_tests.cpp host/testing/sdl_observers.cpp)
    target_link_libraries(mhp3rd_sdl_observer_tests PRIVATE mhp3rd_game_observation SDL3::SDL3)
    set_target_properties(mhp3rd_sdl_observer_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(PSPRECOMP_BUILD_TESTS)
        add_test(NAME mhp3rd_sdl_observer_tests COMMAND mhp3rd_sdl_observer_tests)
        set_tests_properties(mhp3rd_sdl_observer_tests PROPERTIES TIMEOUT 30)
    endif()
endif()
add_executable(mhp3rd_session_recorder_tests tests/session_recorder_tests.cpp)
target_link_libraries(mhp3rd_session_recorder_tests PRIVATE mhp3rd_session_recording)
set_target_properties(mhp3rd_session_recorder_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_session_recorder_tests COMMAND mhp3rd_session_recorder_tests)
    set_tests_properties(mhp3rd_session_recorder_tests PROPERTIES TIMEOUT 30)
    if(Python3_Interpreter_FOUND)
        add_test(NAME mhp3rd_session_process_tests
            COMMAND "${Python3_EXECUTABLE}" "${MHP3RD_PROFILE_DIR}/tests/test_session_process.py"
                "$<TARGET_FILE:mhp3rd_session_recorder_tests>")
        set_tests_properties(mhp3rd_session_process_tests PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 30)
    endif()
endif()

# Camera dispatch, ownership and integration checks without game data.
add_executable(mhp3rd_camera_tests
    tests/analog_camera_tests.cpp
    host/camera/camera_input.cpp
    host/camera/game_camera.cpp
    host/input/bindings.cpp)
target_include_directories(mhp3rd_camera_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_camera_tests PRIVATE psprecomp_core mhp3rd_game_observation)
set_target_properties(mhp3rd_camera_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_camera_tests COMMAND mhp3rd_camera_tests)
endif()

# The game's view shape: what is written to guest memory, and what is left.
add_executable(mhp3rd_aspect_tests
    tests/game_aspect_tests.cpp
    host/camera/game_aspect.cpp)
target_include_directories(mhp3rd_aspect_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_aspect_tests PRIVATE psprecomp_core)
set_target_properties(mhp3rd_aspect_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_aspect_tests COMMAND mhp3rd_aspect_tests)
endif()

# Each platform's settings defaults.
add_executable(mhp3rd_settings_tests
    tests/settings_defaults_tests.cpp
    host/settings/settings.cpp
    host/install/user_data.cpp
    host/input/bindings.cpp)
target_include_directories(mhp3rd_settings_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_settings_tests PRIVATE psprecomp_core mhp3rd_game_observation)
set_target_properties(mhp3rd_settings_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_settings_tests COMMAND mhp3rd_settings_tests)
endif()

# Interface localization and settings persistence do not need game data or a GPU.
if(MSVC)
    # The catalog and system font paths must remain UTF-8 on every Windows locale.
    set_source_files_properties(host/ui/localization.cpp host/ui/fonts.cpp tests/ui_localization_tests.cpp
        PROPERTIES COMPILE_OPTIONS "/utf-8")
endif()
add_executable(mhp3rd_ui_localization_tests
    tests/ui_localization_tests.cpp
    host/ui/localization.cpp
    host/settings/settings.cpp
    host/install/user_data.cpp
    host/input/bindings.cpp)
target_include_directories(mhp3rd_ui_localization_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_ui_localization_tests PRIVATE psprecomp_core mhp3rd_game_observation)
set_target_properties(mhp3rd_ui_localization_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    foreach(mode default saved override invalid)
        add_test(NAME mhp3rd_ui_localization_${mode} COMMAND mhp3rd_ui_localization_tests ${mode})
    endforeach()
endif()

# Fast loading's decision: when a load runs ahead of real time.
add_executable(mhp3rd_fast_loading_tests
    tests/fast_loading_tests.cpp
    host/kernel/load_detector.cpp)
target_include_directories(mhp3rd_fast_loading_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
set_target_properties(mhp3rd_fast_loading_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_fast_loading_tests COMMAND mhp3rd_fast_loading_tests)
endif()

# The on-screen touch controls: layout, fingers and the stick's maths.
add_executable(mhp3rd_touch_tests
    tests/touch_controls_tests.cpp
    host/input/touch_controls.cpp)
target_include_directories(mhp3rd_touch_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
set_target_properties(mhp3rd_touch_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_touch_tests COMMAND mhp3rd_touch_tests)
endif()

# Texture pack import: finding a pack in the layouts packs come in, the
# checks made before copying, and the copy, swap and backup; and the PSP's DXT
# block layout. No game data.
add_executable(mhp3rd_texture_pack_tests
    tests/texture_pack_tests.cpp
    host/gpu/texture_decode.cpp
    host/gpu/texture_pack.cpp
    host/gpu/texture_pack_import.cpp)
target_include_directories(mhp3rd_texture_pack_tests PRIVATE
    "${MHP3RD_PROFILE_DIR}/host"
    "${MHP3RD_PROFILE_DIR}/host/gpu"
    "${MHP3RD_PROFILE_DIR}/third_party")
target_link_libraries(mhp3rd_texture_pack_tests PRIVATE psprecomp_core Threads::Threads)
set_target_properties(mhp3rd_texture_pack_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_texture_pack_tests COMMAND mhp3rd_texture_pack_tests)
endif()

# Keyboard and mouse bindings: names, settings.ini spelling, the menu's
# assignment rules and what held keys press. No SDL or game data.
add_executable(mhp3rd_input_tests
    tests/input_tests.cpp
    host/input/bindings.cpp)
target_include_directories(mhp3rd_input_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
set_target_properties(mhp3rd_input_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_input_tests COMMAND mhp3rd_input_tests)
endif()

# Mods: the community's mod folders, choices, conflicts and import, the
# DATA.BIN obfuscation and an archive served with mods' files. No game data.
add_executable(mhp3rd_mods_tests
    tests/mods_tests.cpp
    host/mods/file_overlay.cpp
    host/mods/mhp3rd_data_bin.cpp
    host/mods/mhp3rd_mod_format.cpp
    host/mods/mod_import.cpp
    host/mods/mod_ini.cpp
    host/mods/mod_library.cpp
    host/mods/mod_session.cpp)
target_include_directories(mhp3rd_mods_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
set_target_properties(mhp3rd_mods_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_mods_tests COMMAND mhp3rd_mods_tests)
endif()

# Renderer helpers that need no GPU.
add_executable(mhp3rd_render_tests tests/render_tests.cpp)
target_include_directories(mhp3rd_render_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_render_tests PRIVATE psprecomp_core)
set_target_properties(mhp3rd_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_render_tests COMMAND mhp3rd_render_tests)
endif()

# Frame interpolation checks: matching draws, cuts, blending, the present
# clock and the frame rate governor.
add_executable(mhp3rd_interpolation_tests
    tests/interpolation_tests.cpp
    host/gpu/frame_interpolation.cpp
    host/gpu/frame_pacing.cpp)
target_include_directories(mhp3rd_interpolation_tests PRIVATE
    "${MHP3RD_PROFILE_DIR}/host"
    "${PROJECT_SOURCE_DIR}/include")
set_target_properties(mhp3rd_interpolation_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_interpolation_tests COMMAND mhp3rd_interpolation_tests)
endif()

# The built-in ad hoc server against the client and a raw protocol peer, and
# local network discovery, all over the loopback address.
add_executable(mhp3rd_adhoc_tests
    tests/adhoc_tests.cpp
    host/adhoc/client.cpp
    host/adhoc/discovery.cpp
    host/adhoc/server.cpp)
target_include_directories(mhp3rd_adhoc_tests PRIVATE "${MHP3RD_PROFILE_DIR}/host")
target_link_libraries(mhp3rd_adhoc_tests PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(mhp3rd_adhoc_tests PRIVATE ws2_32 iphlpapi)
endif()
set_target_properties(mhp3rd_adhoc_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME mhp3rd_adhoc_tests COMMAND mhp3rd_adhoc_tests)
endif()

# Chained AOT calls nest native frames; give the guest-executing main thread
# the same 64 MiB stack the Windows profiles use.
if(MSVC)
    target_link_options(Yakumo PRIVATE /STACK:67108864)
elseif(APPLE)
    target_link_options(Yakumo PRIVATE "LINKER:-stack_size,0x4000000")
endif()

# Recompiled overlay corpora, produced by tools/add_overlay.py. Each directory
# under overlays/ holds one corpus and becomes one shared module that the host
# loads at run time, so a new overlay never relinks the executable.
set(MHP3RD_OVERLAY_DIR "${CMAKE_BINARY_DIR}/bin/overlays")

function(mhp3rd_add_overlay meta_path)
    get_filename_component(source_dir "${meta_path}" DIRECTORY)
    get_filename_component(MHP3RD_OVERLAY_PREFIX "${source_dir}" NAME)
    file(STRINGS "${meta_path}" meta_lines REGEX "^(base|size|code_size|hash|name)=")
    foreach(line IN LISTS meta_lines)
        string(REGEX MATCH "^([a-z_]+)=(.+)$" matched "${line}")
        if(NOT matched)
            continue()
        endif()
        string(TOUPPER "${CMAKE_MATCH_1}" key)
        set(MHP3RD_OVERLAY_${key} "${CMAKE_MATCH_2}")
    endforeach()
    if(NOT MHP3RD_OVERLAY_BASE OR NOT MHP3RD_OVERLAY_SIZE OR NOT MHP3RD_OVERLAY_CODE_SIZE
       OR NOT MHP3RD_OVERLAY_HASH OR NOT MHP3RD_OVERLAY_NAME)
        message(WARNING "mhp3rd: incomplete overlay metadata in ${meta_path}")
        return()
    endif()

    file(GLOB sources CONFIGURE_DEPENDS "${source_dir}/*.cpp")
    if(NOT sources)
        message(WARNING "mhp3rd: no sources for overlay ${MHP3RD_OVERLAY_PREFIX}")
        return()
    endif()
    set(entry_point "${CMAKE_CURRENT_BINARY_DIR}/overlays/${MHP3RD_OVERLAY_PREFIX}_module.cpp")
    configure_file("${MHP3RD_PROFILE_DIR}/host/overlay_module.cpp.in" "${entry_point}" @ONLY)

    set(target "overlay_${MHP3RD_OVERLAY_PREFIX}")
    add_library(${target} MODULE ${sources} "${entry_point}")
    # Headers only: linking psprecomp_core would give the module its own copy of
    # the runtime state the host already owns.
    target_include_directories(${target} PRIVATE
        "${source_dir}" "${MHP3RD_PROFILE_DIR}/host" "${PROJECT_SOURCE_DIR}/include")
    if(WIN32 OR APPLE OR MHP3RD_ANDROID_APP)
        # Both linkers want the host binary while linking the module: MSVC for
        # its import library, ld64 as the bundle loader. ELF leaves the host
        # symbols undefined and resolves them when the module is loaded. In an
        # Android app the host is libmain.so, which the app loads privately,
        # so the module names it as a dependency to find its symbols.
        target_link_libraries(${target} PRIVATE Yakumo)
    else()
        add_dependencies(${target} Yakumo)
        if(ANDROID)
            # The NDK links every shared library with --no-undefined; these
            # leave the host's symbols to the loader like any other ELF.
            target_link_options(${target} PRIVATE "LINKER:-z,undefs")
        endif()
    endif()
    if(MSVC)
        target_compile_definitions(${target} PRIVATE PSPRECOMP_IMPORT_HOST_SYMBOLS=1)
        if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
            target_compile_options(${target} PRIVATE /Od /bigobj)
        else()
            target_compile_options(${target} PRIVATE /O${PSPRECOMP_GENERATED_OPT_LEVEL} /Ob0 /bigobj)
        endif()
    else()
        target_compile_options(${target} PRIVATE -O${PSPRECOMP_GENERATED_OPT_LEVEL} -g0)
    endif()
    if(MHP3RD_RELEASE_LINK_OPTIONS)
        target_link_options(${target} PRIVATE ${MHP3RD_RELEASE_LINK_OPTIONS})
    endif()
    set_target_properties(${target} PROPERTIES
        JOB_POOL_COMPILE psprecomp_generated
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        PREFIX ""
        SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
        OUTPUT_NAME "${MHP3RD_OVERLAY_PREFIX}"
        LIBRARY_OUTPUT_DIRECTORY "${MHP3RD_OVERLAY_DIR}")
    if(MHP3RD_ANDROID_APP)
        # An APK only carries libraries named lib*.so.
        set_target_properties(${target} PROPERTIES PREFIX "lib")
    endif()
endfunction()

file(GLOB MHP3RD_OVERLAY_METADATA CONFIGURE_DEPENDS "${MHP3RD_PROFILE_DIR}/overlays/*/meta.txt")
foreach(meta IN LISTS MHP3RD_OVERLAY_METADATA)
    mhp3rd_add_overlay("${meta}")
endforeach()
list(LENGTH MHP3RD_OVERLAY_METADATA MHP3RD_OVERLAY_COUNT)
message(STATUS "mhp3rd: ${MHP3RD_OVERLAY_COUNT} overlay librar(ies) in ${MHP3RD_OVERLAY_DIR}")
