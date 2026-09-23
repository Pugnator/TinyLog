#ifndef TINYLOG_TINYLOG_H
#define TINYLOG_TINYLOG_H

/*! \file C interface: a stable ABI for other compilers, languages and LoadLibrary users.
 *
 * Levels: 0 trace, 1 debug, 2 info, 3 warning, 4 error, 5 critical, 6 fatal, 7 off.
 */

#include <stddef.h>

#if defined(TINYLOG_SHARED)
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(TINYLOG_BUILDING)
#define TINYLOG_C_API __declspec(dllexport)
#else
#define TINYLOG_C_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define TINYLOG_C_API __attribute__((visibility("default")))
#else
#define TINYLOG_C_API
#endif
#else
#define TINYLOG_C_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * Configures the logger from "key=value;key=value" settings (see
   * tinylog::apply_config_string); NULL or "" gives the defaults.
   * Returns 0 on success, -1 on failure (see tinylog_last_error()).
   */
  TINYLOG_C_API int tinylog_init(const char *settings);

  //! Message of the last failed call on this thread ("" if none).
  TINYLOG_C_API const char *tinylog_last_error(void);

  TINYLOG_C_API void tinylog_shutdown(void);
  TINYLOG_C_API void tinylog_flush(void);
  TINYLOG_C_API void tinylog_set_level(int level);
  TINYLOG_C_API int tinylog_should_log(int level);

  //! Logs `length` bytes of UTF-8 text. `file`/`function` may be NULL; they must be string literals otherwise.
  TINYLOG_C_API void tinylog_write(int level, const char *file, int line, const char *function,
                                   const char *message, size_t length);

  //! "MAJOR.MINOR.PATCH"
  TINYLOG_C_API const char *tinylog_version(void);

#ifdef __cplusplus
}
#endif

#endif
