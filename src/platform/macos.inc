// macOS implementation: storage via statfs/getfsstat in libSystem.
// Tray + window helpers are Objective-C (NSStatusItem / NSApp) and live in
// src/system_integrator_tray.m, forwarded by macos/Classes.
// open / open -R / osascript / pmset / LaunchAgents are driven from Dart.
#include <dirent.h>
#include <limits.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <unistd.h>

#ifndef MNT_REMOVABLE
#define MNT_REMOVABLE 0x00000200
#endif

FFI_PLUGIN_EXPORT int32_t si_native_capabilities(void) {
  return SI_CAP_DISK_SPACE | SI_CAP_LIST_VOLUMES | SI_CAP_TRAY |
         SI_CAP_TRAY_LEFT_CLICK | SI_CAP_WINDOW | SI_CAP_FLASH_WINDOW;
}

FFI_PLUGIN_EXPORT int32_t si_get_disk_space(const char* path,
                                            si_disk_space_t* out) {
  struct statfs s;  // 64-bit inode layout (== statfs64) on all current SDKs
  if (path == NULL || out == NULL || statfs(path, &s) != 0) return SI_ERROR;
  out->total_bytes = (int64_t)s.f_blocks * (int64_t)s.f_bsize;
  out->free_bytes = (int64_t)s.f_bavail * (int64_t)s.f_bsize;
  out->total_free_bytes = (int64_t)s.f_bfree * (int64_t)s.f_bsize;
  return SI_OK;
}

/// /Volumes contains a symlink "<Boot volume name>" -> "/".
static void si_boot_volume_name(char* out, size_t cap) {
  DIR* d = opendir("/Volumes");
  struct dirent* e;
  si_copy(out, cap, "Macintosh HD");
  if (d == NULL) return;
  while ((e = readdir(d)) != NULL) {
    char p[PATH_MAX];
    char target[PATH_MAX];
    ssize_t n;
    if (e->d_name[0] == '.') continue;
    snprintf(p, sizeof(p), "/Volumes/%s", e->d_name);
    n = readlink(p, target, sizeof(target) - 1);
    if (n > 0) {
      target[n] = '\0';
      if (strcmp(target, "/") == 0) {
        si_copy(out, cap, e->d_name);
        break;
      }
    }
  }
  closedir(d);
}

FFI_PLUGIN_EXPORT int32_t si_list_volumes(si_volume_t* out, int32_t capacity) {
  int n = getfsstat(NULL, 0, MNT_NOWAIT);
  int cap, i;
  int32_t count = 0;
  struct statfs* buf;
  if (n <= 0) return SI_ERROR;
  cap = n + 8;
  buf = (struct statfs*)calloc((size_t)cap, sizeof(struct statfs));
  if (buf == NULL) return SI_ERROR;
  n = getfsstat(buf, (int)(cap * (int)sizeof(struct statfs)), MNT_NOWAIT);
  for (i = 0; i < n; i++) {
    struct statfs* s = &buf[i];
    const char* mnt = s->f_mntonname;
    int is_root = strcmp(mnt, "/") == 0;
    if (!is_root && (s->f_flags & MNT_DONTBROWSE)) continue;
    if (strcmp(s->f_fstypename, "devfs") == 0 ||
        strcmp(s->f_fstypename, "autofs") == 0 ||
        strcmp(s->f_fstypename, "nullfs") == 0) {
      continue;
    }
    if (strncmp(mnt, "/System/Volumes/", 16) == 0 || s->f_blocks == 0) continue;
    if (count < capacity && out != NULL) {
      si_volume_t* v = &out[count];
      const char* base = strrchr(mnt, '/');
      memset(v, 0, sizeof(*v));
      si_copy(v->id, sizeof(v->id), mnt);
      si_copy(v->path, sizeof(v->path), mnt);
      si_copy(v->fs_type, sizeof(v->fs_type), s->f_fstypename);
      if (is_root) {
        si_boot_volume_name(v->label, sizeof(v->label));
      } else {
        si_copy(v->label, sizeof(v->label), base ? base + 1 : mnt);
      }
      v->total_bytes = (int64_t)s->f_blocks * (int64_t)s->f_bsize;
      v->free_bytes = (int64_t)s->f_bavail * (int64_t)s->f_bsize;
      v->read_only = (!is_root && (s->f_flags & MNT_RDONLY)) ? 1 : 0;
      if (!(s->f_flags & MNT_LOCAL)) {
        v->kind = SI_VOLUME_NETWORK;
      } else if ((s->f_flags & MNT_REMOVABLE) || strncmp(mnt, "/Volumes/", 9) == 0) {
        v->kind = SI_VOLUME_REMOVABLE;  // external disks are mounted in /Volumes
      } else {
        v->kind = SI_VOLUME_FIXED;
      }
    }
    count++;
  }
  free(buf);
  return count;
}

// Done from Dart with built-in tools (see lib/src/os_commands.dart).
FFI_PLUGIN_EXPORT int32_t si_open_file(const char* path) {
  (void)path;
  return SI_UNSUPPORTED;
}
FFI_PLUGIN_EXPORT int32_t si_reveal_in_file_manager(const char* path) {
  (void)path;
  return SI_UNSUPPORTED;
}
FFI_PLUGIN_EXPORT int32_t si_schedule_shutdown(int32_t seconds) {
  (void)seconds;
  return SI_UNSUPPORTED;
}
FFI_PLUGIN_EXPORT int32_t si_cancel_shutdown(void) { return SI_UNSUPPORTED; }
FFI_PLUGIN_EXPORT int32_t si_sleep_now(void) { return SI_UNSUPPORTED; }
FFI_PLUGIN_EXPORT int32_t si_set_launch_at_startup(const char* app_id,
                                                   const char* command) {
  (void)app_id;
  (void)command;
  return SI_UNSUPPORTED;
}
FFI_PLUGIN_EXPORT int32_t si_is_launch_at_startup(const char* app_id) {
  (void)app_id;
  return SI_UNSUPPORTED;
}
