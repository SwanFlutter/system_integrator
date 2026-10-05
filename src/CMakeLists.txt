# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

project(system_integrator_library VERSION 0.0.1 LANGUAGES C)

# platform/*.inc are #included by system_integrator.c (single translation unit).
add_library(system_integrator SHARED
  "system_integrator.c"
)

set_target_properties(system_integrator PROPERTIES
  PUBLIC_HEADER system_integrator.h
  OUTPUT_NAME "system_integrator"
)

target_compile_definitions(system_integrator PUBLIC DART_SHARED_LIB)

if (WIN32)
  # Win32 APIs used by platform/windows.inc (all available to standard users).
  target_compile_definitions(system_integrator PRIVATE UNICODE _UNICODE)
  target_link_libraries(system_integrator PRIVATE
    shell32 user32 ole32 advapi32 powrprof uuid)
elseif (UNIX AND NOT APPLE AND NOT ANDROID)
  # GTK is already a dependency of every Flutter Linux app (tray menus, window).
  # AppIndicator is dlopen()ed at runtime -> no build-time dependency.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SI_GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
  target_link_libraries(system_integrator PRIVATE PkgConfig::SI_GTK ${CMAKE_DL_LIBS})
endif()

if (ANDROID)
  # Support Android 15 16k page size
  target_link_options(system_integrator PRIVATE "-Wl,-z,max-page-size=16384")
endif()
