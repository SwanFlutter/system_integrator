// Linux implementation.
// Storage: statvfs64 (libc) + /proc/self/mounts. Tray: StatusNotifierItem via
// libayatana-appindicator3 / libappindicator3 (dlopen'ed, talks DBus), with a
// GtkStatusIcon fallback. xdg-open, DBus FileManager1, shutdown/systemctl and
// XDG autostart are driven from Dart.
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64  // statvfs == statvfs64
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <mntent.h>
#include <sys/statvfs.h>
#include <unistd.h>

FFI_PLUGIN_EXPORT int32_t si_native_capabilities(void);

// ----------------------------------------------------------------- storage
FFI_PLUGIN_EXPORT int32_t si_get_disk_space(const char* path,
                                            si_disk_space_t* out) {
  struct statvfs s;
  unsigned long frsize;
  if (path == NULL || out == NULL || statvfs(path, &s) != 0) return SI_ERROR;
  frsize = s.f_frsize ? s.f_frsize : s.f_bsize;
  out->total_bytes = (int64_t)s.f_blocks * (int64_t)frsize;
  out->free_bytes = (int64_t)s.f_bavail * (int64_t)frsize;
  out->total_free_bytes = (int64_t)s.f_bfree * (int64_t)frsize;
  return SI_OK;
}

static int si_is_network_fs(const char* t) {
  static const char* kNet[] = {"nfs", "nfs4", "cifs", "smb3", "smbfs",
                               "sshfs", "fuse.sshfs", "9p", "fuse.rclone",
                               "davfs", "fuse.davfs2", "ceph", "glusterfs", NULL};
  int i;
  for (i = 0; kNet[i] != NULL; i++) {
    if (strcmp(t, kNet[i]) == 0) return 1;
  }
  return 0;
}

static int si_read_flag_file(const char* path) {
  FILE* f = fopen(path, "r");
  int c;
  if (f == NULL) return -1;
  c = fgetc(f);
  fclose(f);
  return c == '1' ? 1 : 0;
}

static int si_is_removable_device(const char* dev) {
  char real[PATH_MAX], sys[PATH_MAX], sysreal[PATH_MAX], file[PATH_MAX + 16];
  const char* name;
  int r;
  if (realpath(dev, real) == NULL) return 0;
  name = strrchr(real, '/');
  name = name ? name + 1 : real;
  snprintf(sys, sizeof(sys), "/sys/class/block/%s", name);
  if (realpath(sys, sysreal) == NULL) return 0;
  if (strstr(sysreal, "/usb") != NULL) return 1;
  snprintf(file, sizeof(file), "%s/removable", sysreal);
  r = si_read_flag_file(file);
  if (r >= 0) return r;
  // Partition: the flag lives on the parent disk.
  {
    char* slash = strrchr(sysreal, '/');
    if (slash != NULL) *slash = '\0';
    snprintf(file, sizeof(file), "%s/removable", sysreal);
    r = si_read_flag_file(file);
  }
  return r > 0;
}

static void si_label_for_device(const char* dev, char* out, size_t cap) {
  char real[PATH_MAX];
  DIR* d;
  struct dirent* e;
  out[0] = '\0';
  if (realpath(dev, real) == NULL) return;
  d = opendir("/dev/disk/by-label");
  if (d == NULL) return;
  while ((e = readdir(d)) != NULL) {
    char p[PATH_MAX], target[PATH_MAX];
    if (e->d_name[0] == '.') continue;
    snprintf(p, sizeof(p), "/dev/disk/by-label/%s", e->d_name);
    if (realpath(p, target) != NULL && strcmp(target, real) == 0) {
      // udev escapes spaces etc. as \x20
      size_t o = 0;
      const char* s = e->d_name;
      while (*s && o + 1 < cap) {
        unsigned int ch;
        if (s[0] == '\\' && s[1] == 'x' && sscanf(s + 2, "%2x", &ch) == 1) {
          out[o++] = (char)ch;
          s += 4;
        } else {
          out[o++] = *s++;
        }
      }
      out[o] = '\0';
      break;
    }
  }
  closedir(d);
}

typedef struct {
  char dev[256];
  char dir[1024];
  char type[32];
  int ro;
} si_mount;

FFI_PLUGIN_EXPORT int32_t si_list_volumes(si_volume_t* out, int32_t capacity) {
  FILE* f = setmntent("/proc/self/mounts", "r");
  struct mntent* m;
  si_mount* mounts = NULL;
  int n = 0, cap = 0, i;
  int32_t count = 0;
  if (f == NULL) return SI_ERROR;
  while ((m = getmntent(f)) != NULL) {  // getmntent decodes \040 escapes
    int is_block = strncmp(m->mnt_fsname, "/dev/", 5) == 0 &&
                   strncmp(m->mnt_fsname, "/dev/loop", 9) != 0;
    int j, dup = -1;
    if (!(is_block || si_is_network_fs(m->mnt_type) ||
          strcmp(m->mnt_type, "zfs") == 0)) {
      continue;
    }
    if (strcmp(m->mnt_type, "squashfs") == 0 ||
        strncmp(m->mnt_dir, "/snap/", 6) == 0 ||
        strncmp(m->mnt_dir, "/boot", 5) == 0 ||
        strncmp(m->mnt_dir, "/var/lib/docker", 15) == 0) {
      continue;
    }
    for (j = 0; j < n; j++) {
      if (strcmp(mounts[j].dev, m->mnt_fsname) == 0) dup = j;
    }
    if (dup >= 0) {  // btrfs subvolumes / bind mounts: keep the shortest path
      if (strlen(m->mnt_dir) < strlen(mounts[dup].dir)) {
        si_copy(mounts[dup].dir, sizeof(mounts[dup].dir), m->mnt_dir);
      }
      continue;
    }
    if (n == cap) {
      si_mount* grown;
      cap = cap ? cap * 2 : 16;
      grown = (si_mount*)realloc(mounts, (size_t)cap * sizeof(si_mount));
      if (grown == NULL) break;
      mounts = grown;
    }
    si_copy(mounts[n].dev, sizeof(mounts[n].dev), m->mnt_fsname);
    si_copy(mounts[n].dir, sizeof(mounts[n].dir), m->mnt_dir);
    si_copy(mounts[n].type, sizeof(mounts[n].type), m->mnt_type);
    mounts[n].ro = hasmntopt(m, "ro") != NULL;
    n++;
  }
  endmntent(f);

  for (i = 0; i < n; i++) {
    struct statvfs s;
    unsigned long frsize;
    si_mount* mt = &mounts[i];
    if (statvfs(mt->dir, &s) != 0 || s.f_blocks == 0) continue;
    if (count < capacity && out != NULL) {
      si_volume_t* v = &out[count];
      memset(v, 0, sizeof(*v));
      frsize = s.f_frsize ? s.f_frsize : s.f_bsize;
      si_copy(v->id, sizeof(v->id), mt->dev);
      si_copy(v->path, sizeof(v->path), mt->dir);
      si_copy(v->fs_type, sizeof(v->fs_type), mt->type);
      si_label_for_device(mt->dev, v->label, sizeof(v->label));
      if (v->label[0] == '\0') {
        const char* base = strrchr(mt->dir, '/');
        si_copy(v->label, sizeof(v->label),
                strcmp(mt->dir, "/") == 0 ? "File System" : (base ? base + 1 : mt->dir));
      }
      v->total_bytes = (int64_t)s.f_blocks * (int64_t)frsize;
      v->free_bytes = (int64_t)s.f_bavail * (int64_t)frsize;
      v->read_only = (mt->ro || (s.f_flag & ST_RDONLY)) ? 1 : 0;
      if (si_is_network_fs(mt->type)) {
        v->kind = SI_VOLUME_NETWORK;
      } else if (strncmp(mt->dir, "/media/", 7) == 0 ||
                 strncmp(mt->dir, "/run/media/", 11) == 0 ||
                 si_is_removable_device(mt->dev)) {
        v->kind = SI_VOLUME_REMOVABLE;
      } else {
        v->kind = SI_VOLUME_FIXED;
      }
    }
    count++;
  }
  free(mounts);
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

// -------------------------------------------------------------------- tray
typedef gpointer (*si_ai_new_fn)(const gchar*, const gchar*, int);
typedef void (*si_ai_set_status_fn)(gpointer, int);
typedef void (*si_ai_set_menu_fn)(gpointer, GtkMenu*);
typedef void (*si_ai_set_icon_full_fn)(gpointer, const gchar*, const gchar*);
typedef void (*si_ai_set_title_fn)(gpointer, const gchar*);
typedef void (*si_ai_set_label_fn)(gpointer, const gchar*, const gchar*);
typedef void (*si_ai_set_secondary_fn)(gpointer, GtkWidget*);

static struct {
  int tried, loaded;
  si_ai_new_fn new_fn;
  si_ai_set_status_fn set_status;
  si_ai_set_menu_fn set_menu;
  si_ai_set_icon_full_fn set_icon_full;
  si_ai_set_title_fn set_title;
  si_ai_set_label_fn set_label;
  si_ai_set_secondary_fn set_secondary;
} g_ai;

#define SI_AI_ACTIVE 1
#define SI_AI_PASSIVE 0

static int si_load_appindicator(void) {
  static const char* libs[] = {"libayatana-appindicator3.so.1",
                               "libappindicator3.so.1", NULL};
  int i;
  if (g_ai.tried) return g_ai.loaded;
  g_ai.tried = 1;
  for (i = 0; libs[i] != NULL; i++) {
    void* h = dlopen(libs[i], RTLD_LAZY | RTLD_LOCAL);
    if (h == NULL) continue;
    g_ai.new_fn = (si_ai_new_fn)dlsym(h, "app_indicator_new");
    g_ai.set_status = (si_ai_set_status_fn)dlsym(h, "app_indicator_set_status");
    g_ai.set_menu = (si_ai_set_menu_fn)dlsym(h, "app_indicator_set_menu");
    g_ai.set_icon_full = (si_ai_set_icon_full_fn)dlsym(h, "app_indicator_set_icon_full");
    g_ai.set_title = (si_ai_set_title_fn)dlsym(h, "app_indicator_set_title");
    g_ai.set_label = (si_ai_set_label_fn)dlsym(h, "app_indicator_set_label");
    g_ai.set_secondary =
        (si_ai_set_secondary_fn)dlsym(h, "app_indicator_set_secondary_activate_target");
    if (g_ai.new_fn && g_ai.set_status && g_ai.set_menu) {
      g_ai.loaded = 1;
      return 1;
    }
    dlclose(h);
  }
  return 0;
}

FFI_PLUGIN_EXPORT int32_t si_native_capabilities(void) {
  int32_t caps = SI_CAP_DISK_SPACE | SI_CAP_LIST_VOLUMES | SI_CAP_TRAY |
                 SI_CAP_WINDOW | SI_CAP_FLASH_WINDOW;
  // AppIndicator: left click always opens the menu.
  if (!si_load_appindicator()) caps |= SI_CAP_TRAY_LEFT_CLICK;
  return caps;
}

static si_tray_callback g_tray_cb = NULL;
static gpointer g_indicator = NULL;      // AppIndicator*
static GtkStatusIcon* g_status_icon = NULL;  // fallback
static GtkWidget* g_menu = NULL;
static gchar* g_tooltip = NULL;
static gchar* g_progress = NULL;
static gchar* g_icon_path = NULL;

enum { SI_JOB_SHOW, SI_JOB_HIDE, SI_JOB_TOOLTIP, SI_JOB_PROGRESS, SI_JOB_MENU,
       SI_JOB_WIN_HIDE, SI_JOB_WIN_RESTORE, SI_JOB_WIN_FLASH, SI_JOB_DISPOSE };

typedef struct {
  int op;
  gchar* a;
  gchar* b;
  gchar** labels;
  int32_t* flags;
  int32_t count;
} si_job;

static GtkWindow* si_main_window(void) {
  GList* list = gtk_window_list_toplevels();
  GList* l;
  GtkWindow* found = NULL;
  for (l = list; l != NULL; l = l->next) {
    GtkWindow* w = GTK_WINDOW(l->data);
    if (gtk_window_get_window_type(w) == GTK_WINDOW_TOPLEVEL &&
        gtk_window_get_transient_for(w) == NULL) {
      found = w;
      break;
    }
  }
  g_list_free(list);
  return found;
}

static void si_on_menu_item(GtkMenuItem* item, gpointer data) {
  (void)item;
  if (g_tray_cb != NULL) g_tray_cb(SI_TRAY_EVENT_MENU, GPOINTER_TO_INT(data));
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void si_on_status_activate(GtkStatusIcon* icon, gpointer data) {
  (void)icon;
  (void)data;
  if (g_tray_cb != NULL) g_tray_cb(SI_TRAY_EVENT_CLICK, -1);
}
static void si_on_status_popup(GtkStatusIcon* icon, guint button, guint time,
                               gpointer data) {
  (void)icon;
  (void)button;
  (void)time;
  (void)data;
  if (g_menu != NULL) gtk_menu_popup_at_pointer(GTK_MENU(g_menu), NULL);
}
G_GNUC_END_IGNORE_DEPRECATIONS

static void si_apply_text(void) {
  gchar* text = (g_progress && g_progress[0])
                    ? g_strdup_printf("%s \u2014 %s", g_tooltip ? g_tooltip : "", g_progress)
                    : g_strdup(g_tooltip ? g_tooltip : "");
  if (g_indicator != NULL) {
    // AppIndicator has no tooltip / progress: title + panel label instead.
    if (g_ai.set_title) g_ai.set_title(g_indicator, text);
    if (g_ai.set_label) g_ai.set_label(g_indicator, g_progress ? g_progress : "", "100%");
  }
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  if (g_status_icon != NULL) gtk_status_icon_set_tooltip_text(g_status_icon, text);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_free(text);
}

static GtkWidget* si_build_menu(gchar** labels, int32_t* flags, int32_t count) {
  GtkWidget* menu = gtk_menu_new();
  GtkWidget* first = NULL;
  int32_t i;
  for (i = 0; i < count; i++) {
    GtkWidget* mi;
    if (flags[i] & SI_MENU_SEPARATOR) {
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
      continue;
    }
    if (flags[i] & SI_MENU_CHECKED) {
      mi = gtk_check_menu_item_new_with_label(labels[i]);
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), TRUE);
    } else {
      mi = gtk_menu_item_new_with_label(labels[i]);
    }
    gtk_widget_set_sensitive(mi, (flags[i] & SI_MENU_ENABLED) != 0);
    g_signal_connect(mi, "activate", G_CALLBACK(si_on_menu_item), GINT_TO_POINTER(i));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    if (first == NULL) first = mi;
  }
  gtk_widget_show_all(menu);
  g_object_ref_sink(menu);
  // Middle click on an AppIndicator activates the first item (e.g. "Show").
  if (g_indicator != NULL && g_ai.set_secondary && first != NULL) {
    g_ai.set_secondary(g_indicator, first);
  }
  return menu;
}

static void si_do_show(void) {
  const gchar* icon = g_icon_path ? g_icon_path : "application-x-executable";
  if (g_indicator == NULL && g_status_icon == NULL) {
    if (si_load_appindicator()) {
      gchar* id = g_strdup_printf("system_integrator_%d", (int)getpid());
      g_indicator = g_ai.new_fn(id, icon, 0 /* APPLICATION_STATUS */);
      g_free(id);
      if (g_menu == NULL) g_menu = si_build_menu(NULL, NULL, 0);
      g_ai.set_menu(g_indicator, GTK_MENU(g_menu));
    } else {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      g_status_icon = gtk_status_icon_new();
      g_signal_connect(g_status_icon, "activate", G_CALLBACK(si_on_status_activate), NULL);
      g_signal_connect(g_status_icon, "popup-menu", G_CALLBACK(si_on_status_popup), NULL);
      G_GNUC_END_IGNORE_DEPRECATIONS
    }
  }
  if (g_indicator != NULL) {
    if (g_ai.set_icon_full) g_ai.set_icon_full(g_indicator, icon, "tray");
    g_ai.set_status(g_indicator, SI_AI_ACTIVE);
  }
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  if (g_status_icon != NULL) {
    if (g_file_test(icon, G_FILE_TEST_EXISTS)) {
      gtk_status_icon_set_from_file(g_status_icon, icon);
    } else {
      gtk_status_icon_set_from_icon_name(g_status_icon, icon);
    }
    gtk_status_icon_set_visible(g_status_icon, TRUE);
  }
  G_GNUC_END_IGNORE_DEPRECATIONS
  si_apply_text();
}

static void si_do_hide(void) {
  if (g_indicator != NULL) g_ai.set_status(g_indicator, SI_AI_PASSIVE);
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  if (g_status_icon != NULL) gtk_status_icon_set_visible(g_status_icon, FALSE);
  G_GNUC_END_IGNORE_DEPRECATIONS
}

/// Runs on the GTK main thread.
static gboolean si_run_job(gpointer data) {
  si_job* j = (si_job*)data;
  GtkWindow* w;
  int32_t i;
  switch (j->op) {
    case SI_JOB_SHOW:
      if (j->a != NULL && j->a[0] != '\0') {
        g_free(g_icon_path);
        g_icon_path = g_strdup(j->a);
      }
      if (j->b != NULL) {
        g_free(g_tooltip);
        g_tooltip = g_strdup(j->b);
      }
      si_do_show();
      break;
    case SI_JOB_HIDE:
      si_do_hide();
      break;
    case SI_JOB_TOOLTIP:
      g_free(g_tooltip);
      g_tooltip = g_strdup(j->a ? j->a : "");
      si_apply_text();
      break;
    case SI_JOB_PROGRESS:
      g_free(g_progress);
      g_progress = g_strdup(j->a ? j->a : "");
      si_apply_text();
      break;
    case SI_JOB_MENU: {
      GtkWidget* menu = si_build_menu(j->labels, j->flags, j->count);
      if (g_indicator != NULL) g_ai.set_menu(g_indicator, GTK_MENU(menu));
      if (g_menu != NULL) g_object_unref(g_menu);
      g_menu = menu;
      break;
    }
    case SI_JOB_WIN_HIDE:
      w = si_main_window();
      if (w != NULL) gtk_widget_hide(GTK_WIDGET(w));
      break;
    case SI_JOB_WIN_RESTORE:
      w = si_main_window();
      if (w != NULL) {
        gtk_widget_show(GTK_WIDGET(w));
        gtk_window_deiconify(w);
        gtk_window_present(w);
      }
      break;
    case SI_JOB_WIN_FLASH:
      w = si_main_window();  // X11 urgency hint; ignored by most Wayland compositors
      if (w != NULL) gtk_window_set_urgency_hint(w, TRUE);
      break;
    case SI_JOB_DISPOSE:
      si_do_hide();
      break;
  }
  g_free(j->a);
  g_free(j->b);
  for (i = 0; i < j->count; i++) g_free(j->labels[i]);
  g_free(j->labels);
  g_free(j->flags);
  g_free(j);
  return G_SOURCE_REMOVE;
}

static int32_t si_post(int op, const char* a, const char* b) {
  si_job* j = g_new0(si_job, 1);
  j->op = op;
  j->a = a ? g_strdup(a) : NULL;
  j->b = b ? g_strdup(b) : NULL;
  g_main_context_invoke(NULL, si_run_job, j);
  return SI_OK;
}

FFI_PLUGIN_EXPORT int32_t si_tray_init(si_tray_callback callback) {
  g_tray_cb = callback;
  return SI_OK;
}
FFI_PLUGIN_EXPORT int32_t si_tray_show(const char* icon_path, const char* tooltip) {
  return si_post(SI_JOB_SHOW, icon_path, tooltip);
}
FFI_PLUGIN_EXPORT int32_t si_tray_hide(void) { return si_post(SI_JOB_HIDE, NULL, NULL); }
FFI_PLUGIN_EXPORT int32_t si_tray_set_tooltip(const char* tooltip) {
  return si_post(SI_JOB_TOOLTIP, tooltip ? tooltip : "", NULL);
}
FFI_PLUGIN_EXPORT int32_t si_tray_set_progress(int32_t value, const char* label) {
  return si_post(SI_JOB_PROGRESS, value == SI_PROGRESS_NONE ? "" : (label ? label : ""), NULL);
}
FFI_PLUGIN_EXPORT int32_t si_tray_set_menu(const char** labels, const int32_t* flags,
                                           int32_t count) {
  si_job* j = g_new0(si_job, 1);
  int32_t i;
  j->op = SI_JOB_MENU;
  j->count = count > 0 ? count : 0;
  j->labels = g_new0(gchar*, j->count + 1);
  j->flags = g_new0(int32_t, j->count + 1);
  for (i = 0; i < j->count; i++) {
    j->labels[i] = g_strdup(labels[i] ? labels[i] : "");
    j->flags[i] = flags[i];
  }
  g_main_context_invoke(NULL, si_run_job, j);
  return SI_OK;
}
FFI_PLUGIN_EXPORT void si_tray_dispose(void) {
  g_tray_cb = NULL;
  si_post(SI_JOB_DISPOSE, NULL, NULL);
}

// ------------------------------------------------------------------ window
FFI_PLUGIN_EXPORT int32_t si_window_hide(void) { return si_post(SI_JOB_WIN_HIDE, NULL, NULL); }
FFI_PLUGIN_EXPORT int32_t si_window_restore(void) {
  return si_post(SI_JOB_WIN_RESTORE, NULL, NULL);
}
FFI_PLUGIN_EXPORT int32_t si_window_flash(void) { return si_post(SI_JOB_WIN_FLASH, NULL, NULL); }
