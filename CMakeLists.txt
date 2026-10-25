# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_OSX_DEPLOYMENT_TARGET) # must be set before project(); ignored outside macOS
    # The plugin itself could go lower, but the public link needs cloudflared, which
    # needs macOS 13 or later (it is built with Go 1.27): no point installing it earlier.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

project(StudioRider VERSION 1.0.0 LANGUAGES C CXX)

option(STUDIORIDER_BUILD_PLUGIN  "Build the plugin (needs JUCE)" ON)
option(STUDIORIDER_BUILD_HARNESS "Build the test tool (no JUCE)" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

find_package(Threads REQUIRED)

# --- civetweb: HTTP/WebSocket server (MIT) -----------------------------------
# Built without SSL, without CGI and without any file system access:
# it can only serve what the plugin explicitly hands it.
add_library(studiorider_civetweb STATIC third_party/civetweb/src/civetweb.c)
target_include_directories(studiorider_civetweb PUBLIC third_party/civetweb/include
                                               PRIVATE Source/civetweb_hooks)
target_compile_definitions(studiorider_civetweb PUBLIC CIVETWEB_API=) # nothing is exported from the plugin
target_compile_definitions(studiorider_civetweb PRIVATE
    NO_SSL NO_CGI NO_FILES NO_FILESYSTEMS NO_CACHING USE_WEBSOCKET
    MG_EXTERNAL_FUNCTION_mg_cry_internal_impl MG_EXTERNAL_FUNCTION_log_access)
target_compile_options(studiorider_civetweb PRIVATE -w)
target_link_libraries(studiorider_civetweb PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# --- Codecs: libFLAC (BSD), libopus (BSD), speexdsp resampler (BSD) ----------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(_opt BUILD_CXXLIBS BUILD_PROGRAMS BUILD_EXAMPLES BUILD_TESTING BUILD_DOCS BUILD_UTILS
             INSTALL_MANPAGES INSTALL_PKGCONFIG_MODULES INSTALL_CMAKE_CONFIG_MODULE WITH_OGG
             OPUS_BUILD_TESTING OPUS_BUILD_PROGRAMS OPUS_INSTALL_PKG_CONFIG_MODULE
             OPUS_INSTALL_CMAKE_CONFIG_MODULE OPUS_DRED OPUS_OSCE OPUS_DEEP_PLC)
    set(${_opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
    # Universal build (Apple Silicon + Intel in one binary): Opus can't pick CPU-specific
    # code for two architectures at once, so it uses its portable C code. libFLAC does
    # the same on its own. Plenty fast for one stream.
    set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
    # libFLAC detects intrinsics by test-compiling headers: make sure it never picks up
    # x86 or ARM-only code for a binary that holds both.
    foreach(_v FLAC__HAS_X86INTRIN FLAC__HAS_NEONINTRIN FLAC__HAS_A64NEONINTRIN HAVE_CPUID_H)
        set(${_v} 0 CACHE INTERNAL "")
    endforeach()
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    # Apple Silicon Macs always have NEON: use it directly (same configuration
    # as Opus on iOS). OPUS_MAY_HAVE_NEON must stay on, otherwise the NEON
    # functions are not declared.
    set(OPUS_PRESUME_NEON ON CACHE BOOL "" FORCE)
    set(OPUS_MAY_HAVE_NEON ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(third_party/flac EXCLUDE_FROM_ALL)
add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)
target_compile_definitions(opus PRIVATE OPUS_EXPORT=)

add_library(studiorider_resampler STATIC third_party/speexdsp/resample.c)
target_include_directories(studiorider_resampler PUBLIC third_party/speexdsp)
target_compile_definitions(studiorider_resampler PUBLIC OUTSIDE_SPEEX RANDOM_PREFIX=studiorider_speex
                                              PRIVATE FLOATING_POINT EXPORT=)
target_compile_options(studiorider_resampler PRIVATE -w)

# --- Built-in listening page ---------------------------------------------------
set(STUDIORIDER_WEB_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
    ${CMAKE_CURRENT_SOURCE_DIR}/web/app.js
    ${CMAKE_CURRENT_SOURCE_DIR}/web/worklet.js
    ${CMAKE_CURRENT_SOURCE_DIR}/web/style.css
    ${CMAKE_CURRENT_SOURCE_DIR}/web/flac-decoder.min.js
    ${CMAKE_CURRENT_SOURCE_DIR}/web/opus-decoder.min.js
    ${CMAKE_CURRENT_SOURCE_DIR}/web/logo.png)
set(STUDIORIDER_WEB_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/WebAssets.cpp)
add_custom_command(
    OUTPUT ${STUDIORIDER_WEB_CPP}
    COMMAND ${CMAKE_COMMAND} -DWEB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/web -DOUT=${STUDIORIDER_WEB_CPP}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedWebAssets.cmake
    DEPENDS ${STUDIORIDER_WEB_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedWebAssets.cmake
    COMMENT "Embedding the listening page")

# --- Network core (no JUCE) ----------------------------------------------------
add_library(studiorider_core STATIC Source/StreamServer.cpp Source/Encoders.cpp ${STUDIORIDER_WEB_CPP})
target_include_directories(studiorider_core PUBLIC Source)
target_link_libraries(studiorider_core PUBLIC studiorider_civetweb PRIVATE FLAC::FLAC opus studiorider_resampler)

if(STUDIORIDER_BUILD_HARNESS)
    add_executable(studiorider_harness tests/harness.cpp)
    target_link_libraries(studiorider_harness PRIVATE studiorider_core)
endif()

# --- JUCE plugin ---------------------------------------------------------------
if(STUDIORIDER_BUILD_PLUGIN)
    set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to JUCE")
    if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "JUCE not found in ${JUCE_DIR} (run build.sh, which downloads it).")
    endif()
    add_subdirectory(${JUCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/JUCE)

    if(APPLE)
        set(STUDIORIDER_FORMATS AU VST3 Standalone)
    else()
        set(STUDIORIDER_FORMATS VST3 Standalone)
    endif()

    juce_add_plugin(StudioRider
        PRODUCT_NAME "StudioRider"
        COMPANY_NAME "StudioRider"
        BUNDLE_ID "net.studiorider.plugin"
        VERSION ${PROJECT_VERSION}
        # The AU/VST3 codes are kept from the former names (MixShare, then StudioWire) so that existing
        # DAW projects still find the plugin.
        PLUGIN_MANUFACTURER_CODE Mxsr
        PLUGIN_CODE Mxs1
        FORMATS ${STUDIORIDER_FORMATS}
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        AU_MAIN_TYPE kAudioUnitType_Effect
        VST3_CATEGORIES Fx Tools Network
        DESCRIPTION "Streams the master to a web browser, lossless or Opus")

    # The logo shown in the plugin window (BinaryData::logo_png).
    juce_add_binary_data(StudioRiderAssets SOURCES assets/logo.png)
    set_target_properties(StudioRiderAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

    target_sources(StudioRider PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/Tunnel.cpp
        Source/Cloudflared.cpp)

    target_compile_definitions(StudioRider PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_USE_FLAC=0
        JUCE_USE_OGGVORBIS=0)

    target_link_libraries(StudioRider
        PRIVATE
            studiorider_core
            StudioRiderAssets
            juce::juce_audio_utils
            juce::juce_cryptography
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()

# --- Tunnel launcher test (JUCE core only) --------------------------------------
option(STUDIORIDER_BUILD_TUNNEL_TEST "Build the tunnel test" OFF)
if(STUDIORIDER_BUILD_TUNNEL_TEST AND STUDIORIDER_BUILD_PLUGIN)
    juce_add_console_app(studiorider_tunnel_test PRODUCT_NAME "studiorider_tunnel_test")
    target_sources(studiorider_tunnel_test PRIVATE tests/tunnel_test.cpp Source/Tunnel.cpp)
    target_include_directories(studiorider_tunnel_test PRIVATE Source)
    target_compile_definitions(studiorider_tunnel_test PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
    target_link_libraries(studiorider_tunnel_test PRIVATE juce::juce_core PUBLIC juce::juce_recommended_config_flags)
endif()

# --- cloudflared finder/updater test (JUCE core only, uses a local fake release server) ---
option(STUDIORIDER_BUILD_CLOUDFLARED_TEST "Build the cloudflared updater test" OFF)
if(STUDIORIDER_BUILD_CLOUDFLARED_TEST AND STUDIORIDER_BUILD_PLUGIN)
    juce_add_console_app(studiorider_cloudflared_test PRODUCT_NAME "studiorider_cloudflared_test")
    target_sources(studiorider_cloudflared_test PRIVATE tests/cloudflared_test.cpp Source/Cloudflared.cpp)
    target_include_directories(studiorider_cloudflared_test PRIVATE Source)
    target_compile_definitions(studiorider_cloudflared_test PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
    target_link_libraries(studiorider_cloudflared_test PRIVATE juce::juce_core juce::juce_cryptography
                          PUBLIC juce::juce_recommended_config_flags)
endif()

# --- Plugin state save/restore test ----------------------------------------------
option(STUDIORIDER_BUILD_STATE_TEST "Build the plugin state test" OFF)
if(STUDIORIDER_BUILD_STATE_TEST AND STUDIORIDER_BUILD_PLUGIN)
    # Reuses the plugin's already-compiled code (JUCE included): same headers and settings.
    add_executable(studiorider_state_test tests/state_test.cpp)
    target_include_directories(studiorider_state_test PRIVATE Source $<TARGET_PROPERTY:StudioRider,INCLUDE_DIRECTORIES>)
    target_compile_definitions(studiorider_state_test PRIVATE $<TARGET_PROPERTY:StudioRider,COMPILE_DEFINITIONS>)
    target_link_libraries(studiorider_state_test PRIVATE StudioRider)
endif()
