part of '../system_integrator.dart';

// Dart-level core: built-in OS tools via Process.run (macOS / Linux),
// plus small helpers shared by all platforms.

/// Closest existing directory for [path] (a download target may not exist yet).
String _existingAncestor(String path) {
  var dir = Directory(path).absolute;
  while (!dir.existsSync()) {
    final parent = dir.parent;
    if (parent.path == dir.path) break;
    dir = parent;
  }
  return dir.path;
}

/// Splits a command-line string honouring "double" and 'single' quotes.
List<String> _splitArgs(String input) {
  final out = <String>[];
  final buf = StringBuffer();
  String? quote;
  var has = false;
  for (final ch in input.split('')) {
    if (quote != null) {
      if (ch == quote) {
        quote = null;
      } else {
        buf.write(ch);
      }
    } else if (ch == '"' || ch == "'") {
      quote = ch;
      has = true;
    } else if (ch.trim().isEmpty) {
      if (has || buf.isNotEmpty) out.add(buf.toString());
      buf.clear();
      has = false;
    } else {
      buf.write(ch);
    }
  }
  if (has || buf.isNotEmpty) out.add(buf.toString());
  return out;
}

String _quoteWin(String a) =>
    a.contains(' ') || a.contains('"') ? '"${a.replaceAll('"', r'\"')}"' : a;

/// A shutdown scheduled by this plugin (persisted across app restarts).
class _PendingShutdown {
  _PendingShutdown(this.deadline, {this.pid, this.method = ''});
  final DateTime deadline;
  final int? pid;
  final String method;
}

class _ShutdownStore {
  final File _file = File('${Directory.systemTemp.path}${Platform.pathSeparator}'
      'system_integrator_shutdown.json');

  Future<void> save(_PendingShutdown s) async {
    try {
      await _file.writeAsString(
          jsonEncode({
            'deadline': s.deadline.millisecondsSinceEpoch,
            'pid': s.pid,
            'method': s.method,
          }),
          flush: true);
    } on FileSystemException {
      // best effort
    }
  }

  Future<_PendingShutdown?> load() async {
    try {
      if (!await _file.exists()) return null;
      final j = jsonDecode(await _file.readAsString()) as Map<String, Object?>;
      return _PendingShutdown(
        DateTime.fromMillisecondsSinceEpoch((j['deadline']! as num).toInt()),
        pid: (j['pid'] as num?)?.toInt(),
        method: (j['method'] as String?) ?? '',
      );
    } catch (_) {
      return null;
    }
  }

  Future<void> clear() async {
    try {
      if (await _file.exists()) await _file.delete();
    } on FileSystemException {
      // ignore
    }
  }
}

class _OsCommands {
  final _ShutdownStore store = _ShutdownStore();
  final Map<String, String?> _which = {};
  bool? _fm1;
  Timer? _fallbackTimer;

  static const _fm1Name = 'org.freedesktop.FileManager1';

  bool get isWayland =>
      Platform.isLinux &&
      (Platform.environment['XDG_SESSION_TYPE'] == 'wayland' ||
          (Platform.environment['WAYLAND_DISPLAY']?.isNotEmpty ?? false));

  // ------------------------------------------------------------ process
  Future<ProcessResult?> run(String exe, List<String> args,
      {Duration timeout = const Duration(seconds: 15)}) async {
    try {
      return await Process.run(exe, args).timeout(timeout);
    } on ProcessException catch (e) {
      debugPrint('system_integrator: cannot run $exe: ${e.message}');
      return null;
    } on TimeoutException {
      return null;
    }
  }

  Future<bool> ok(String exe, List<String> args) async =>
      (await run(exe, args))?.exitCode == 0;

  /// For GUI helpers that may block until the launched app exits.
  Future<bool> launch(String exe, List<String> args) async {
    try {
      final p = await Process.start(exe, args);
      unawaited(p.stdout.drain<void>());
      unawaited(p.stderr.drain<void>());
      final code = await p.exitCode
          .timeout(const Duration(seconds: 3), onTimeout: () => 0);
      return code == 0;
    } on ProcessException {
      return false;
    }
  }

  Future<String?> which(String cmd) async {
    if (_which.containsKey(cmd)) return _which[cmd];
    final dirs = <String>{
      ...(Platform.environment['PATH'] ?? '').split(':'),
      '/usr/local/bin', '/usr/bin', '/bin', '/usr/sbin', '/sbin', //
    }..removeWhere((d) => d.isEmpty);
    for (final d in dirs) {
      if (File('$d/$cmd').existsSync()) return _which[cmd] = '$d/$cmd';
    }
    return _which[cmd] = null;
  }

  // -------------------------------------------------------------- Linux
  Future<bool> hasFileManager1() async {
    if (_fm1 != null) return _fm1!;
    for (final method in ['ListNames', 'ListActivatableNames']) {
      final r = await run('gdbus', [
        'call', '--session', '--dest', 'org.freedesktop.DBus',
        '--object-path', '/org/freedesktop/DBus',
        '--method', 'org.freedesktop.DBus.$method', //
      ]);
      if (r != null && r.exitCode == 0 && '${r.stdout}'.contains(_fm1Name)) {
        return _fm1 = true;
      }
    }
    return _fm1 = false;
  }

  /// DBus FileManager1.ShowItems, fallback: open the parent directory.
  Future<bool> linuxReveal(String abs) async {
    final uri = Uri.file(abs).toString().replaceAll(',', '%2C');
    if (await hasFileManager1()) {
      final esc = uri.replaceAll("'", r"\'");
      if (await ok('gdbus', [
            'call', '--session', '--dest', _fm1Name,
            '--object-path', '/org/freedesktop/FileManager1',
            '--method', '$_fm1Name.ShowItems', "['$esc']", "''", //
          ]) ||
          await ok('dbus-send', [
            '--session', '--print-reply', '--dest=$_fm1Name',
            '--type=method_call', '/org/freedesktop/FileManager1',
            '$_fm1Name.ShowItems', 'array:string:$uri', 'string:', //
          ])) {
        return true;
      }
    }
    return launch('xdg-open', [File(abs).parent.path]);
  }

  Future<bool> linuxScheduleShutdown(int seconds) async {
    await linuxCancelShutdown();
    final minutes = seconds <= 0 ? 0 : (seconds / 60).ceil();
    final shutdown = await which('shutdown');
    if (shutdown != null &&
        await ok(shutdown, ['-h', minutes == 0 ? 'now' : '+$minutes'])) {
      await store.save(_PendingShutdown(
          DateTime.now().add(Duration(minutes: minutes)),
          method: 'shutdown'));
      return true;
    }
    // Fallback (no systemd scheduling / polkit denied): in-process timer.
    final systemctl = await which('systemctl');
    if (systemctl == null) return false;
    _fallbackTimer = Timer(
        Duration(seconds: seconds), () => run(systemctl, ['poweroff']));
    await store.save(_PendingShutdown(
        DateTime.now().add(Duration(seconds: seconds)),
        method: 'timer'));
    return true;
  }

  Future<bool> linuxCancelShutdown() async {
    _fallbackTimer?.cancel();
    _fallbackTimer = null;
    final s = await store.load();
    var result = true;
    if (s?.method == 'shutdown' ||
        File('/run/systemd/shutdown/scheduled').existsSync()) {
      final shutdown = await which('shutdown');
      result = shutdown != null && await ok(shutdown, ['-c']);
    }
    await store.clear();
    return result;
  }

  Future<bool> linuxShutdownPending() async {
    if (_fallbackTimer?.isActive ?? false) return true;
    if (File('/run/systemd/shutdown/scheduled').existsSync()) return true;
    final s = await store.load();
    if (s != null && s.method == 'shutdown' && s.deadline.isAfter(DateTime.now())) {
      return true; // non-systemd: trust our record
    }
    if (s != null) await store.clear();
    return false;
  }

  static String _execQuote(String a) {
    if (!RegExp(r'[\s"`$\\]').hasMatch(a)) return a;
    return '"${a.replaceAll(r'\', r'\\').replaceAll('"', r'\"').replaceAll('`', r'\`').replaceAll(r'$', r'\$')}"';
  }

  File autostartFile(String appId) {
    if (Platform.isMacOS) {
      return File('${Platform.environment['HOME']}/Library/LaunchAgents/$appId.plist');
    }
    final cfg = Platform.environment['XDG_CONFIG_HOME'] ??
        '${Platform.environment['HOME']}/.config';
    return File('$cfg/autostart/$appId.desktop');
  }

  Future<bool> linuxSetAutostart(bool enabled, String appId, String appName,
      String exe, List<String> args) async {
    final f = autostartFile(appId);
    try {
      if (!enabled) {
        if (await f.exists()) await f.delete();
        return true;
      }
      final program = Platform.environment['APPIMAGE'] ?? exe;
      await f.parent.create(recursive: true);
      await f.writeAsString('''
[Desktop Entry]
Type=Application
Name=$appName
Exec=${[program, ...args].map(_execQuote).join(' ')}
Terminal=false
Hidden=false
NoDisplay=false
X-GNOME-Autostart-enabled=true
''', flush: true);
      return true;
    } on FileSystemException {
      return false;
    }
  }

  // -------------------------------------------------------------- macOS
  Future<bool> _isOurOsascript(int pid) async {
    final r = await run('/bin/ps', ['-p', '$pid', '-o', 'comm=']);
    return r != null && r.exitCode == 0 && '${r.stdout}'.contains('osascript');
  }

  Future<bool> macScheduleShutdown(int seconds) async {
    await macCancelShutdown();
    try {
      final p = await Process.start(
        '/usr/bin/osascript',
        ['-e', 'delay $seconds', '-e', 'tell application "System Events" to shut down'],
        mode: ProcessStartMode.detached,
      );
      await store.save(_PendingShutdown(
          DateTime.now().add(Duration(seconds: seconds)),
          pid: p.pid,
          method: 'osascript'));
      return true;
    } on ProcessException {
      return false;
    }
  }

  Future<bool> macCancelShutdown() async {
    final s = await store.load();
    var result = true;
    if (s?.pid != null && await _isOurOsascript(s!.pid!)) {
      result = Process.killPid(s.pid!, ProcessSignal.sigterm);
    }
    await store.clear();
    return result;
  }

  Future<bool> macShutdownPending() async {
    final s = await store.load();
    if (s?.pid == null) return false;
    if (await _isOurOsascript(s!.pid!)) return true;
    await store.clear();
    return false;
  }

  Future<bool> macSetLaunchAgent(bool enabled, String appId, List<String> argv) async {
    final f = autostartFile(appId);
    try {
      if (!enabled) {
        if (await f.exists()) await f.delete();
        return true;
      }
      String x(String s) =>
          s.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
      final args = argv.map((a) => '    <string>${x(a)}</string>').join('\n');
      await f.parent.create(recursive: true);
      await f.writeAsString('''
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>${x(appId)}</string>
  <key>ProgramArguments</key>
  <array>
$args
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>ProcessType</key>
  <string>Interactive</string>
  <key>LimitLoadToSessionType</key>
  <string>Aqua</string>
</dict>
</plist>
''', flush: true);
      return true;
    } on FileSystemException {
      return false;
    }
  }
}
