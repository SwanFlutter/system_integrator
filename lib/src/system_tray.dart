part of '../system_integrator.dart';

/// Tray icon: Shell_NotifyIconW (Windows), NSStatusItem (macOS),
/// StatusNotifierItem/AppIndicator over DBus or GtkStatusIcon (Linux).
///
/// Native events arrive through a [NativeCallable.listener], so they are safe
/// no matter which native thread raises them.
class SystemTray {
  SystemTray._();

  NativeCallable<si_tray_callbackFunction>? _callable;
  List<TrayMenuItem> _items = const [];

  /// Left click on the icon (not delivered by Linux AppIndicator).
  VoidCallback? onClick;

  /// A context-menu item was selected; receives [TrayMenuItem.id].
  ValueChanged<String>? onMenuItemSelected;

  bool get _supported {
    try {
      return _bindings.si_native_capabilities() & SI_CAP_TRAY != 0;
    } catch (_) {
      return false;
    }
  }

  bool _ensureInit() {
    if (_callable != null) return true;
    if (!_supported) return false;
    final callable = NativeCallable<si_tray_callbackFunction>.listener(_onEvent);
    if (_bindings.si_tray_init(callable.nativeFunction) != SI_OK) {
      callable.close();
      return false;
    }
    _callable = callable;
    return true;
  }

  void _onEvent(int event, int index) {
    if (event == SI_TRAY_EVENT_CLICK) {
      onClick?.call();
    } else if (event == SI_TRAY_EVENT_MENU &&
        index >= 0 &&
        index < _items.length) {
      onMenuItemSelected?.call(_items[index].id);
    }
  }

  /// [iconPath]: `.ico` on Windows, `.png` on macOS/Linux (absolute path, see
  /// [assetPath]). `null` keeps the current icon (default: app icon).
  Future<bool> show({String? iconPath, String? tooltip}) async =>
      _ensureInit() &&
      using((arena) => _bindings.si_tray_show(
            iconPath == null ? nullptr : iconPath.toNativeUtf8(allocator: arena).cast(),
            tooltip == null ? nullptr : tooltip.toNativeUtf8(allocator: arena).cast(),
          )) ==
          SI_OK;

  Future<bool> hide() async => _callable != null && _bindings.si_tray_hide() == SI_OK;

  Future<bool> setTooltip(String tooltip) async =>
      _ensureInit() && _withCString(tooltip, _bindings.si_tray_set_tooltip) == SI_OK;

  /// 0..100, or `null` to clear. Windows: taskbar progress. macOS / Linux have
  /// no native tray progress, so `42%` is shown as title / label text.
  Future<bool> setProgress(int? percent) async {
    if (!_ensureInit()) return false;
    final value = percent == null ? SI_PROGRESS_NONE : percent.clamp(0, 100);
    final label = percent == null ? '' : '$value%';
    return _withCString(label, (l) => _bindings.si_tray_set_progress(value, l)) == SI_OK;
  }

  Future<bool> setIndeterminate() async =>
      _ensureInit() &&
      _withCString('…', (l) => _bindings.si_tray_set_progress(SI_PROGRESS_INDETERMINATE, l)) ==
          SI_OK;

  /// Context menu (right click on Windows/macOS, any click on AppIndicator).
  Future<bool> setMenu(List<TrayMenuItem> items) async {
    if (!_ensureInit()) return false;
    _items = List.unmodifiable(items);
    return using((arena) {
      final labels = arena<Pointer<Char>>(items.isEmpty ? 1 : items.length);
      final flags = arena<Int32>(items.isEmpty ? 1 : items.length);
      for (var i = 0; i < items.length; i++) {
        labels[i] = items[i].label.toNativeUtf8(allocator: arena).cast();
        flags[i] = items[i]._flags;
      }
      return _bindings.si_tray_set_menu(labels, flags, items.length) == SI_OK;
    });
  }

  /// Removes the icon and releases the native callback.
  void dispose() {
    if (_callable == null) return;
    _bindings.si_tray_dispose();
    _callable!.close();
    _callable = null;
  }

  /// Absolute path of a bundled Flutter asset on desktop.
  static String assetPath(String asset) {
    final exeDir = File(Platform.resolvedExecutable).parent.path;
    if (Platform.isMacOS) {
      return '$exeDir/../Frameworks/App.framework/Resources/flutter_assets/$asset';
    }
    final sep = Platform.pathSeparator;
    return '$exeDir${sep}data${sep}flutter_assets$sep${asset.replaceAll('/', sep)}';
  }
}
