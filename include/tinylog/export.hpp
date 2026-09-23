#pragma once

/*! \file Symbol visibility for the tinylog library.
 *
 * Static linking is the default: nothing needs to be defined to compile the
 * sources straight into an application. The CMake `tinylog_shared` target
 * propagates TINYLOG_SHARED to its consumers, and defines TINYLOG_BUILDING
 * while compiling the library itself.
 */

#if defined(TINYLOG_SHARED)
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(TINYLOG_BUILDING)
#define TINYLOG_API __declspec(dllexport)
#else
#define TINYLOG_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define TINYLOG_API __attribute__((visibility("default")))
#else
#define TINYLOG_API
#endif
#else
#define TINYLOG_API
#endif
