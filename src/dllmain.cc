#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "sinks.hpp"

// Nothing here may take locks, start or join threads, or touch files: DllMain
// runs under the loader lock. The logger initializes lazily on first use.
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID reserved)
{
  switch (reason)
  {
  case DLL_PROCESS_DETACH:
    // A non-null `reserved` means the process is exiting: every other thread
    // is already gone, possibly while holding a logger lock, so the static
    // finalizer must not wait for or lock anything.
    if (reserved != nullptr)
      tinylog::detail::g_process_exiting.store(true);
    break;
  default:
    break;
  }
  return TRUE;
}
#endif
