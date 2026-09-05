cmake_minimum_required(VERSION 3.16)
project(obs_multisite VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_OBS_PLUGIN "Build the OBS plugin module (needs libobs)" OFF)

# ── Reliability core: no OBS, no FFmpeg. Builds everywhere. ───────────────────
# Windows: keep <windows.h> from defining min/max macros that break std::min /
# std::max, and trim the header surface.
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

add_library(multisite_core STATIC
    src/core/spool_queue.cpp
    src/core/retry_uploader.cpp
    src/core/model.cpp
    src/core/session.cpp
    src/core/segment_cache.cpp
    src/core/decoder_session.cpp
    src/core/aws_sigv4.cpp
    src/core/s3_list_xml.cpp
    src/core/event_catalog.cpp)
target_include_directories(multisite_core PUBLIC src src/core)
# Must be position-independent: this static lib gets linked into the shared
# OBS plugin module.
set_target_properties(multisite_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    target_sources(multisite_core PRIVATE src/core/crypto_bcrypt.cpp)
    target_link_libraries(multisite_core PUBLIC bcrypt)
else()
    find_package(OpenSSL REQUIRED)
    target_sources(multisite_core PRIVATE src/core/crypto_openssl.cpp)
    target_link_libraries(multisite_core PUBLIC OpenSSL::Crypto)
endif()

find_package(Threads REQUIRED)
target_link_libraries(multisite_core PUBLIC Threads::Threads)

enable_testing()
add_executable(test_reliability tests/test_reliability.cpp)
target_link_libraries(test_reliability PRIVATE multisite_core)
add_test(NAME reliability COMMAND test_reliability)

# The core is shared with the planned headless appliance, which links neither
# OBS nor Qt. Enforce that boundary rather than trusting convention.
if(UNIX)
    # Invoked through sh explicitly: the executable bit is not preserved when
    # files are uploaded through GitHub's web interface, and ctest then reports
    # "permission denied" rather than running the check.
    add_test(NAME core_portable
             COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_core_portable.sh
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
endif()

add_executable(test_session tests/test_session.cpp)
target_link_libraries(test_session PRIVATE multisite_core)
add_test(NAME session COMMAND test_session)

# Phase 3: the satellite receive path and timeslipping.
add_executable(test_decoder tests/test_decoder.cpp)
target_link_libraries(test_decoder PRIVATE multisite_core)
add_test(NAME decoder COMMAND test_decoder)

# The dock reads a snapshot struct rather than the session directly, so the
# values it is built from must agree with their source. A stray line once reset
# one of them after it had been assigned, and the UI showed two contradictory
# states at once.
add_executable(test_snapshot tests/test_snapshot.cpp)
target_link_libraries(test_snapshot PRIVATE multisite_core)
add_test(NAME snapshot COMMAND test_snapshot)

# Guards the property that matters in live production: UI queries must stay
# fast while the decoder is downloading. Holding the state lock across network
# I/O once made a timeline click appear to lock OBS up entirely.
add_executable(test_responsive tests/test_responsive.cpp)
target_link_libraries(test_responsive PRIVATE multisite_core)
add_test(NAME responsive COMMAND test_responsive)

# Event listing reads a ListObjectsV2 response and signs a query string — the
# two things nothing else in the protocol does. Both are checked against
# captured response shapes rather than a live bucket.
add_executable(test_s3_list tests/test_s3_list.cpp)
target_link_libraries(test_s3_list PRIVATE multisite_core)
add_test(NAME s3_list COMMAND test_s3_list)

# What the event list shows an operator: which event is on air, which are
# finished recordings, and which were cut off by an encoder that died.
add_executable(test_event_catalog tests/test_event_catalog.cpp)
target_link_libraries(test_event_catalog PRIVATE multisite_core)
add_test(NAME event_catalog COMMAND test_event_catalog)

# ── Optional: S3 transport (needs libcurl) ────────────────────────────────────
find_package(CURL QUIET)
if(CURL_FOUND)
    add_library(multisite_s3 STATIC src/core/s3_transport.cpp)
    target_include_directories(multisite_s3 PUBLIC src src/core)
    target_link_libraries(multisite_s3 PUBLIC multisite_core CURL::libcurl)
    set_target_properties(multisite_s3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    message(STATUS "libcurl found — building S3 transport")

    # Endpoint/bucket values are pasted by operators, so the URL builder has to
    # tolerate schemes, trailing slashes and whitespace.
    add_executable(test_s3_url tests/test_s3_url.cpp)
    target_link_libraries(test_s3_url PRIVATE multisite_s3)
    add_test(NAME s3_url COMMAND test_s3_url)
else()
    message(STATUS "libcurl not found — skipping S3 transport")
endif()

# ── Optional: CMAF muxer + tests (needs FFmpeg) ───────────────────────────────
# FFmpeg discovery has two paths:
#   1. pkg-config (Linux/macOS, and anywhere .pc files exist)
#   2. a direct search of CMAKE_PREFIX_PATH (Windows obs-deps ships headers and
#      import libs with no pkg-config metadata)
option(FORCE_FFMPEG_MANUAL_SEARCH "Skip pkg-config and use the manual search" OFF)

set(FFMPEG_FOUND FALSE)

if(NOT FORCE_FFMPEG_MANUAL_SEARCH)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG IMPORTED_TARGET
            libavformat libavcodec libavutil libswresample libswscale)
    endif()
endif()

if(FFMPEG_FOUND AND TARGET PkgConfig::FFMPEG)
    add_library(ffmpeg_iface INTERFACE)
    target_link_libraries(ffmpeg_iface INTERFACE PkgConfig::FFMPEG)
    message(STATUS "FFmpeg found via pkg-config (${FFMPEG_libavformat_VERSION})")
else()
    # Manual search: works against obs-deps on Windows.
    find_path(FFMPEG_INCLUDE_DIR
        NAMES libavformat/avformat.h
        PATH_SUFFIXES include)
    set(_ffmpeg_libs "")
    set(_ffmpeg_missing "")
    foreach(_lib avformat avcodec avutil swresample swscale)
        find_library(FFMPEG_${_lib}_LIBRARY
            NAMES ${_lib} lib${_lib}
            PATH_SUFFIXES lib bin)
        if(FFMPEG_${_lib}_LIBRARY)
            list(APPEND _ffmpeg_libs "${FFMPEG_${_lib}_LIBRARY}")
        else()
            list(APPEND _ffmpeg_missing ${_lib})
        endif()
    endforeach()

    if(FFMPEG_INCLUDE_DIR AND NOT _ffmpeg_missing)
        set(FFMPEG_FOUND TRUE)
        add_library(ffmpeg_iface INTERFACE)
        target_include_directories(ffmpeg_iface INTERFACE "${FFMPEG_INCLUDE_DIR}")
        target_link_libraries(ffmpeg_iface INTERFACE ${_ffmpeg_libs})
        message(STATUS "FFmpeg found by manual search: ${FFMPEG_INCLUDE_DIR}")
    else()
        message(STATUS "FFmpeg not found "
                       "(include: ${FFMPEG_INCLUDE_DIR}, missing libs: ${_ffmpeg_missing})")
    endif()
endif()

if(FFMPEG_FOUND)
    add_library(multisite_cmaf STATIC
        src/core/cmaf_muxer.cpp
        src/core/cmaf_decoder.cpp)
    target_include_directories(multisite_cmaf PUBLIC src src/core)
    target_link_libraries(multisite_cmaf PUBLIC ffmpeg_iface)
    set_target_properties(multisite_cmaf PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_executable(test_cmaf tests/test_cmaf.cpp)
    target_link_libraries(test_cmaf PRIVATE multisite_cmaf)
    add_test(NAME cmaf
             COMMAND ${CMAKE_COMMAND}
                     -DTEST_EXE=$<TARGET_FILE:test_cmaf>
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_cmaf_test.cmake)

    # HEVC through the same muxer. The pipeline is codec-agnostic by design, so
    # the guarantee is worth asserting rather than assuming — especially with
    # the Raspberry Pi decoder tier depending on HEVC.
    add_test(NAME cmaf_hevc
             COMMAND ${CMAKE_COMMAND}
                     -DTEST_EXE=$<TARGET_FILE:test_cmaf>
                     -DCMAKE_CURRENT_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_hevc_test.cmake)

    # Round trip: decode the output the muxer test just produced. Generating
    # the fixture at test time keeps large media out of the repository while
    # still exercising encode -> decode on real bytes.
    add_executable(test_cmaf_decode tests/test_cmaf_decode.cpp)
    target_link_libraries(test_cmaf_decode PRIVATE multisite_cmaf)
    add_test(NAME cmaf_decode
             COMMAND $<TARGET_FILE:test_cmaf_decode>
                     "${CMAKE_CURRENT_BINARY_DIR}/cmaf_out/init.mp4"
                     "${CMAKE_CURRENT_BINARY_DIR}/cmaf_out/seg_000.m4s")
    # The muxer test writes that fixture, so it has to run first.
    set_tests_properties(cmaf_decode PROPERTIES DEPENDS cmaf)
else()
    message(STATUS "skipping CMAF muxer targets")
endif()

# ── Optional: the satellite appliance (headless player) ───────────────────────
# The Raspberry Pi / mini-PC receive box: the same core, with its own video and
# audio output and a browser-based operator interface instead of OBS.
#
# It builds anywhere the core does. On a machine with no KMS and no ALSA — a
# developer's laptop — the outputs fall back to null implementations, which is
# what lets the engine and the whole web interface be exercised without the
# hardware in front of you.
option(BUILD_PLAYER "Build the satellite appliance (headless player)" ON)

if(BUILD_PLAYER AND FFMPEG_FOUND AND CURL_FOUND AND NOT WIN32)
    set(MULTISITE_PLAYER_VERSION "0.1.0" CACHE STRING "Player version string")
    # Where the operator interface is installed. Overridable at runtime with
    # --web-root so the page can be edited on a running box.
    set(MULTISITE_PLAYER_WEB_ROOT "${CMAKE_INSTALL_PREFIX}/share/multisite-player/web"
        CACHE STRING "Installed location of the operator interface")

    add_executable(multisite-player
        src/appliance/main.cpp
        src/appliance/config.cpp
        src/appliance/log.cpp
        src/appliance/player.cpp
        src/appliance/http_server.cpp
        src/appliance/api.cpp
        src/appliance/sysinfo.cpp
        src/appliance/preview.cpp
        src/appliance/outputs.cpp)

    target_link_libraries(multisite-player PRIVATE
        multisite_core multisite_s3 multisite_cmaf)
    target_compile_definitions(multisite-player PRIVATE
        MULTISITE_PLAYER_VERSION="${MULTISITE_PLAYER_VERSION}"
        MULTISITE_PLAYER_WEB_ROOT="${MULTISITE_PLAYER_WEB_ROOT}")

    # DRM/KMS: the appliance drives the HDMI output itself, with no desktop.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBDRM IMPORTED_TARGET libdrm)
        pkg_check_modules(ALSA IMPORTED_TARGET alsa)
    endif()

    if(TARGET PkgConfig::LIBDRM)
        target_sources(multisite-player PRIVATE src/appliance/drm_output.cpp)
        target_link_libraries(multisite-player PRIVATE PkgConfig::LIBDRM)
        target_compile_definitions(multisite-player PRIVATE MULTISITE_HAVE_DRM=1)
        message(STATUS "player: DRM/KMS display output enabled")
    else()
        message(STATUS "player: no libdrm — building without display output")
    endif()

    if(TARGET PkgConfig::ALSA)
        target_sources(multisite-player PRIVATE src/appliance/alsa_output.cpp)
        target_link_libraries(multisite-player PRIVATE PkgConfig::ALSA)
        target_compile_definitions(multisite-player PRIVATE MULTISITE_HAVE_ALSA=1)
        message(STATUS "player: ALSA audio output enabled")
    else()
        message(STATUS "player: no ALSA — building without audio output")
    endif()

    install(TARGETS multisite-player RUNTIME DESTINATION bin)
    install(DIRECTORY src/appliance/web/
            DESTINATION share/multisite-player/web)
elseif(BUILD_PLAYER)
    message(STATUS "skipping the appliance "
                   "(needs FFmpeg and libcurl on a POSIX host)")
endif()

# ── Optional: the OBS plugin module ───────────────────────────────────────────
# Enable with -DBUILD_OBS_PLUGIN=ON. libobs is discovered either via its CMake
# config (obs-deps / an OBS build tree) or via pkg-config (distro libobs-dev).
if(BUILD_OBS_PLUGIN)
    if(NOT FFMPEG_FOUND OR NOT CURL_FOUND)
        message(FATAL_ERROR
            "OBS plugin needs both FFmpeg and libcurl.\n"
            "  FFmpeg found: ${FFMPEG_FOUND}\n"
            "  libcurl found: ${CURL_FOUND}\n"
            "Point CMAKE_PREFIX_PATH at obs-deps (which ships both), e.g.\n"
            "  cmake -S . -B build -DBUILD_OBS_PLUGIN=ON "
            "-DCMAKE_PREFIX_PATH=<obs-deps>")
    endif()

    # libobs discovery. Three paths, in order:
    #   1. explicit -DLIBOBS_INCLUDE_DIR / -DLIBOBS_LIBRARY (most deterministic;
    #      what CI uses on Windows against an OBS build tree)
    #   2. its CMake config package (obs-deps / installed OBS)
    #   3. pkg-config (distro libobs-dev)
    #
    # Path 2 is skipped on Windows: libobsConfig.cmake from a build tree calls
    # find_dependency(w32-pthreads), which isn't exported with it and hard-fails.
    option(LIBOBS_MANUAL_SEARCH "Bypass libobs's CMake config package" OFF)
    if(WIN32)
        set(LIBOBS_MANUAL_SEARCH ON)
    endif()

    if(LIBOBS_INCLUDE_DIR OR LIBOBS_LIBRARY)
        if(NOT LIBOBS_INCLUDE_DIR OR NOT LIBOBS_LIBRARY)
            message(FATAL_ERROR
                "Give both -DLIBOBS_INCLUDE_DIR and -DLIBOBS_LIBRARY, or neither.\n"
                "  LIBOBS_INCLUDE_DIR: ${LIBOBS_INCLUDE_DIR}\n"
                "  LIBOBS_LIBRARY: ${LIBOBS_LIBRARY}")
        endif()
        if(NOT EXISTS "${LIBOBS_INCLUDE_DIR}/obs-module.h")
            message(FATAL_ERROR
                "obs-module.h not found in LIBOBS_INCLUDE_DIR "
                "(${LIBOBS_INCLUDE_DIR}) — point it at obs-studio's libobs/ dir")
        endif()
        # obsconfig.h is GENERATED into the OBS build tree, so when building
        # against a build tree we need that directory on the include path too.
        set(_obs_includes "${LIBOBS_INCLUDE_DIR}")
        if(LIBOBS_CONFIG_INCLUDE_DIR)
            list(APPEND _obs_includes "${LIBOBS_CONFIG_INCLUDE_DIR}")
            message(STATUS "libobs generated headers: ${LIBOBS_CONFIG_INCLUDE_DIR}")
        endif()
        add_library(OBS::libobs UNKNOWN IMPORTED)
        set_target_properties(OBS::libobs PROPERTIES
            IMPORTED_LOCATION "${LIBOBS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_obs_includes}")
        message(STATUS "libobs used explicitly: ${LIBOBS_LIBRARY}")
    else()
        if(NOT LIBOBS_MANUAL_SEARCH)
            find_package(libobs CONFIG QUIET)
            if(TARGET OBS::libobs)
                message(STATUS "libobs found via CMake config")
            endif()
        endif()
        if(NOT TARGET OBS::libobs)
            if(PkgConfig_FOUND)
                pkg_check_modules(LIBOBS IMPORTED_TARGET libobs)
            endif()
            if(TARGET PkgConfig::LIBOBS)
                add_library(OBS::libobs ALIAS PkgConfig::LIBOBS)
                message(STATUS "libobs found via pkg-config (${LIBOBS_VERSION})")
            else()
                message(FATAL_ERROR
                    "libobs not found. Either install libobs-dev, or pass\n"
                    "  -DLIBOBS_INCLUDE_DIR=<obs-studio>/libobs\n"
                    "  -DLIBOBS_LIBRARY=<path to obs.lib or libobs.so>")
            endif()
        endif()
    endif()

    # The Tools-menu items need obs-frontend-api, which OBS only builds as part
    # of its UI (and which pulls in Qt). Hotkeys live in libobs itself, so the
    # plugin is fully usable without this; enable it if you have the library.
    option(ENABLE_FRONTEND_API "Build Tools-menu items (needs obs-frontend-api)" OFF)
    # The operator docks need Qt6 and obs-frontend-api. OBS only builds
    # obs-frontend-api as part of its Qt UI, so a CI build must configure OBS
    # with ENABLE_UI=ON (building just the libobs and obs-frontend-api targets
    # is enough — the whole OBS app is not needed).
    option(ENABLE_QT "Build the operator docks (needs Qt6 + obs-frontend-api)" OFF)

    add_library(obs-multisite MODULE
        src/obs/plugin-main.cpp
        src/obs/multisite_output.cpp
        src/obs/multisite_source.cpp
        src/obs/multisite_ui.cpp
        src/obs/broadcast_controller.cpp
        src/obs/decoder_settings.cpp)
    target_include_directories(obs-multisite PRIVATE src src/core)
    target_compile_definitions(obs-multisite PRIVATE
        PLUGIN_VERSION="${PROJECT_VERSION}")

    if(ENABLE_QT)
        set(CMAKE_AUTOMOC ON)
        find_package(Qt6 REQUIRED COMPONENTS Widgets)
        if(LIBOBS_FRONTEND_LIBRARY)
            set(_qt_frontend "${LIBOBS_FRONTEND_LIBRARY}")
        else()
            find_library(_qt_frontend NAMES obs-frontend-api REQUIRED)
        endif()
        # obs-frontend-api.h is NOT beside the libobs headers: it lives in
        # OBS's own source tree (frontend/api in OBS 32, UI/obs-frontend-api in
        # older releases). Passing only the .lib is not enough.
        if(LIBOBS_FRONTEND_INCLUDE_DIR)
            set(_fe_inc "${LIBOBS_FRONTEND_INCLUDE_DIR}")
        else()
            find_path(_fe_inc obs-frontend-api.h
                      HINTS "${LIBOBS_INCLUDE_DIR}" "${LIBOBS_INCLUDE_DIR}/.."
                      PATH_SUFFIXES obs frontend/api UI/obs-frontend-api)
        endif()
        if(NOT _fe_inc)
            message(FATAL_ERROR
                "ENABLE_QT=ON but obs-frontend-api.h was not found. Pass "
                "-DLIBOBS_FRONTEND_INCLUDE_DIR=<obs-studio>/frontend/api")
        endif()
        message(STATUS "obs-frontend-api headers: ${_fe_inc}")
        target_include_directories(obs-multisite PRIVATE "${_fe_inc}")

        target_sources(obs-multisite PRIVATE
            src/obs/ui/encoder_dock.cpp
            src/obs/ui/decoder_dock.cpp
            src/obs/ui/dock_register.cpp)
        set_target_properties(obs-multisite PROPERTIES AUTOMOC ON)
        target_link_libraries(obs-multisite PRIVATE Qt6::Widgets "${_qt_frontend}")
        target_compile_definitions(obs-multisite PRIVATE
            MULTISITE_HAVE_QT MULTISITE_HAVE_FRONTEND_API)
        message(STATUS "Qt docks enabled (Qt6 ${Qt6_VERSION}, frontend ${_qt_frontend})")
    endif()

    if(ENABLE_FRONTEND_API AND NOT ENABLE_QT)
        if(LIBOBS_FRONTEND_LIBRARY)
            set(_frontend_lib "${LIBOBS_FRONTEND_LIBRARY}")
        else()
            find_library(_frontend_lib NAMES obs-frontend-api)
        endif()
        if(_frontend_lib)
            target_link_libraries(obs-multisite PRIVATE "${_frontend_lib}")
            target_compile_definitions(obs-multisite PRIVATE
                MULTISITE_HAVE_FRONTEND_API)
            message(STATUS "obs-frontend-api: ${_frontend_lib} (Tools menu on)")
        else()
            message(WARNING
                "ENABLE_FRONTEND_API=ON but obs-frontend-api was not found; "
                "building without Tools-menu items (hotkeys still work)")
        endif()
    else()
        message(STATUS "Tools menu disabled (hotkeys only) — "
                       "set -DENABLE_FRONTEND_API=ON to include it")
    endif()
    target_link_libraries(obs-multisite PRIVATE
        OBS::libobs multisite_core multisite_s3 multisite_cmaf)
    # FFmpeg comes through multisite_cmaf's INTERFACE link (ffmpeg_iface).
    set_target_properties(obs-multisite PROPERTIES PREFIX "")

    if(WIN32)
        # A MODULE library installs under LIBRARY on every platform, including
        # Windows — RUNTIME applies to executables and DLLs from SHARED targets.
        install(TARGETS obs-multisite
            LIBRARY DESTINATION "obs-plugins/64bit"
            RUNTIME DESTINATION "obs-plugins/64bit")
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
            DESTINATION "data/obs-plugins/obs-multisite")
    else()
        install(TARGETS obs-multisite
            LIBRARY DESTINATION "$ENV{HOME}/.config/obs-studio/plugins/obs-multisite/bin/64bit")
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
            DESTINATION "$ENV{HOME}/.config/obs-studio/plugins/obs-multisite/data")
    endif()
    message(STATUS "Building OBS plugin module")
endif()
