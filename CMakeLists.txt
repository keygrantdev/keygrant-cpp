cmake_minimum_required(VERSION 3.20)

project(keygrant
  VERSION 0.2.0
  DESCRIPTION "KeyGrant licensing SDK for native desktop apps"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(KEYGRANT_TOP_LEVEL ON)
else()
  set(KEYGRANT_TOP_LEVEL OFF)
endif()

option(KEYGRANT_BUILD_TESTS "Build the SDK's tests (needs GoogleTest)" ${KEYGRANT_TOP_LEVEL})
option(KEYGRANT_WARNINGS_AS_ERRORS "Treat warnings as errors in the SDK's own targets" ${KEYGRANT_TOP_LEVEL})
option(KEYGRANT_INSTALL "Generate install rules and a CMake package" ${KEYGRANT_TOP_LEVEL})

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)

# Before 7.84, curl_global_init is not thread-safe: the app must call it in
# main() before starting any thread (CurlHttp's own call is then a no-op).
if(DEFINED CURL_VERSION_STRING AND NOT CURL_VERSION_STRING STREQUAL "")
  set(KEYGRANT_CURL_VERSION "${CURL_VERSION_STRING}")
elseif(DEFINED CURL_VERSION AND NOT CURL_VERSION STREQUAL "")
  set(KEYGRANT_CURL_VERSION "${CURL_VERSION}")
endif()
if(DEFINED KEYGRANT_CURL_VERSION AND KEYGRANT_CURL_VERSION VERSION_LESS 7.84)
  message(WARNING "libcurl ${KEYGRANT_CURL_VERSION} is older than 7.84: its curl_global_init is not thread-safe. "
                  "Call curl_global_init(CURL_GLOBAL_DEFAULT) in main() before the app starts any thread (see the README).")
endif()

add_library(keygrant
  src/adapters.cpp
  src/clock.cpp
  src/crypto.cpp
  src/device.cpp
  src/engine.cpp
  src/fingerprinter.cpp
  src/http_curl.cpp
  src/json_util.cpp
  src/lease.cpp
  src/license.cpp
  src/machine_id.cpp
  src/machine_id_system.cpp
  src/offline.cpp
  src/pickup.cpp
  src/purchases.cpp
  src/renewal.cpp
  src/state_json.cpp
  src/storage_file.cpp
  src/storage_fs.cpp
  src/storage_paths.cpp
  src/sys.cpp
  src/trials.cpp
  src/types.cpp
  src/url.cpp
  src/utf.cpp
  src/wire.cpp)
add_library(keygrant::keygrant ALIAS keygrant)

include(GNUInstallDirs)

target_compile_features(keygrant PUBLIC cxx_std_17)
set_target_properties(keygrant PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(keygrant
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(keygrant
  PRIVATE
    OpenSSL::Crypto
    CURL::libcurl
    Threads::Threads
    # Header-only, and not in any public header: consumers never need it.
    $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>)

if(WIN32)
  target_link_libraries(keygrant PRIVATE advapi32 ws2_32 shell32 ole32 uuid)
  target_compile_definitions(keygrant PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
elseif(APPLE)
  target_link_libraries(keygrant PRIVATE "-framework IOKit" "-framework CoreFoundation")
endif()

# The SDK's own targets only: -Werror never reaches an app that builds it.
function(keygrant_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    if(KEYGRANT_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(KEYGRANT_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
keygrant_warnings(keygrant)

if(KEYGRANT_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  # The tests' own TLS server (the libcurl adapter's TLS and redirects).
  find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto SSL)
  add_executable(keygrant_tests
    tests/conformance_purchase_test.cpp
    tests/conformance_support.cpp
    tests/conformance_test.cpp
    tests/crypto_test.cpp
    tests/http_test.cpp
    tests/launch_report_test.cpp
    tests/license_activate_test.cpp
    tests/license_device_test.cpp
    tests/license_entitlements_test.cpp
    tests/license_pickup_test.cpp
    tests/license_purchase_test.cpp
    tests/license_renewal_test.cpp
    tests/license_status_test.cpp
    tests/license_timing_test.cpp
    tests/license_trial_test.cpp
    tests/machine_id_test.cpp
    tests/offline_test.cpp
    tests/stash_test.cpp
    tests/state_test.cpp
    tests/storage_engine_test.cpp
    tests/storage_test.cpp
    tests/support.cpp
    tests/wire_test.cpp)
  target_include_directories(keygrant_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(keygrant_tests PRIVATE
    keygrant::keygrant
    GTest::gtest
    GTest::gtest_main
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads)
  set_target_properties(keygrant_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  # The conformance vectors every KeyGrant SDK answers: conformance/vectors.json
  # beside this file, else in the folder above it (a checkout that keeps the SDK
  # in a subfolder).
  set(KEYGRANT_VECTORS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/conformance/vectors.json")
  if(NOT EXISTS "${KEYGRANT_VECTORS_FILE}")
    set(KEYGRANT_VECTORS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../conformance/vectors.json")
  endif()
  target_compile_definitions(keygrant_tests PRIVATE
    KEYGRANT_VECTORS_PATH="${KEYGRANT_VECTORS_FILE}")
  keygrant_warnings(keygrant_tests)
  if(WIN32)
    target_compile_definitions(keygrant_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(keygrant_tests PRIVATE ws2_32 advapi32)
  endif()
  include(GoogleTest)
  gtest_discover_tests(keygrant_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)

  # The public headers as an app built as C++20 sees them (char8_t and its u8
  # literals exist there only), compiled with every test build on every
  # platform: tests/package/entitlement_literals.cpp checks, as it compiles,
  # what an app's literals make of an Entitlement, and fails if it was not
  # built as C++20 after all.
  add_library(keygrant_cxx20_checks OBJECT tests/package/entitlement_literals.cpp)
  target_link_libraries(keygrant_cxx20_checks PRIVATE keygrant::keygrant)
  set_target_properties(keygrant_cxx20_checks PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_compile_definitions(keygrant_cxx20_checks PRIVATE KEYGRANT_CONSUMER_CXX_STANDARD=20)
  keygrant_warnings(keygrant_cxx20_checks)
  add_dependencies(keygrant_tests keygrant_cxx20_checks)
endif()

if(KEYGRANT_INSTALL)
  include(CMakePackageConfigHelpers)
  if(BUILD_SHARED_LIBS)
    set(KEYGRANT_STATIC OFF)
  else()
    set(KEYGRANT_STATIC ON)
  endif()
  set(KEYGRANT_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/keygrant)
  install(TARGETS keygrant EXPORT keygrantTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/keygrant DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT keygrantTargets NAMESPACE keygrant:: DESTINATION ${KEYGRANT_CMAKE_DIR})
  configure_package_config_file(cmake/keygrantConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/keygrantConfig.cmake
    INSTALL_DESTINATION ${KEYGRANT_CMAKE_DIR})
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/keygrantConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/keygrantConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/keygrantConfigVersion.cmake
    DESTINATION ${KEYGRANT_CMAKE_DIR})
endif()
