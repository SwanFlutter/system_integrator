#ifndef SYSTEM_INTEGRATOR_H_
#define SYSTEM_INTEGRATOR_H_

#include <stdint.h>

#if _WIN32
#define FFI_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FFI_PLUGIN_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Result codes
// ---------------------------------------------------------------------------

/// Operation succeeded.
#define SI_OK 0
/// Operation failed.
#define SI_ERROR -1
/// Not implemented natively on this OS; the Dart core uses Process.run.
#define SI_UNSUPPORTED -2

// ---------------------------------------------------------------------------
// Native capability bits returned by si_native_capabilities().
// ---------------------------------------------------------------------------
#define SI_CAP_OPEN_FILE 1
#define SI_CAP_REVEAL_IN_PLACE 2
#define SI_CAP_DISK_SPACE 4
#define SI_CAP_LIST_VOLUMES 8
#define SI_CAP_SHUTDOWN 16
#define SI_CAP_SLEEP 32
#define SI_CAP_LAUNCH_AT_STARTUP 64
#define SI_CAP_TRAY 128
#define SI_CAP_TRAY_NATIVE_PROGRESS 256
#define SI_CAP_TRAY_LEFT_CLICK 512
#define SI_CAP_WINDOW 1024
#define SI_CAP_FLASH_WINDOW 2048

// ---------------------------------------------------------------------------
// Volume kinds (si_volume.kind)
// ---------------------------------------------------------------------------
#define SI_VOLUME_UNKNOWN 0
#define SI_VOLUME_FIXED 1
#define SI_VOLUME_REMOVABLE 2
#define SI_VOLUME_NETWORK 3

// ---------------------------------------------------------------------------
// Tray
// ---------------------------------------------------------------------------
/// Left click on the tray icon.
#define SI_TRAY_EVENT_CLICK 1
/// A context menu item was selected (item_index = position in the menu).
#define SI_TRAY_EVENT_MENU 2

#define SI_MENU_ENABLED 1
#define SI_MENU_CHECKED 2
#define SI_MENU_SEPARATOR 4

/// si_tray_set_progress: hide progress.
#define SI_PROGRESS_NONE -2
/// si_tray_set_progress: indeterminate progress.
#define SI_PROGRESS_INDETERMINATE -1

/// Disk space of the volume containing a path. All values in bytes.
typedef struct si_disk_space {
  int64_t total_bytes;
  /// Bytes available to the calling (unprivileged) user.
  int64_t free_bytes;
  /// Total free bytes including root-reserved blocks / quota.
  int64_t total_free_bytes;
} si_disk_space_t;

/// One mounted volume. Strings are NUL-terminated UTF-8.
typedef struct si_volume {
  char id[128];
  char label[256];
  char path[1024];
  char fs_type[32];
  int64_t total_bytes;
  int64_t free_bytes;
  int32_t kind;
  int32_t read_only;
} si_volume_t;

/// Called on the native UI/tray thread; bind with NativeCallable.listener.
typedef void (*si_tray_callback)(int32_t event, int32_t item_index);

/// Bitmask of SI_CAP_* implemented natively on the current OS.
FFI_PLUGIN_EXPORT int32_t si_native_capabilities(void);

// ---------------------------------------------------------------------------
// Storage (all OS). May block on network volumes: call from a helper isolate.
// ---------------------------------------------------------------------------

/// Win: GetDiskFreeSpaceExW. macOS: statfs (libSystem). Linux: statvfs64 (libc).
FFI_PLUGIN_EXPORT int32_t si_get_disk_space(const char* path, si_disk_space_t* out);

/// Fills up to `capacity` volumes and returns how many exist (can be > capacity),
/// or SI_ERROR.
FFI_PLUGIN_EXPORT int32_t si_list_volumes(si_volume_t* out, int32_t capacity);

// ---------------------------------------------------------------------------
// Files (native on Windows; SI_UNSUPPORTED elsewhere -> open / xdg-open).
// ---------------------------------------------------------------------------

/// Win: ShellExecuteW("open").
FFI_PLUGIN_EXPORT int32_t si_open_file(const char* path);

/// Win: SHOpenFolderAndSelectItems (Explorer with the file selected).
FFI_PLUGIN_EXPORT int32_t si_reveal_in_file_manager(const char* path);

// ---------------------------------------------------------------------------
// Power (native on Windows; SI_UNSUPPORTED elsewhere -> osascript / shutdown).
// ---------------------------------------------------------------------------

/// Win: InitiateSystemShutdownExW with a timeout (no admin needed).
FFI_PLUGIN_EXPORT int32_t si_schedule_shutdown(int32_t seconds);

/// Win: AbortSystemShutdownW.
FFI_PLUGIN_EXPORT int32_t si_cancel_shutdown(void);

/// Win: SetSuspendState(FALSE, FALSE, FALSE).
FFI_PLUGIN_EXPORT int32_t si_sleep_now(void);

// ---------------------------------------------------------------------------
// Launch at startup (native on Windows: HKCU\...\Run; Dart writes files elsewhere)
// ---------------------------------------------------------------------------

/// `command` == NULL removes the entry.
FFI_PLUGIN_EXPORT int32_t si_set_launch_at_startup(const char* app_id, const char* command);

/// 1 if registered, 0 if not, SI_UNSUPPORTED elsewhere.
FFI_PLUGIN_EXPORT int32_t si_is_launch_at_startup(const char* app_id);

// ---------------------------------------------------------------------------
// Tray (the only UI-bound module). Thread-safe: work is marshalled to the
// native UI thread (Win: dedicated tray thread, macOS: main queue,
// Linux: GTK main context).
// ---------------------------------------------------------------------------

FFI_PLUGIN_EXPORT int32_t si_tray_init(si_tray_callback callback);

/// icon_path: .ico on Windows, .png on macOS/Linux; NULL keeps the current icon.
FFI_PLUGIN_EXPORT int32_t si_tray_show(const char* icon_path, const char* tooltip);

FFI_PLUGIN_EXPORT int32_t si_tray_hide(void);

FFI_PLUGIN_EXPORT int32_t si_tray_set_tooltip(const char* tooltip);

/// value: 0..100, SI_PROGRESS_INDETERMINATE or SI_PROGRESS_NONE.
/// label is shown as text where the OS has no native progress (macOS/Linux).
FFI_PLUGIN_EXPORT int32_t si_tray_set_progress(int32_t value, const char* label);

/// flags[i] is a combination of SI_MENU_* bits.
FFI_PLUGIN_EXPORT int32_t si_tray_set_menu(const char** labels, const int32_t* flags, int32_t count);

FFI_PLUGIN_EXPORT void si_tray_dispose(void);

// ---------------------------------------------------------------------------
// Window helpers (act on the Flutter main window)
// ---------------------------------------------------------------------------

/// Hides the window (minimize to tray).
FFI_PLUGIN_EXPORT int32_t si_window_hide(void);

FFI_PLUGIN_EXPORT int32_t si_window_restore(void);

/// Win: FlashWindowEx. macOS: requestUserAttention. Linux: X11 urgency hint.
FFI_PLUGIN_EXPORT int32_t si_window_flash(void);

#ifdef __cplusplus
}
#endif

#endif  // SYSTEM_INTEGRATOR_H_
