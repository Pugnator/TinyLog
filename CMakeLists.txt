cmake_minimum_required(VERSION 3.21)

project(tinyLog LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CheckCXXCompilerFlag)

CHECK_CXX_COMPILER_FLAG("/std:c++latest" _cpp_latest_flag_supported)
if (_cpp_latest_flag_supported)
  add_compile_options("/std:c++latest")
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
set(BUILD_SHARED_LIBS TRUE)

set(OPTIM "-Ofast")
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