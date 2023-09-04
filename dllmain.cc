#ifdef _WIN32
#include <Windows.h>
#include "log.hpp"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
  switch (ul_reason_for_call)
  {
  case DLL_PROCESS_ATTACH:
// Initialization code for when the DLL is loaded
#ifdef DEBUG
    Log::get().set_level(TraceSeverity::debug);
#else
    Log::get().set_level(TraceSeverity::info);
#endif
    Log::get().set_level(TraceSeverity::debug);
    break;
  case DLL_PROCESS_DETACH:
    // Cleanup code for when the DLL is unloaded
    break;
  case DLL_THREAD_ATTACH:
    // Thread-specific initialization code
    break;
  case DLL_THREAD_DETACH:
    // Thread-specific cleanup code
    break;
  }
  return TRUE;
}
#endif