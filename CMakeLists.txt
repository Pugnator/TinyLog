cmake_minimum_required(VERSION 3.21)

project(tinylog
  VERSION 0.2.0
  DESCRIPTION "Small, fast C++20 logger with zstd-compressed rotation"
  HOMEPAGE_URL "https://github.com/Pugnator/SimpleLogger"
  LANGUAGES C CXX)

if(WIN32)
  enable_language(RC)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Options. As a subproject (add_subdirectory) only the static library is built.
# ---------------------------------------------------------------------------
option(TINYLOG_BUILD_STATIC "Build the static library (target tinylog)" ON)
option(TINYLOG_BUILD_SHARED "Build the shared library / DLL (target tinylog_shared)" ${PROJECT_IS_TOP_LEVEL})
option(TINYLOG_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(TINYLOG_BUILD_BENCH "Build the benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(TINYLOG_BUILD_TOOLS "Build tinylog-cat" ${PROJECT_IS_TOP_LEVEL})
option(TINYLOG_INSTALL "Generate install rules" ${PROJECT_IS_TOP_LEVEL})
option(TINYLOG_USE_SYSTEM_ZSTD "Link an installed zstd instead of compiling vendor/zstd in" OFF)
option(TINYLOG_BENCH_COMPARE "Download spdlog and Quill and benchmark against them" OFF)
option(TINYLOG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(TINYLOG_SANITIZE "" CACHE STRING "Sanitizers for all targets, e.g. address,undefined or thread")

if(NOT TINYLOG_BUILD_STATIC AND NOT TINYLOG_BUILD_SHARED)
  message(FATAL_ERROR "Enable TINYLOG_BUILD_STATIC and/or TINYLOG_BUILD_SHARED")
endif()

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(TINYLOG_SANITIZE)
  if(MSVC)
    add_compile_options(/fsanitize=${TINYLOG_SANITIZE})
  else()
    add_compile_options(-fsanitize=${TINYLOG_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${TINYLOG_SANITIZE})
  endif()
endif()

if(PROJECT_IS_TOP_LEVEL)
  # One output directory, so tests find the DLL next to them on Windows.
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# zstd: compiled into the library by default, so users never see it.
# ---------------------------------------------------------------------------
if(TINYLOG_USE_SYSTEM_ZSTD)
  # The static library prefers libzstd.a, the shared one libzstd.so: distro
  # static archives are often not position independent enough to go into a .so.
  find_package(zstd CONFIG QUIET)
  if(TARGET zstd::libzstd)
    set(TINYLOG_ZSTD_STATIC zstd::libzstd)
    set(TINYLOG_ZSTD_SHARED zstd::libzstd)
  endif()
  if(TARGET zstd::libzstd_shared)
    set(TINYLOG_ZSTD_SHARED zstd::libzstd_shared)
    set(TINYLOG_ZSTD_STATIC zstd::libzstd_shared)
  endif()
  if(TARGET zstd::libzstd_static)
    set(TINYLOG_ZSTD_STATIC zstd::libzstd_static)
    if(NOT TINYLOG_ZSTD_SHARED)
      set(TINYLOG_ZSTD_SHARED zstd::libzstd_static)
    endif()
  endif()
  if(NOT TINYLOG_ZSTD_STATIC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    set(TINYLOG_ZSTD_STATIC PkgConfig::ZSTD)
    set(TINYLOG_ZSTD_SHARED PkgConfig::ZSTD)
  endif()
else()
  set(TINYLOG_ZSTD_DIR ${PROJECT_SOURCE_DIR}/vendor/zstd/lib)
  if(NOT EXISTS ${TINYLOG_ZSTD_DIR}/zstd.h)
    message(FATAL_ERROR "vendor/zstd is empty - run: git submodule update --init")
  endif()
  file(GLOB TINYLOG_ZSTD_SOURCES CONFIGURE_DEPENDS
    ${TINYLOG_ZSTD_DIR}/common/*.c
    ${TINYLOG_ZSTD_DIR}/compress/*.c
    ${TINYLOG_ZSTD_DIR}/decompress/*.c)
  add_library(tinylog_zstd OBJECT ${TINYLOG_ZSTD_SOURCES})
  target_include_directories(tinylog_zstd PRIVATE ${TINYLOG_ZSTD_DIR} ${TINYLOG_ZSTD_DIR}/common)
  # Hidden symbols, no assembly (portable), xxhash namespaced as zstd's own build does.
  target_compile_definitions(tinylog_zstd PRIVATE
    ZSTD_DISABLE_ASM XXH_NAMESPACE=ZSTD_ ZSTDLIB_VISIBLE= ZSTDERRORLIB_VISIBLE= ZSTD_LEGACY_SUPPORT=0)
  set_target_properties(tinylog_zstd PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
  if(MSVC)
    target_compile_options(tinylog_zstd PRIVATE /W0)
  else()
    target_compile_options(tinylog_zstd PRIVATE -w)
  endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(TINYLOG_GENERATED ${PROJECT_BINARY_DIR}/generated)
configure_file(include/tinylog/version.hpp.in ${TINYLOG_GENERATED}/include/tinylog/version.hpp @ONLY)

set(TINYLOG_SOURCES
  src/c_api.cc
  src/config.cc
  src/console_sink.cc
  src/core.cc
  src/file_sink.cc
  src/format.cc
  src/platform.cc
  src/reader.cc
  src/zstd_util.cc)

function(tinylog_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus
      $<$<BOOL:${TINYLOG_WARNINGS_AS_ERRORS}>:/WX>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
      $<$<BOOL:${TINYLOG_WARNINGS_AS_ERRORS}>:-Werror>)
  endif()
endfunction()

function(tinylog_setup_library target zstd_target)
  target_sources(${target} PRIVATE ${TINYLOG_SOURCES})
  target_include_directories(${target}
    PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${TINYLOG_GENERATED}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${PROJECT_SOURCE_DIR}/src)
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_compile_definitions(${target} PRIVATE TINYLOG_BUILDING)
  target_link_libraries(${target} PRIVATE Threads::Threads)
  if(TINYLOG_USE_SYSTEM_ZSTD)
    target_link_libraries(${target} PRIVATE ${zstd_target})
  else()
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:tinylog_zstd>)
    target_include_directories(${target} PRIVATE ${TINYLOG_ZSTD_DIR})
  endif()
  set_target_properties(${target} PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
  tinylog_warnings(${target})
endfunction()

set(TINYLOG_EXPORT_TARGETS)

if(TINYLOG_BUILD_STATIC)
  add_library(tinylog STATIC)
  tinylog_setup_library(tinylog "${TINYLOG_ZSTD_STATIC}")
  if(MSVC)
    # tinylog.lib is the DLL's import library.
    set_target_properties(tinylog PROPERTIES OUTPUT_NAME tinylog_static)
  endif()
  set_target_properties(tinylog PROPERTIES EXPORT_NAME tinylog)
  add_library(tinylog::static ALIAS tinylog)
  list(APPEND TINYLOG_EXPORT_TARGETS tinylog)
endif()

if(TINYLOG_BUILD_SHARED)
  add_library(tinylog_shared SHARED)
  tinylog_setup_library(tinylog_shared "${TINYLOG_ZSTD_SHARED}")
  target_compile_definitions(tinylog_shared PUBLIC TINYLOG_SHARED)
  set_target_properties(tinylog_shared PROPERTIES
    OUTPUT_NAME tinylog
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    EXPORT_NAME shared
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  if(WIN32)
    set(TINYLOG_DLL_NAME ${CMAKE_SHARED_LIBRARY_PREFIX}tinylog${CMAKE_SHARED_LIBRARY_SUFFIX})
    configure_file(src/tinylog.rc.in ${TINYLOG_GENERATED}/tinylog.rc @ONLY)
    target_sources(tinylog_shared PRIVATE src/dllmain.cc ${TINYLOG_GENERATED}/tinylog.rc)
  endif()
  add_library(tinylog::shared ALIAS tinylog_shared)
  list(APPEND TINYLOG_EXPORT_TARGETS tinylog_shared)
endif()

# tinylog::tinylog is the static library when built; tinyLog is the pre-0.2 target name.
if(TINYLOG_BUILD_STATIC)
  add_library(tinylog::tinylog ALIAS tinylog)
  add_library(tinyLog ALIAS tinylog)
else()
  add_library(tinylog::tinylog ALIAS tinylog_shared)
  add_library(tinyLog ALIAS tinylog_shared)
endif()

# ---------------------------------------------------------------------------
# Tools, tests, benchmarks
# ---------------------------------------------------------------------------
if(TINYLOG_BUILD_TOOLS)
  add_executable(tinylog-cat tools/tinylog_cat.cc)
  target_link_libraries(tinylog-cat PRIVATE tinylog::tinylog)
  tinylog_warnings(tinylog-cat)
endif()

if(TINYLOG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TINYLOG_BUILD_BENCH)
  add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
# Install: headers, libraries, CMake package, pkg-config
# ---------------------------------------------------------------------------
if(TINYLOG_INSTALL)
  install(DIRECTORY include/tinylog DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PATTERN "*.in" EXCLUDE)
  install(FILES ${TINYLOG_GENERATED}/include/tinylog/version.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tinylog)
  install(TARGETS ${TINYLOG_EXPORT_TARGETS} EXPORT tinylogTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  if(TINYLOG_BUILD_TOOLS)
    install(TARGETS tinylog-cat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()

  set(TINYLOG_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/tinylog)
  install(EXPORT tinylogTargets NAMESPACE tinylog:: DESTINATION ${TINYLOG_CMAKE_DIR})
  configure_package_config_file(cmake/tinylogConfig.cmake.in
    ${PROJECT_BINARY_DIR}/tinylogConfig.cmake
    INSTALL_DESTINATION ${TINYLOG_CMAKE_DIR})
  # Before 1.0 a minor release may break compatibility.
  if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(TINYLOG_COMPATIBILITY SameMinorVersion)
  else()
    set(TINYLOG_COMPATIBILITY SameMajorVersion)
  endif()
  write_basic_package_version_file(${PROJECT_BINARY_DIR}/tinylogConfigVersion.cmake
    COMPATIBILITY ${TINYLOG_COMPATIBILITY})
  install(FILES
    ${PROJECT_BINARY_DIR}/tinylogConfig.cmake
    ${PROJECT_BINARY_DIR}/tinylogConfigVersion.cmake
    DESTINATION ${TINYLOG_CMAKE_DIR})

  if(TINYLOG_BUILD_SHARED)
    set(TINYLOG_PC_CFLAGS "-DTINYLOG_SHARED")
  else()
    set(TINYLOG_PC_CFLAGS "")
  endif()
  configure_file(cmake/tinylog.pc.in ${PROJECT_BINARY_DIR}/tinylog.pc @ONLY)
  install(FILES ${PROJECT_BINARY_DIR}/tinylog.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

  set(CPACK_PACKAGE_NAME tinylog)
  set(CPACK_PACKAGE_VENDOR "Lavrentii Ivanov")
  set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE)
  if(WIN32)
    set(CPACK_GENERATOR ZIP)
  else()
    set(CPACK_GENERATOR TGZ)
  endif()
  include(CPack)
endif()
