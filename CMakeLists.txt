cmake_minimum_required(VERSION 3.10)

project(tinyLog LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
set(BUILD_SHARED_LIBS TRUE)

set(DEBUG_ENABLE 1)

if(DEBUG_ENABLE)
    set(CODE_COVERAGE "-fprofile-arcs -ftest-coverage")
    set(LIBCODE_COVERAGE "-lgcov --coverage")
    set(OPTIM "-g -Og")
    add_definitions(-DWAKA_DEBUG)
else()
    set(OPTIM "-Ofast")
    add_definitions(-DWAKA_RELEASE)
endif()

set(CFLAGS "${CFLAGS} ${OPTIM}")
set(CXXFLAGS "${CXXFLAGS} ${OPTIM}")

set(CPPSRC
  log.cc
  tiny.rc
)

if(WIN32)
  list (APPEND CPPSRC dllmain.cc)
endif()

add_library(tinyLog SHARED ${CPPSRC})
set_property(TARGET tinyLog PROPERTY POSITION_INDEPENDENT_CODE 1)
set_target_properties(tinyLog PROPERTIES LINKER_LANGUAGE C)

set_target_properties(tinyLog PROPERTIES PREFIX "")
set_target_properties(tinyLog PROPERTIES OUTPUT_NAME "logger")