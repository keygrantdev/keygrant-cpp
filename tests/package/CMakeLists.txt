# An app's own CMake project using the SDK, as the README says to: the
# installed package (find_package), or the source tree (add_subdirectory).
# scripts/in-container.sh builds and runs it both ways.
cmake_minimum_required(VERSION 3.20)
project(keygrant_consumer LANGUAGES CXX)

# The app's own standard: C++17 by default; scripts/in-container.sh also
# builds it as C++20, so the public headers' C++20 paths (char8_t) compile.
set(KEYGRANT_CONSUMER_CXX_STANDARD 17 CACHE STRING "The C++ standard the consumer app is built with")
set(CMAKE_CXX_STANDARD ${KEYGRANT_CONSUMER_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(KEYGRANT_SOURCE_DIR)
  add_subdirectory(${KEYGRANT_SOURCE_DIR} keygrant)
else()
  find_package(keygrant 0.1 REQUIRED)
endif()

add_executable(consumer main.cpp entitlement_literals.cpp)
target_link_libraries(consumer PRIVATE keygrant::keygrant)
# Tells entitlement_literals.cpp which standard it was asked for, so a build
# that silently fell back to C++17 fails rather than skip the C++20 checks.
target_compile_definitions(consumer PRIVATE KEYGRANT_CONSUMER_CXX_STANDARD=${KEYGRANT_CONSUMER_CXX_STANDARD})
