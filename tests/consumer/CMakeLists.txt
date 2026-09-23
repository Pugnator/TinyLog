# Builds a program against tinylog the way users do:
#   -DTINYLOG_CONSUMER_MODE=package      find_package() on an installed tree
#   -DTINYLOG_CONSUMER_MODE=subdirectory add_subdirectory() of the source tree
cmake_minimum_required(VERSION 3.21)
project(tinylog_consumer LANGUAGES CXX)

set(TINYLOG_CONSUMER_MODE package CACHE STRING "package or subdirectory")
if(TINYLOG_CONSUMER_MODE STREQUAL "package")
  find_package(tinylog 0.2 REQUIRED)
else()
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. tinylog)
endif()

add_executable(consumer main.cc)
target_link_libraries(consumer PRIVATE tinylog::tinylog)
enable_testing()
add_test(NAME consumer COMMAND consumer)
