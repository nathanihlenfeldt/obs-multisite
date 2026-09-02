cmake_minimum_required(VERSION 3.16)
project(obs_multisite VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_OBS_PLUGIN "Build the OBS plugin module (needs libobs)" OFF)

# ── Reliability core: no OBS, no FFmpeg. Builds everywhere. ───────────────────
add_library(multisite_core STATIC
    src/core/spool_queue.cpp
    src/core/retry_uploader.cpp
    src/core/model.cpp
    src/core/session.cpp
    src/core/aws_sigv4.cpp)
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

add_executable(test_session tests/test_session.cpp)
target_link_libraries(test_session PRIVATE multisite_core)
add_test(NAME session COMMAND test_session)

# ── Optional: S3 transport (needs libcurl) ────────────────────────────────────
find_package(CURL QUIET)
if(CURL_FOUND)
    add_library(multisite_s3 STATIC src/core/s3_transport.cpp)
    target_include_directories(multisite_s3 PUBLIC src src/core)
    target_link_libraries(multisite_s3 PUBLIC multisite_core CURL::libcurl)
    set_target_properties(multisite_s3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    message(STATUS "libcurl found — building S3 transport")
else()
    message(STATUS "libcurl not found — skipping S3 transport")
endif()

# ── Optional: CMAF muxer + tests (needs FFmpeg) ───────────────────────────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil)
endif()

if(FFMPEG_FOUND)
    message(STATUS "FFmpeg found — building CMAF muxer")
    add_library(multisite_cmaf STATIC src/core/cmaf_muxer.cpp)
    target_include_directories(multisite_cmaf PUBLIC src src/core)
    target_link_libraries(multisite_cmaf PUBLIC PkgConfig::FFMPEG)
    set_target_properties(multisite_cmaf PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_executable(test_cmaf tests/test_cmaf.cpp)
    target_link_libraries(test_cmaf PRIVATE multisite_cmaf)
    add_test(NAME cmaf
             COMMAND ${CMAKE_COMMAND}
                     -DTEST_EXE=$<TARGET_FILE:test_cmaf>
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_cmaf_test.cmake)
else()
    message(STATUS "FFmpeg not found — skipping CMAF muxer")
endif()

# ── Optional: the OBS plugin module ───────────────────────────────────────────
# Enable with -DBUILD_OBS_PLUGIN=ON. libobs is discovered either via its CMake
# config (obs-deps / an OBS build tree) or via pkg-config (distro libobs-dev).
if(BUILD_OBS_PLUGIN)
    if(NOT FFMPEG_FOUND OR NOT CURL_FOUND)
        message(FATAL_ERROR "OBS plugin needs both FFmpeg and libcurl")
    endif()

    find_package(libobs CONFIG QUIET)
    if(NOT TARGET OBS::libobs)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBOBS IMPORTED_TARGET libobs)
        endif()
        if(TARGET PkgConfig::LIBOBS)
            add_library(OBS::libobs ALIAS PkgConfig::LIBOBS)
            message(STATUS "libobs found via pkg-config (${LIBOBS_VERSION})")
        else()
            message(FATAL_ERROR
                "libobs not found. Install libobs-dev, or pass "
                "-DCMAKE_PREFIX_PATH=<obs-deps>.")
        endif()
    else()
        message(STATUS "libobs found via CMake config")
    endif()

    add_library(obs-multisite MODULE
        src/obs/plugin-main.cpp
        src/obs/multisite_output.cpp)
    target_include_directories(obs-multisite PRIVATE src src/core)
    target_compile_definitions(obs-multisite PRIVATE
        PLUGIN_VERSION="${PROJECT_VERSION}")
    target_link_libraries(obs-multisite PRIVATE
        OBS::libobs multisite_core multisite_s3 multisite_cmaf)
    set_target_properties(obs-multisite PROPERTIES PREFIX "")

    if(WIN32)
        install(TARGETS obs-multisite
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
