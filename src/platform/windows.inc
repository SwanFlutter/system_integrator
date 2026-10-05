// Windows implementation (Win32 API, no admin rights required).
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <powrprof.h>

#define SI_WM_TRAY (WM_APP + 0x51)
#define SI_WM_SHOW (WM_APP + 0x52)
#define SI_WM_HIDE (WM_APP + 0x53)
#define SI_WM_TOOLTIP (WM_APP + 0x54)
#define SI_WM_PROGRESS (WM_APP + 0x55)
#define SI_WM_MENU (WM_APP + 0x56)
#define SI_MENU_BASE 0x5100
#define SI_APP_ICON_ID 101  // IDI_APP_ICON of the Flutter runner
#ifndef ERROR_SHUTDOWN_IS_SCHEDULED
#define ERROR_SHUTDOWN_IS_SCHEDULED 1190L
#endif

// --------------------------------------------------------------- helpers
static wchar_t* si_wide(const char* s) {
  if (s == NULL) return NULL;
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t* w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
  if (w != NULL) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

static void si_utf8(const wchar_t* w, char* out, int cap) {
  out[0] = '\0';
  if (w == NULL) return;
  if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) <= 0) {
    out[0] = '\0';
  }
  out[cap - 1] = '\0';
}

static void si_enable_shutdown_privilege(void) {
  HANDLE token;
  TOKEN_PRIVILEGES tp;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    return;
  }
  if (LookupPrivilegeValueW(NULL, L"SeShutdownPrivilege",
                            &tp.Privileges[0].Luid)) {
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
  }
  CloseHandle(token);
}

typedef struct {
  DWORD pid;
  HWND found;
} si_find_ctx;

static BOOL CALLBACK si_enum_proc(HWND hwnd, LPARAM lp) {
  si_find_ctx* c = (si_find_ctx*)lp;
  DWORD pid = 0;
  wchar_t cls[64];
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != c->pid) return TRUE;
  if (GetClassNameW(hwnd, cls, 64) &&
      wcscmp(cls, L"FLUTTER_RUNNER_WIN32_WINDOW") == 0) {
    c->found = hwnd;
    return FALSE;
  }
  return TRUE;
}

/// Top-level Flutter window of this process (also when hidden).
static HWND si_main_window(void) {
  si_find_ctx c;
  c.pid = GetCurrentProcessId();
  c.found = NULL;
  EnumWindows(si_enum_proc, (LPARAM)&c);
  return c.found;
}

// ------------------------------------------------------------ capabilities
FFI_PLUGIN_EXPORT int32_t si_native_capabilities(void) {
  return SI_CAP_OPEN_FILE | SI_CAP_REVEAL_IN_PLACE | SI_CAP_DISK_SPACE |
         SI_CAP_LIST_VOLUMES | SI_CAP_SHUTDOWN | SI_CAP_SLEEP |
         SI_CAP_LAUNCH_AT_STARTUP | SI_CAP_TRAY | SI_CAP_TRAY_NATIVE_PROGRESS |
         SI_CAP_TRAY_LEFT_CLICK | SI_CAP_WINDOW | SI_CAP_FLASH_WINDOW;
}

// ----------------------------------------------------------------- storage
FFI_PLUGIN_EXPORT int32_t si_get_disk_space(const char* path,
                                            si_disk_space_t* out) {
  ULARGE_INTEGER avail, total, total_free;
  wchar_t* w;
  UINT prev;
  BOOL ok;
  if (out == NULL) return SI_ERROR;
  w = si_wide(path);
  if (w == NULL) return SI_ERROR;
  prev = SetErrorMode(SEM_FAILCRITICALERRORS);
  ok = GetDiskFreeSpaceExW(w, &avail, &total, &total_free);
  SetErrorMode(prev);
  free(w);
  if (!ok) return SI_ERROR;
  out->total_bytes = (int64_t)total.QuadPart;
  out->free_bytes = (int64_t)avail.QuadPart;
  out->total_free_bytes = (int64_t)total_free.QuadPart;
  return SI_OK;
}

FFI_PLUGIN_EXPORT int32_t si_list_volumes(si_volume_t* out, int32_t capacity) {
  wchar_t roots[1024];
  DWORD len = GetLogicalDriveStringsW(1024, roots);
  int32_t count = 0;
  UINT prev;
  const wchar_t* root;
  if (len == 0 || len > 1024) return SI_ERROR;
  prev = SetErrorMode(SEM_FAILCRITICALERRORS);
  for (root = roots; *root != L'\0'; root += wcslen(root) + 1) {
    UINT type = GetDriveTypeW(root);
    ULARGE_INTEGER avail, total, total_free;
    wchar_t name[MAX_PATH + 1] = L"";
    wchar_t fs[MAX_PATH + 1] = L"";
    DWORD flags = 0;
    if (type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) continue;
    // Fails for empty card readers / optical drives -> skip them.
    if (!GetDiskFreeSpaceExW(root, &avail, &total, &total_free)) continue;
    if (count < capacity && out != NULL) {
      si_volume_t* v = &out[count];
      char letter[3] = {(char)root[0], ':', '\0'};
      memset(v, 0, sizeof(*v));
      GetVolumeInformationW(root, name, MAX_PATH + 1, NULL, NULL, &flags, fs,
                            MAX_PATH + 1);
      si_copy(v->id, sizeof(v->id), letter);
      si_utf8(name, v->label, (int)sizeof(v->label));
      si_utf8(root, v->path, (int)sizeof(v->path));
      si_utf8(fs, v->fs_type, (int)sizeof(v->fs_type));
      v->total_bytes = (int64_t)total.QuadPart;
      v->free_bytes = (int64_t)avail.QuadPart;
      v->read_only = (flags & FILE_READ_ONLY_VOLUME) ? 1 : 0;
      switch (type) {
        case DRIVE_REMOVABLE:
        case DRIVE_CDROM:
          v->kind = SI_VOLUME_REMOVABLE;
          break;
        case DRIVE_REMOTE:
          v->kind = SI_VOLUME_NETWORK;
          break;
        case DRIVE_FIXED:
        case DRIVE_RAMDISK:
          v->kind = SI_VOLUME_FIXED;
          break;
        default:
          v->kind = SI_VOLUME_UNKNOWN;
      }
    }
    count++;
  }
  SetErrorMode(prev);
  return count;
}

// ------------------------------------------------------------------- files
FFI_PLUGIN_EXPORT int32_t si_open_file(const char* path) {
  wchar_t* w = si_wide(path);
  INT_PTR r;
  if (w == NULL) return SI_ERROR;
  r = (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
  free(w);
  return r > 32 ? SI_OK : SI_ERROR;
}

FFI_PLUGIN_EXPORT int32_t si_reveal_in_file_manager(const char* path) {
  wchar_t* w = si_wide(path);
  HRESULT init, hr = E_FAIL;
  PIDLIST_ABSOLUTE pidl;
  if (w == NULL) return SI_ERROR;
  init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  pidl = ILCreateFromPathW(w);
  if (pidl != NULL) {
    hr = SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
    ILFree(pidl);
  }
  if (SUCCEEDED(init)) CoUninitialize();
  free(w);
  return SUCCEEDED(hr) ? SI_OK : SI_ERROR;
}

// ------------------------------------------------------------------- power
FFI_PLUGIN_EXPORT int32_t si_schedule_shutdown(int32_t seconds) {
  DWORD reason = SHTDN_REASON_MAJOR_APPLICATION |
                 SHTDN_REASON_MINOR_MAINTENANCE | SHTDN_REASON_FLAG_PLANNED;
  wchar_t msg[] = L"Downloads finished. The computer will shut down.";
  BOOL ok;
  if (seconds < 0) seconds = 0;
  si_enable_shutdown_privilege();
  ok = InitiateSystemShutdownExW(NULL, msg, (DWORD)seconds, FALSE, FALSE,
                                 reason);
  if (!ok && (GetLastError() == ERROR_SHUTDOWN_IN_PROGRESS ||
              GetLastError() == ERROR_SHUTDOWN_IS_SCHEDULED)) {
    AbortSystemShutdownW(NULL);
    ok = InitiateSystemShutdownExW(NULL, msg, (DWORD)seconds, FALSE, FALSE,
                                   reason);
  }
  return ok ? SI_OK : SI_ERROR;
}

FFI_PLUGIN_EXPORT int32_t si_cancel_shutdown(void) {
  si_enable_shutdown_privilege();
  if (AbortSystemShutdownW(NULL)) return SI_OK;
  return GetLastError() == ERROR_NO_SHUTDOWN_IN_PROGRESS ? SI_OK : SI_ERROR;
}

FFI_PLUGIN_EXPORT int32_t si_sleep_now(void) {
  si_enable_shutdown_privilege();
  return SetSuspendState(FALSE, FALSE, FALSE) ? SI_OK : SI_ERROR;
}

// ----------------------------------------------------------------- startup
static const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

FFI_PLUGIN_EXPORT int32_t si_set_launch_at_startup(const char* app_id,
                                                   const char* command) {
  HKEY key;
  LONG r;
  wchar_t* name = si_wide(app_id);
  if (name == NULL) return SI_ERROR;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) !=
      ERROR_SUCCESS) {
    free(name);
    return SI_ERROR;
  }
  if (command == NULL) {
    r = RegDeleteValueW(key, name);
    if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
  } else {
    wchar_t* cmd = si_wide(command);
    r = cmd == NULL ? ERROR_INVALID_DATA
                    : RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)cmd,
                                     (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    free(cmd);
  }
  RegCloseKey(key);
  free(name);
  return r == ERROR_SUCCESS ? SI_OK : SI_ERROR;
}

FFI_PLUGIN_EXPORT int32_t si_is_launch_at_startup(const char* app_id) {
  HKEY key;
  LONG r;
  wchar_t* name = si_wide(app_id);
  if (name == NULL) return SI_ERROR;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) !=
      ERROR_SUCCESS) {
    free(name);
    return 0;
  }
  r = RegQueryValueExW(key, name, NULL, NULL, NULL, NULL);
  RegCloseKey(key);
  free(name);
  return r == ERROR_SUCCESS ? 1 : 0;
}

// -------------------------------------------------------------------- tray
// A dedicated thread owns a hidden window + message loop, so the tray works
// no matter which thread Dart runs on. Calls are marshalled with SendMessageW.
typedef struct {
  wchar_t** labels;
  int32_t* flags;
  int32_t count;
} si_menu_data;

static HANDLE g_tray_thread = NULL;
static HANDLE g_tray_ready = NULL;
static HWND g_tray_hwnd = NULL;
static si_tray_callback g_tray_cb = NULL;
static NOTIFYICONDATAW g_nid;
static BOOL g_tray_visible = FALSE;
static HICON g_icon = NULL;
static BOOL g_owns_icon = FALSE;
static UINT g_taskbar_created = 0;
static wchar_t g_tooltip[128] = L"";
static wchar_t g_progress[32] = L"";
static si_menu_data g_menu = {NULL, NULL, 0};
static ITaskbarList3* g_taskbar = NULL;

static void si_free_menu(si_menu_data* m) {
  int32_t i;
  for (i = 0; i < m->count; i++) free(m->labels[i]);
  free(m->labels);
  free(m->flags);
  m->labels = NULL;
  m->flags = NULL;
  m->count = 0;
}

static void si_apply_tip(void) {
  wchar_t tip[128];
  if (g_progress[0] != L'\0') {
    _snwprintf_s(tip, 128, _TRUNCATE, L"%ls \x2014 %ls", g_tooltip, g_progress);
  } else {
    wcsncpy_s(tip, 128, g_tooltip, _TRUNCATE);
  }
  wcsncpy_s(g_nid.szTip, 128, tip, _TRUNCATE);
}

static void si_tray_popup(HWND hwnd) {
  HMENU menu;
  POINT pt;
  UINT cmd;
  int32_t i;
  if (g_menu.count <= 0) return;
  menu = CreatePopupMenu();
  for (i = 0; i < g_menu.count; i++) {
    int32_t f = g_menu.flags[i];
    if (f & SI_MENU_SEPARATOR) {
      AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    } else {
      AppendMenuW(menu,
                  MF_STRING | ((f & SI_MENU_ENABLED) ? MF_ENABLED : MF_GRAYED) |
                      ((f & SI_MENU_CHECKED) ? MF_CHECKED : MF_UNCHECKED),
                  SI_MENU_BASE + (UINT)i, g_menu.labels[i]);
    }
  }
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);  // so the menu closes on outside click
  cmd = (UINT)TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
      pt.x, pt.y, 0, hwnd, NULL);
  PostMessageW(hwnd, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (cmd >= SI_MENU_BASE && cmd < SI_MENU_BASE + (UINT)g_menu.count &&
      g_tray_cb != NULL) {
    g_tray_cb(SI_TRAY_EVENT_MENU, (int32_t)(cmd - SI_MENU_BASE));
  }
}

static LRESULT si_do_show(const wchar_t* icon_path, const wchar_t* tooltip) {
  HICON icon = NULL;
  BOOL owns = FALSE;
  if (icon_path != NULL && icon_path[0] != L'\0') {
    icon = (HICON)LoadImageW(NULL, icon_path, IMAGE_ICON,
                             GetSystemMetrics(SM_CXSMICON),
                             GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
    owns = icon != NULL;
  }
  if (icon != NULL || g_icon == NULL) {
    if (icon == NULL) {
      icon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(SI_APP_ICON_ID));
    }
    if (icon == NULL) icon = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    if (g_owns_icon && g_icon != NULL && g_icon != icon) DestroyIcon(g_icon);
    g_icon = icon;
    g_owns_icon = owns;
  }
  if (tooltip != NULL) wcsncpy_s(g_tooltip, 128, tooltip, _TRUNCATE);
  g_nid.cbSize = sizeof(g_nid);
  g_nid.hWnd = g_tray_hwnd;
  g_nid.uID = 1;
  g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_nid.uCallbackMessage = SI_WM_TRAY;
  g_nid.hIcon = g_icon;
  si_apply_tip();
  if (Shell_NotifyIconW(g_tray_visible ? NIM_MODIFY : NIM_ADD, &g_nid)) {
    g_tray_visible = TRUE;
    return SI_OK;
  }
  return SI_ERROR;
}

static void si_do_progress(int32_t value) {
  HWND main = si_main_window();
  if (g_taskbar == NULL) {
    if (SUCCEEDED(CoCreateInstance(&CLSID_TaskbarList, NULL,
                                   CLSCTX_INPROC_SERVER, &IID_ITaskbarList3,
                                   (void**)&g_taskbar))) {
      if (FAILED(ITaskbarList3_HrInit(g_taskbar))) {
        ITaskbarList3_Release(g_taskbar);
        g_taskbar = NULL;
      }
    }
  }
  if (g_taskbar == NULL || main == NULL) return;
  if (value == SI_PROGRESS_NONE) {
    ITaskbarList3_SetProgressState(g_taskbar, main, TBPF_NOPROGRESS);
  } else if (value == SI_PROGRESS_INDETERMINATE) {
    ITaskbarList3_SetProgressState(g_taskbar, main, TBPF_INDETERMINATE);
  } else {
    ITaskbarList3_SetProgressState(g_taskbar, main, TBPF_NORMAL);
    ITaskbarList3_SetProgressValue(g_taskbar, main, (ULONGLONG)value, 100);
  }
}

static LRESULT CALLBACK si_tray_wndproc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp) {
  switch (msg) {
    case SI_WM_TRAY:
      if (LOWORD(lp) == WM_LBUTTONUP && g_tray_cb != NULL) {
        g_tray_cb(SI_TRAY_EVENT_CLICK, -1);
      } else if (LOWORD(lp) == WM_RBUTTONUP) {
        si_tray_popup(hwnd);
      }
      return 0;
    case SI_WM_SHOW:
      return si_do_show((const wchar_t*)wp, (const wchar_t*)lp);
    case SI_WM_HIDE:
      if (g_tray_visible) Shell_NotifyIconW(NIM_DELETE, &g_nid);
      g_tray_visible = FALSE;
      return SI_OK;
    case SI_WM_TOOLTIP:
      wcsncpy_s(g_tooltip, 128, lp ? (const wchar_t*)lp : L"", _TRUNCATE);
      if (g_tray_visible) {
        si_apply_tip();
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
      }
      return SI_OK;
    case SI_WM_PROGRESS:
      wcsncpy_s(g_progress, 32, lp ? (const wchar_t*)lp : L"", _TRUNCATE);
      si_do_progress((int32_t)wp);
      if (g_tray_visible) {
        si_apply_tip();
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
      }
      return SI_OK;
    case SI_WM_MENU: {
      si_menu_data* m = (si_menu_data*)lp;
      si_free_menu(&g_menu);
      g_menu = *m;  // take ownership
      return SI_OK;
    }
    case WM_DESTROY:
      if (g_tray_visible) Shell_NotifyIconW(NIM_DELETE, &g_nid);
      g_tray_visible = FALSE;
      PostQuitMessage(0);
      return 0;
    default:
      if (g_taskbar_created != 0 && msg == g_taskbar_created) {
        if (g_tray_visible) Shell_NotifyIconW(NIM_ADD, &g_nid);  // Explorer restarted
        return 0;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

static DWORD WINAPI si_tray_thread_main(LPVOID param) {
  WNDCLASSEXW wc;
  MSG msg;
  (void)param;
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = si_tray_wndproc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.lpszClassName = L"SystemIntegratorTrayWindow";
  RegisterClassExW(&wc);
  g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  g_tray_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0,
                                0, 0, NULL, NULL, wc.hInstance, NULL);
  SetEvent(g_tray_ready);
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (g_taskbar != NULL) {
    ITaskbarList3_Release(g_taskbar);
    g_taskbar = NULL;
  }
  if (g_owns_icon && g_icon != NULL) DestroyIcon(g_icon);
  g_icon = NULL;
  si_free_menu(&g_menu);
  g_tray_hwnd = NULL;
  CoUninitialize();
  return 0;
}

FFI_PLUGIN_EXPORT int32_t si_tray_init(si_tray_callback callback) {
  g_tray_cb = callback;
  if (g_tray_thread != NULL) return g_tray_hwnd ? SI_OK : SI_ERROR;
  g_tray_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  g_tray_thread = CreateThread(NULL, 0, si_tray_thread_main, NULL, 0, NULL);
  if (g_tray_thread == NULL) return SI_ERROR;
  WaitForSingleObject(g_tray_ready, 5000);
  return g_tray_hwnd != NULL ? SI_OK : SI_ERROR;
}

FFI_PLUGIN_EXPORT int32_t si_tray_show(const char* icon_path,
                                       const char* tooltip) {
  wchar_t* icon;
  wchar_t* tip;
  LRESULT r;
  if (g_tray_hwnd == NULL) return SI_ERROR;
  icon = si_wide(icon_path);
  tip = si_wide(tooltip);
  r = SendMessageW(g_tray_hwnd, SI_WM_SHOW, (WPARAM)icon, (LPARAM)tip);
  free(icon);
  free(tip);
  return (int32_t)r;
}

FFI_PLUGIN_EXPORT int32_t si_tray_hide(void) {
  if (g_tray_hwnd == NULL) return SI_ERROR;
  return (int32_t)SendMessageW(g_tray_hwnd, SI_WM_HIDE, 0, 0);
}

FFI_PLUGIN_EXPORT int32_t si_tray_set_tooltip(const char* tooltip) {
  wchar_t* tip;
  LRESULT r;
  if (g_tray_hwnd == NULL) return SI_ERROR;
  tip = si_wide(tooltip);
  r = SendMessageW(g_tray_hwnd, SI_WM_TOOLTIP, 0, (LPARAM)tip);
  free(tip);
  return (int32_t)r;
}

FFI_PLUGIN_EXPORT int32_t si_tray_set_progress(int32_t value,
                                               const char* label) {
  wchar_t* l;
  LRESULT r;
  if (g_tray_hwnd == NULL) return SI_ERROR;
  l = si_wide(label);
  r = SendMessageW(g_tray_hwnd, SI_WM_PROGRESS, (WPARAM)value, (LPARAM)l);
  free(l);
  return (int32_t)r;
}

FFI_PLUGIN_EXPORT int32_t si_tray_set_menu(const char** labels,
                                           const int32_t* flags,
                                           int32_t count) {
  si_menu_data m;
  int32_t i;
  if (g_tray_hwnd == NULL) return SI_ERROR;
  if (count < 0) count = 0;
  m.count = count;
  m.labels = (wchar_t**)calloc((size_t)(count > 0 ? count : 1), sizeof(wchar_t*));
  m.flags = (int32_t*)calloc((size_t)(count > 0 ? count : 1), sizeof(int32_t));
  for (i = 0; i < count; i++) {
    m.labels[i] = si_wide(labels[i] ? labels[i] : "");
    m.flags[i] = flags[i];
  }
  return (int32_t)SendMessageW(g_tray_hwnd, SI_WM_MENU, 0, (LPARAM)&m);
}

FFI_PLUGIN_EXPORT void si_tray_dispose(void) {
  if (g_tray_thread == NULL) return;
  if (g_tray_hwnd != NULL) PostMessageW(g_tray_hwnd, WM_CLOSE, 0, 0);
  WaitForSingleObject(g_tray_thread, 2000);
  CloseHandle(g_tray_thread);
  CloseHandle(g_tray_ready);
  g_tray_thread = NULL;
  g_tray_ready = NULL;
  g_tray_cb = NULL;
}

// ------------------------------------------------------------------ window
FFI_PLUGIN_EXPORT int32_t si_window_hide(void) {
  HWND w = si_main_window();
  if (w == NULL) return SI_ERROR;
  ShowWindowAsync(w, SW_HIDE);
  return SI_OK;
}

FFI_PLUGIN_EXPORT int32_t si_window_restore(void) {
  HWND w = si_main_window();
  if (w == NULL) return SI_ERROR;
  ShowWindowAsync(w, IsIconic(w) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(w);
  return SI_OK;
}

FFI_PLUGIN_EXPORT int32_t si_window_flash(void) {
  FLASHWINFO fi;
  HWND w = si_main_window();
  if (w == NULL) return SI_ERROR;
  fi.cbSize = sizeof(fi);
  fi.hwnd = w;
  fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
  fi.uCount = 0;
  fi.dwTimeout = 0;
  FlashWindowEx(&fi);
  return SI_OK;
}
