// Shared native code for all target platforms.
// The OS-specific implementations live in platform/*.inc and are compiled
// into this single translation unit (so CMake and the macOS forwarder in
// macos/Classes stay exactly as in the FFI plugin template).
#include "system_integrator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void si_copy(char* dst, size_t cap, const char* src) {
  if (cap == 0) return;
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

#if _WIN32
#include "platform/windows.inc"
#elif __APPLE__
#include "platform/macos.inc"  // tray/window: macos/Classes/system_integrator_tray.m
#else
#include "platform/linux.inc"
#endif
