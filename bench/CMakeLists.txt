add_executable(tinylog_bench bench_main.cc bench_tinylog.cc)
target_link_libraries(tinylog_bench PRIVATE tinylog::tinylog)
tinylog_warnings(tinylog_bench)

# Same scenarios with spdlog and Quill, to compare on the same machine.
if(TINYLOG_BENCH_COMPARE)
  # SYSTEM keeps their headers out of our warning flags. The Quill tarball
  # contains a symlink, so extract it where symlinks work (Linux/macOS/CI).
  cmake_minimum_required(VERSION 3.25)
  include(FetchContent)
  set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SYSTEM
    EXCLUDE_FROM_ALL)
  FetchContent_Declare(quill
    URL https://github.com/odygrd/quill/archive/refs/tags/v13.0.0.tar.gz
    URL_HASH SHA256=88b4a1542125577a4d51cf444c51e34d63618c422ba6a4fa9bd23894b49d696b
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SYSTEM
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(spdlog quill)
  target_sources(tinylog_bench PRIVATE bench_spdlog.cc bench_quill.cc)
  target_link_libraries(tinylog_bench PRIVATE spdlog::spdlog quill::quill)
  target_compile_definitions(tinylog_bench PRIVATE TINYLOG_BENCH_COMPARE)
endif()
