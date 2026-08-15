cmake_minimum_required(VERSION 3.21)

project(tinyLog LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

message(STATUS "C++ standard is ${CMAKE_CXX_STANDARD}")

set(OPTIM "-Ofast")
set(CFLAGS "${CFLAGS} ${OPTIM}")
set(CXXFLAGS "${CXXFLAGS} ${OPTIM}")

# ---------------------------------------------------------------------------
# zstd – git submodule at vendor/zstd, built as a static lib linked into DLL
# ---------------------------------------------------------------------------
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
add_subdirectory(vendor/zstd/build/cmake)

set(CPPSRC
  log.cc
)

if (MSVC)
  set(
    CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} /std:c++20"
  )
endif()

add_library(tinyLog STATIC ${CPPSRC})
set_property(TARGET tinyLog PROPERTY POSITION_INDEPENDENT_CODE 1)
set_target_properties(tinyLog PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(tinyLog PRIVATE
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/EHsc>
)

target_link_libraries(tinyLog PRIVATE libzstd_static)
target_include_directories(tinyLog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/zstd/lib)
