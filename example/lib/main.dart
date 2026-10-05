import 'package:flutter/material.dart';
import 'dart:async';
import 'dart:io';
import 'dart:math';

import 'package:system_integrator/system_integrator.dart';

// Download-manager style demo: Storage Usage card, Disk Pressure stat,
// low-space warning, "shut down when downloads finish", tray, startup.
void main() {
  WidgetsFlutterBinding.ensureInitialized();
  SystemIntegrator.instance.configure(
    appId: 'com.example.downloader',
    appName: 'Example Downloader',
  );
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  final si = SystemIntegrator.instance;
  final _messenger = GlobalKey<ScaffoldMessengerState>();
  static const lowSpaceThreshold = 5 * 1024 * 1024 * 1024; // 5 GiB

  SystemCapabilities caps = const SystemCapabilities();
  List<DiskVolume> volumes = const [];
  StreamSubscription<List<DiskVolume>>? _volumesSub;
  StreamSubscription<DiskVolume>? _lowSpaceSub;

  bool launchAtStartup = false;
  bool shutdownWhenDone = false;
  bool shutdownPending = false;
  String? lastFile;

  // Simulated download that writes real bytes, so watchVolumes shows the delta.
  Timer? _download;
  double progress = 0;
  int? freeBefore;
  int written = 0;

  @override
  void initState() {
    super.initState();
    _init();
  }

  Future<void> _init() async {
    caps = await si.capabilities();
    launchAtStartup = await si.isLaunchAtStartupEnabled();
    shutdownPending = await si.isShutdownPending();

    _volumesSub = si.watchVolumes().listen((v) => setState(() => volumes = v));
    _lowSpaceSub = si.onLowSpace(freeBytesThreshold: lowSpaceThreshold).listen(
        (v) => _snack('Low space on ${v.label}: ${_fmt(v.freeBytes)} left'));

    if (caps.hasTray) await _setupTray();
    if (mounted) setState(() {});
  }

  Future<void> _setupTray() async {
    await si.tray.show(
      iconPath: SystemTray.assetPath(
          Platform.isWindows ? 'assets/tray.ico' : 'assets/tray.png'),
      tooltip: 'Example Downloader',
    );
    await _updateTrayMenu();
    si.tray.onClick = si.restoreWindow; // not delivered by Linux AppIndicator
    si.tray.onMenuItemSelected = (id) async {
      switch (id) {
        case 'show':
          await si.restoreWindow();
        case 'pause':
          _stopDownload();
        case 'shutdown':
          setState(() => shutdownWhenDone = !shutdownWhenDone);
          await _updateTrayMenu();
        case 'quit':
          si.tray.dispose();
          exit(0);
      }
    };
  }

  Future<void> _updateTrayMenu() => si.tray.setMenu([
        const TrayMenuItem(id: 'show', label: 'Show window'),
        const TrayMenuItem(id: 'pause', label: 'Pause all'),
        TrayMenuItem(
            id: 'shutdown',
            label: 'Shut down when done',
            checked: shutdownWhenDone),
        const TrayMenuItem.separator(),
        const TrayMenuItem(id: 'quit', label: 'Quit'),
      ]);

  // ------------------------------------------------------------ download
  Future<void> _startDownload() async {
    const total = 200 * 1024 * 1024; // 200 MiB
    final dir = Directory.systemTemp.path;
    // Pre-flight disk guard (what download_engine.freeSpaceProbe does).
    final free = await si.freeSpaceOf(dir);
    if (free >= 0 && free < total) {
      _snack('Not enough space for this download');
      return;
    }
    final file = File('$dir${Platform.pathSeparator}si_demo_download.bin');
    final sink = file.openWrite();
    final chunk = List<int>.filled(4 * 1024 * 1024, 7);
    freeBefore = free;
    written = 0;
    await si.tray.setProgress(0);

    _download = Timer.periodic(const Duration(milliseconds: 200), (t) async {
      sink.add(chunk);
      await sink.flush();
      written += chunk.length;
      progress = written / total;
      await si.tray.setProgress((progress * 100).round());
      if (written >= total) {
        t.cancel();
        await sink.close();
        lastFile = file.path;
        await _onDownloadsFinished();
      }
      if (mounted) setState(() {});
    });
  }

  void _stopDownload() {
    _download?.cancel();
    si.tray.setProgress(null);
    setState(() => progress = 0);
  }

  Future<void> _onDownloadsFinished() async {
    await si.tray.setProgress(null);
    await si.flashWindow();
    _snack('Download finished: $lastFile');
    if (shutdownWhenDone && caps.canScheduleShutdown) {
      final ok = await si.scheduleShutdown(seconds: 60);
      setState(() => shutdownPending = ok);
      if (ok) _snack('Shutting down in 60 s, press "Cancel shutdown" to abort');
    }
  }

  // ------------------------------------------------------------ helpers
  void _snack(String msg) =>
      _messenger.currentState?.showSnackBar(SnackBar(content: Text(msg)));

  static String _fmt(int bytes) {
    if (bytes < 0) return '?';
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    final i = bytes == 0 ? 0 : (log(bytes) / log(1024)).floor().clamp(0, 4).toInt();
    return '${(bytes / pow(1024, i)).toStringAsFixed(1)} ${units[i]}';
  }

  @override
  void dispose() {
    _volumesSub?.cancel();
    _lowSpaceSub?.cancel();
    _download?.cancel();
    si.tray.dispose();
    super.dispose();
  }

  // ------------------------------------------------------------ UI
  @override
  Widget build(BuildContext context) {
    const spacerSmall = SizedBox(height: 10);
    final primary = volumes.where((v) => v.isPrimary).firstOrNull;
    final pressure = primary?.usagePercent ?? 0;
    final delta = (freeBefore != null && freeBefore! >= 0 && primary != null)
        ? freeBefore! - primary.freeBytes
        : null;

    return MaterialApp(
      scaffoldMessengerKey: _messenger,
      theme: ThemeData(colorSchemeSeed: Colors.indigo),
      home: Scaffold(
        appBar: AppBar(title: const Text('Downloads dashboard'), actions: [
          if (caps.canMinimizeToTray)
            IconButton(
              tooltip: 'Minimize to tray',
              icon: const Icon(Icons.south_east),
              onPressed: si.minimizeToTray,
            ),
        ]),
        body: SingleChildScrollView(
          child: Container(
            padding: const EdgeInsets.all(10),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                // ---------------- Disk Pressure
                Card(
                  child: ListTile(
                    leading: Icon(Icons.speed,
                        color: pressure > 90 ? Colors.red : Colors.green),
                    title: const Text('Disk Pressure'),
                    subtitle: Text(primary == null
                        ? '-'
                        : '${primary.label}: ${pressure.toStringAsFixed(1)} % used'),
                    trailing: delta == null
                        ? null
                        : Text('free Δ ${_fmt(delta)}\nwritten ${_fmt(written)}'),
                  ),
                ),
                spacerSmall,
                // ---------------- Storage Usage
                Card(
                  child: Padding(
                    padding: const EdgeInsets.all(16),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text('Storage Usage',
                            style: Theme.of(context).textTheme.titleMedium),
                        spacerSmall,
                        for (final v in volumes) ...[
                          Row(children: [
                            Icon(switch (v.kind) {
                              VolumeKind.removable => Icons.usb,
                              VolumeKind.network => Icons.cloud,
                              _ => Icons.storage,
                            }),
                            const SizedBox(width: 8),
                            Expanded(
                                child: Text('${v.label} (${v.path})'
                                    '${v.isPrimary ? '  *' : ''}')),
                            Text('${_fmt(v.freeBytes)} free of ${_fmt(v.totalBytes)}'),
                          ]),
                          const SizedBox(height: 4),
                          LinearProgressIndicator(
                            value: v.usagePercent / 100,
                            color: v.freeBytes < lowSpaceThreshold ? Colors.red : null,
                          ),
                          spacerSmall,
                        ],
                      ],
                    ),
                  ),
                ),
                spacerSmall,
                // ---------------- Download + files
                Card(
                  child: Padding(
                    padding: const EdgeInsets.all(16),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        LinearProgressIndicator(value: progress),
                        spacerSmall,
                        Wrap(spacing: 8, runSpacing: 8, children: [
                          FilledButton.icon(
                            onPressed:
                                _download?.isActive == true ? null : _startDownload,
                            icon: const Icon(Icons.download),
                            label: const Text('Simulate 200 MB download'),
                          ),
                          OutlinedButton(
                              onPressed: _stopDownload, child: const Text('Stop')),
                          if (lastFile != null && caps.canOpenFile)
                            OutlinedButton(
                                onPressed: () => si.openFile(lastFile!),
                                child: const Text('Open file')),
                          if (lastFile != null &&
                              (caps.canRevealInPlace || caps.canRevealParent))
                            OutlinedButton(
                                onPressed: () => si.revealInExplorer(lastFile!),
                                child: Text(caps.canRevealInPlace
                                    ? 'Show in folder'
                                    : 'Open containing folder')),
                          if (lastFile != null)
                            OutlinedButton(
                                onPressed: () => si.copyToClipboard(lastFile!),
                                child: const Text('Copy path')),
                        ]),
                      ],
                    ),
                  ),
                ),
                spacerSmall,
                // ---------------- Power & startup
                Card(
                  child: Column(children: [
                    SwitchListTile(
                      title: const Text('Shut down when downloads finish'),
                      subtitle: Text(shutdownPending ? 'Shutdown scheduled' : 'Off'),
                      value: shutdownWhenDone,
                      onChanged: caps.canScheduleShutdown
                          ? (v) async {
                              if (v) await si.prepareShutdownPermission();
                              setState(() => shutdownWhenDone = v);
                              await _updateTrayMenu();
                            }
                          : null,
                    ),
                    if (shutdownPending)
                      ListTile(
                        leading: const Icon(Icons.cancel, color: Colors.red),
                        title: const Text('Cancel shutdown'),
                        onTap: () async {
                          await si.cancelShutdown();
                          setState(() => shutdownPending = false);
                        },
                      ),
                    ListTile(
                      enabled: caps.canSleep,
                      leading: const Icon(Icons.bedtime),
                      title: const Text('Sleep now'),
                      onTap: si.sleepNow,
                    ),
                    SwitchListTile(
                      title: const Text('Launch at startup (minimized)'),
                      value: launchAtStartup,
                      onChanged: caps.canLaunchAtStartup
                          ? (v) async {
                              if (await si.setLaunchAtStartup(v, args: '--minimized')) {
                                setState(() => launchAtStartup = v);
                              }
                            }
                          : null,
                    ),
                  ]),
                ),
                // ---------------- Capabilities
                ExpansionTile(
                  title: const Text('Capabilities on this OS'),
                  children: [
                    for (final e in caps.toMap().entries)
                      ListTile(
                        dense: true,
                        title: Text(e.key),
                        trailing: Icon(e.value ? Icons.check : Icons.close,
                            color: e.value ? Colors.green : Colors.grey),
                      ),
                  ],
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
