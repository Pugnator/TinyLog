include(FetchContent)

find_package(GTest 1.14 CONFIG QUIET)
if(NOT GTest_FOUND)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(tinylog_tests
  test_c_api.cc
  test_compat.cc
  test_config.cc
  test_core.cc
  test_file_sink.cc
  test_format.cc)
target_link_libraries(tinylog_tests PRIVATE tinylog::tinylog GTest::gtest_main)
tinylog_warnings(tinylog_tests)
if(MSVC)
  target_compile_options(tinylog_tests PRIVATE /bigobj)
endif()
# Each test runs in its own process: the logger is a process-wide singleton.
gtest_discover_tests(tinylog_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60
  PROPERTIES TIMEOUT 120 LABELS unit)

# Logging after main() returns: from an atexit handler, after exit() has
# already destroyed the main thread's thread_local objects. It used to write
# into a freed buffer; the sanitizer job reports that, and every job checks
# the line still arrives.
add_executable(tinylog_exit_log exit_log.cc)
target_link_libraries(tinylog_exit_log PRIVATE tinylog::tinylog)
tinylog_warnings(tinylog_exit_log)
add_test(NAME exit_log COMMAND tinylog_exit_log)
set_tests_properties(exit_log PROPERTIES LABELS unit TIMEOUT 60 PASS_REGULAR_EXPRESSION "logged after main 42")

# The DLL / shared object: catches symbols missing from the export list.
if(TINYLOG_BUILD_SHARED)
  add_executable(tinylog_shared_smoke shared_smoke.cc)
  target_link_libraries(tinylog_shared_smoke PRIVATE tinylog::shared)
  tinylog_warnings(tinylog_shared_smoke)
  add_test(NAME shared_smoke COMMAND tinylog_shared_smoke WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(shared_smoke PROPERTIES LABELS unit TIMEOUT 60)
endif()

# Short benchmark run: proves the profiling harness works; numbers are not judged here.
if(TINYLOG_BUILD_BENCH)
  add_test(NAME bench_smoke COMMAND tinylog_bench --quick WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(bench_smoke PROPERTIES LABELS perf TIMEOUT 300)
endif()
