/// Hybrid desktop system integration for Flutter (Windows, macOS, Linux).
///
/// * Dart core: `Process.run` over each OS's built-in tools.
/// * `dart:ffi` into the bundled native library built from `src/`
///   (counters/queries, Win32 calls and the UI-bound tray module).
library;

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'system_integrator_bindings_generated.dart';

part 'src/models.dart';
part 'src/low_space_detector.dart';
part 'src/os_commands.dart';
part 'src/system_tray.dart';

const String _libName = 'system_integrator';

/// The dynamic library in which the symbols for [SystemIntegratorBindings] can be found.
final DynamicLibrary _dylib = () {
  if (Platform.isMacOS || Platform.isIOS) {
    return DynamicLibrary.open('$_libName.framework/$_libName');
  }
  if (Platform.isAndroid || Platform.isLinux) {
    return DynamicLibrary.open('lib$_libName.so');
  }
  if (Platform.isWindows) {
    return DynamicLibrary.open('$_libName.dll');
  }
  throw UnsupportedError('Unknown platform: ${Platform.operatingSystem}');
}();

/// The bindings to the native functions in [_dylib].
final SystemIntegratorBindings _bindings = SystemIntegratorBindings(_dylib);

/// Runs [f] with [s] as a temporary NUL-terminated UTF-8 C string.
T _withCString<T>(String? s, T Function(Pointer<Char> p) f) => using((arena) =>
    f(s == null ? nullptr : s.toNativeUtf8(allocator: arena).cast<Char>()));

// ===========================================================================
// Helper isolate (same pattern as the FFI template's `sumAsync`).
//
// si_list_volumes / si_get_disk_space can block for seconds on a sleeping
// network share, so they never run on the main isolate.
// ===========================================================================

/// A request to list all mounted volumes.
class _VolumesRequest {
  final int id;
  const _VolumesRequest(this.id);
}

/// A request to probe the disk space of the volume containing [path].
class _DiskSpaceRequest {
  final int id;
  final String path;
  const _DiskSpaceRequest(this.id, this.path);
}

/// A response from the helper isolate.
class _Response {
  final int id;
  final Object? result;
  const _Response(this.id, this.result);
}

/// Counter to identify requests and responses.
int _nextRequestId = 0;

/// Mapping from request `id`s to the completers of the pending requests.
final Map<int, Completer<Object?>> _requests = <int, Completer<Object?>>{};

/// The SendPort belonging to the helper isolate.
final Future<SendPort> _helperIsolateSendPort = () async {
  final Completer<SendPort> completer = Completer<SendPort>();

  final ReceivePort receivePort = ReceivePort()
    ..listen((dynamic data) {
      if (data is SendPort) {
        completer.complete(data);
        return;
      }
      if (data is _Response) {
        final Completer<Object?> c = _requests.remove(data.id)!;
        c.complete(data.result);
        return;
      }
      throw UnsupportedError('Unsupported message type: ${data.runtimeType}');
    });

  await Isolate.spawn((SendPort sendPort) async {
    final ReceivePort helperReceivePort = ReceivePort()
      ..listen((dynamic data) {
        if (data is _VolumesRequest) {
          sendPort.send(_Response(data.id, _nativeListVolumes()));
          return;
        }
        if (data is _DiskSpaceRequest) {
          sendPort.send(_Response(data.id, _nativeDiskSpace(data.path)));
          return;
        }
        throw UnsupportedError('Unsupported message type: ${data.runtimeType}');
      });
    sendPort.send(helperReceivePort.sendPort);
  }, receivePort.sendPort);

  return completer.future;
}();

Future<Object?> _sendToHelper(Object Function(int id) makeRequest) async {
  final SendPort port = await _helperIsolateSendPort;
  final int id = _nextRequestId++;
  final Completer<Object?> completer = Completer<Object?>();
  _requests[id] = completer;
  port.send(makeRequest(id));
  return completer.future;
}

String _readCString(Array<Char> chars, int length) {
  final bytes = <int>[];
  for (var i = 0; i < length; i++) {
    final c = chars[i] & 0xff;
    if (c == 0) break;
    bytes.add(c);
  }
  return utf8.decode(bytes, allowMalformed: true);
}

/// Runs in the helper isolate.
List<DiskVolume> _nativeListVolumes() {
  const capacity = 128;
  final Pointer<si_volume> buf = calloc<si_volume>(capacity);
  try {
    final n = _bindings.si_list_volumes(buf, capacity);
    if (n <= 0) return const <DiskVolume>[];
    return [
      for (var i = 0; i < (n < capacity ? n : capacity); i++)
        () {
          final v = (buf + i).ref;
          return DiskVolume(
            id: _readCString(v.id, 128),
            label: _readCString(v.label, 256),
            path: _readCString(v.path, 1024),
            fileSystem: _readCString(v.fs_type, 32),
            totalBytes: v.total_bytes,
            freeBytes: v.free_bytes,
            kind: switch (v.kind) {
              SI_VOLUME_FIXED => VolumeKind.fixed,
              SI_VOLUME_REMOVABLE => VolumeKind.removable,
              SI_VOLUME_NETWORK => VolumeKind.network,
              _ => VolumeKind.unknown,
            },
            isReadOnly: v.read_only != 0,
          );
        }(),
    ];
  } finally {
    calloc.free(buf);
  }
}

/// Runs in the helper isolate. Returns free bytes for the user or -1.
int _nativeDiskSpace(String path) {
  final Pointer<si_disk_space> out = calloc<si_disk_space>();
  try {
    final rc = _withCString(path, (p) => _bindings.si_get_disk_space(p, out));
    return rc == SI_OK ? out.ref.free_bytes : -1;
  } finally {
    calloc.free(out);
  }
}

// ===========================================================================
// Public facade
// ===========================================================================

/// Cross-platform facade with an identical API on Windows, macOS and Linux.
///
/// Nothing throws because a feature is unsupported: check [capabilities]
/// and/or the returned `bool`.
class SystemIntegrator {
  SystemIntegrator._();

  /// The shared instance.
  static final SystemIntegrator instance = SystemIntegrator._();

  /// Native tray icon (the only UI-bound module).
  final SystemTray tray = SystemTray._();

  final _OsCommands _os = _OsCommands();

  String _appId = _defaultAppId();
  String _appName = _defaultAppId();

  /// [appId] names the HKCU Run value / LaunchAgent label / autostart
  /// .desktop file (e.g. `com.example.downloader`).
  void configure({String? appId, String? appName}) {
    if (appId != null) _appId = appId;
    if (appName != null) _appName = appName;
  }

  static String _defaultAppId() => File(Platform.resolvedExecutable)
      .uri
      .pathSegments
      .last
      .replaceAll(RegExp(r'\.exe$', caseSensitive: false), '');

  int? _caps;
  int get _nativeCaps {
    try {
      return _caps ??= _bindings.si_native_capabilities();
    } catch (_) {
      return _caps = 0; // library missing -> everything native reports false
    }
  }

  bool _has(int cap) => _nativeCaps & cap != 0;

  Future<bool> _guard(FutureOr<bool> Function() f) async {
    try {
      return await f();
    } catch (e) {
      debugPrint('system_integrator: $e');
      return false;
    }
  }

  // ---------------------------------------------------- 1. files & folders

  /// Win: ShellExecuteW · macOS: `open` · Linux: `xdg-open`.
  Future<bool> openFile(String path) => _guard(() async {
        if (FileSystemEntity.typeSync(path) == FileSystemEntityType.notFound) {
          return false;
        }
        if (_has(SI_CAP_OPEN_FILE)) {
          return _withCString(path, _bindings.si_open_file) == SI_OK;
        }
        return Platform.isMacOS
            ? _os.ok('/usr/bin/open', [path])
            : _os.launch('xdg-open', [path]);
      });

  /// Shows [path] selected in Explorer / Finder. On Linux it uses DBus
  /// `org.freedesktop.FileManager1.ShowItems`, or opens the containing folder
  /// when that is unavailable (`capabilities().canRevealInPlace == false`).
  Future<bool> revealInExplorer(String path) => _guard(() async {
        final abs = File(path).absolute.path;
        if (FileSystemEntity.typeSync(abs) == FileSystemEntityType.notFound) {
          return false;
        }
        if (Platform.isWindows) {
          final p = abs.replaceAll('/', r'\');
          if (_withCString(p, _bindings.si_reveal_in_file_manager) == SI_OK) {
            return true;
          }
          await Process.start('explorer.exe', ['/select,', p]); // fallback
          return true;
        }
        if (Platform.isMacOS) return _os.ok('/usr/bin/open', ['-R', abs]);
        return _os.linuxReveal(abs);
      });

  Future<bool> copyToClipboard(String text) => _guard(() async {
        await Clipboard.setData(ClipboardData(text: text));
        return true;
      });

  // ------------------------------------------------------------ 2. storage

  /// All mounted volumes (fixed / removable / network). `isPrimary` marks the
  /// volume holding the user's home folder (natural download target).
  Future<List<DiskVolume>> listVolumes() async {
    if (!_has(SI_CAP_LIST_VOLUMES)) return const [];
    try {
      final vols = (await _sendToHelper(_VolumesRequest.new))! as List<DiskVolume>;
      return _markPrimary(vols);
    } catch (e) {
      debugPrint('system_integrator: listVolumes failed: $e');
      return const [];
    }
  }

  List<DiskVolume> _markPrimary(List<DiskVolume> vols) {
    final home = Platform.environment[Platform.isWindows ? 'USERPROFILE' : 'HOME'];
    if (home == null) return vols;
    final ci = Platform.isWindows;
    String norm(String s) => ci ? s.toLowerCase() : s;
    DiskVolume? best;
    for (final v in vols) {
      final root = norm(v.path);
      final h = norm(home);
      final sep = root.endsWith('/') || root.endsWith(r'\') ? '' : Platform.pathSeparator;
      if ((h == root || h.startsWith('$root$sep')) &&
          (best == null || v.path.length > best.path.length)) {
        best = v;
      }
    }
    return [for (final v in vols) v._withPrimary(identical(v, best))];
  }

  Future<DiskVolume?> primaryVolume() async {
    final vols = await listVolumes();
    for (final v in vols) {
      if (v.isPrimary) return v;
    }
    return vols.isEmpty ? null : vols.first;
  }

  /// Re-probes every [interval]; emits the first snapshot and then ONLY when
  /// any value changed (exact bytes, so download writes show up 1:1).
  Stream<List<DiskVolume>> watchVolumes(
      {Duration interval = const Duration(seconds: 2)}) {
    late final StreamController<List<DiskVolume>> controller;
    Timer? timer;
    List<DiskVolume>? last;
    var busy = false;

    Future<void> tick() async {
      if (busy || controller.isClosed) return;
      busy = true;
      try {
        final now = await listVolumes();
        if (!listEquals(last, now) && !controller.isClosed) {
          last = now;
          controller.add(List<DiskVolume>.unmodifiable(now));
        }
      } finally {
        busy = false;
      }
    }

    void start() {
      timer?.cancel();
      unawaited(tick());
      timer = Timer.periodic(interval, (_) => tick());
    }

    controller = StreamController<List<DiskVolume>>(
      onListen: start,
      onPause: () => timer?.cancel(),
      onResume: start,
      onCancel: () => timer?.cancel(),
    );
    return controller.stream;
  }

  /// Bytes available to the user on the volume containing [directoryPath]
  /// (walks up to the nearest existing parent). Returns -1 when unknown.
  ///
  /// Plug into download_engine: `freeSpaceProbe: integrator.freeSpaceOf`.
  Future<int> freeSpaceOf(String directoryPath) async {
    if (!_has(SI_CAP_DISK_SPACE)) return -1;
    try {
      final dir = _existingAncestor(directoryPath);
      return (await _sendToHelper((id) => _DiskSpaceRequest(id, dir)))! as int;
    } catch (_) {
      return -1;
    }
  }

  /// Emits a volume exactly once when its free space drops below
  /// [freeBytesThreshold] (not on every tick). Re-armed after it recovers
  /// above threshold + [hysteresisBytes] (default: 5 % of the threshold).
  Stream<DiskVolume> onLowSpace({
    required int freeBytesThreshold,
    String? volumeId,
    int? hysteresisBytes,
    Duration interval = const Duration(seconds: 2),
  }) {
    final detector = LowSpaceDetector(
      threshold: freeBytesThreshold,
      hysteresis: hysteresisBytes ?? freeBytesThreshold ~/ 20,
    );
    return watchVolumes(interval: interval).expand((vols) => detector.update(
        volumeId == null ? vols : vols.where((v) => v.id == volumeId)));
  }

  // -------------------------------------------------------------- 3. power

  /// Win: InitiateSystemShutdownExW · macOS: detached `osascript` with
  /// `delay N` (pid tracked) · Linux: `shutdown -h +N` (minutes, rounded up).
  Future<bool> scheduleShutdown({required int seconds}) => _guard(() async {
        if (Platform.isWindows) {
          final ok = _bindings.si_schedule_shutdown(seconds) == SI_OK ||
              await _os.ok('shutdown.exe', ['/s', '/t', '$seconds']);
          if (ok) await _os.store.save(_PendingShutdown(DateTime.now().add(Duration(seconds: seconds))));
          return ok;
        }
        return Platform.isMacOS
            ? _os.macScheduleShutdown(seconds)
            : _os.linuxScheduleShutdown(seconds);
      });

  /// Win: AbortSystemShutdownW · macOS: kill the tracked osascript child ·
  /// Linux: `shutdown -c`.
  Future<bool> cancelShutdown() => _guard(() async {
        if (Platform.isWindows) {
          await _os.store.clear();
          return _bindings.si_cancel_shutdown() == SI_OK ||
              await _os.ok('shutdown.exe', ['/a']);
        }
        return Platform.isMacOS ? _os.macCancelShutdown() : _os.linuxCancelShutdown();
      });

  Future<bool> isShutdownPending() => _guard(() async {
        if (Platform.isWindows) {
          final s = await _os.store.load();
          return s != null && s.deadline.isAfter(DateTime.now());
        }
        return Platform.isMacOS ? _os.macShutdownPending() : _os.linuxShutdownPending();
      });

  /// macOS only: asks for the "control System Events" consent now, so the
  /// prompt doesn't block the shutdown later. Other OS: returns true.
  Future<bool> prepareShutdownPermission() => _guard(() async =>
      !Platform.isMacOS ||
      await _os.ok('/usr/bin/osascript',
          ['-e', 'tell application "System Events" to get name']));

  /// Win: SetSuspendState · macOS: `pmset sleepnow` · Linux: `systemctl suspend`.
  Future<bool> sleepNow() => _guard(() async {
        if (Platform.isWindows) return _bindings.si_sleep_now() == SI_OK;
        if (Platform.isMacOS) return _os.ok('/usr/bin/pmset', ['sleepnow']);
        return await _os.ok('systemctl', ['suspend']) ||
            await _os.ok('loginctl', ['suspend']);
      });

  // ---------------------------------------------------- 4. startup & tray

  /// HKCU Run (Win, via FFI) · LaunchAgents plist (macOS) · XDG autostart
  /// .desktop (Linux). Survives reboot; no admin rights.
  Future<bool> setLaunchAtStartup(bool enabled, {String args = ''}) =>
      _guard(() async {
        final exe = Platform.resolvedExecutable;
        final argv = _splitArgs(args);
        if (Platform.isWindows) {
          final cmd = ['"$exe"', ...argv.map(_quoteWin)].join(' ');
          return using((arena) => _bindings.si_set_launch_at_startup(
                  _appId.toNativeUtf8(allocator: arena).cast(),
                  enabled ? cmd.toNativeUtf8(allocator: arena).cast() : nullptr)) ==
              SI_OK;
        }
        return Platform.isMacOS
            ? _os.macSetLaunchAgent(enabled, _appId, [exe, ...argv])
            : _os.linuxSetAutostart(enabled, _appId, _appName, exe, argv);
      });

  Future<bool> isLaunchAtStartupEnabled() => _guard(() async {
        if (Platform.isWindows) {
          return _withCString(_appId, _bindings.si_is_launch_at_startup) == 1;
        }
        return _os.autostartFile(_appId).exists();
      });

  // ---------------------------------------------------------- 5. window

  /// Hides the main window (restore it from the tray).
  Future<bool> minimizeToTray() =>
      _guard(() => _has(SI_CAP_WINDOW) && _bindings.si_window_hide() == SI_OK);

  Future<bool> restoreWindow() =>
      _guard(() => _has(SI_CAP_WINDOW) && _bindings.si_window_restore() == SI_OK);

  /// Win: FlashWindowEx · macOS: requestUserAttention · Linux: X11 urgency
  /// hint (returns false under Wayland).
  Future<bool> flashWindow() => _guard(() =>
      _has(SI_CAP_FLASH_WINDOW) &&
      !_os.isWayland &&
      _bindings.si_window_flash() == SI_OK);

  // ------------------------------------------------------- capabilities

  Future<SystemCapabilities> capabilities() async {
    final linux = Platform.isLinux;
    final xdg = linux ? await _os.which('xdg-open') != null : true;
    final shutdown = Platform.isWindows ||
        Platform.isMacOS ||
        await _os.which('shutdown') != null ||
        await _os.which('systemctl') != null;
    return SystemCapabilities(
      canOpenFile: xdg,
      canRevealInPlace: linux ? await _os.hasFileManager1() : true,
      canRevealParent: xdg,
      canListVolumes: _has(SI_CAP_LIST_VOLUMES),
      canScheduleShutdown: shutdown,
      canCancelShutdown: shutdown,
      canSleep: !linux ||
          await _os.which('systemctl') != null ||
          await _os.which('loginctl') != null,
      canLaunchAtStartup: true,
      hasTray: _has(SI_CAP_TRAY),
      hasTrayMenu: _has(SI_CAP_TRAY),
      hasNativeTrayProgress: _has(SI_CAP_TRAY_NATIVE_PROGRESS),
      trayLeftClickRestores: _has(SI_CAP_TRAY_LEFT_CLICK),
      canMinimizeToTray: _has(SI_CAP_WINDOW),
      canFlashWindow: _has(SI_CAP_FLASH_WINDOW) && !_os.isWayland,
    );
  }
}
