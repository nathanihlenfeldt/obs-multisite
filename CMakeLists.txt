cmake_minimum_required(VERSION 3.16)
project(multisite_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Core reliability library (Phase 1). Format-agnostic; no OBS dependency.
add_library(multisite_core STATIC
    src/core/spool_queue.cpp
    src/core/retry_uploader.cpp
    src/core/model.cpp
    src/core/aws_sigv4.cpp)

target_include_directories(multisite_core PUBLIC src src/core)

if(WIN32)
    target_sources(multisite_core PRIVATE src/core/crypto_bcrypt.cpp)
    target_link_libraries(multisite_core PRIVATE bcrypt)
else()
    find_package(OpenSSL REQUIRED)
    target_sources(multisite_core PRIVATE src/core/crypto_openssl.cpp)
    target_link_libraries(multisite_core PRIVATE OpenSSL::Crypto)
endif()

find_package(Threads REQUIRED)
target_link_libraries(multisite_core PRIVATE Threads::Threads)

# Test harness
enable_testing()
add_executable(test_reliability tests/test_reliability.cpp)
target_link_libraries(test_reliability PRIVATE multisite_core)
add_test(NAME reliability COMMAND test_reliability)
