part of '../system_integrator.dart';

/// How a volume is attached to the machine.
enum VolumeKind { fixed, removable, network, unknown }

/// One mounted volume / drive.
@immutable
class DiskVolume {
  const DiskVolume({
    required this.id,
    required this.label,
    required this.path,
    required this.totalBytes,
    required this.freeBytes,
    this.kind = VolumeKind.unknown,
    this.isPrimary = false,
    this.isReadOnly = false,
    this.fileSystem = '',
  });

  /// `C:` on Windows, mount point on macOS, block device on Linux.
  final String id;
  final String label;

  /// Root / mount point (`C:\`, `/`, `/Volumes/USB`, `/media/me/USB`).
  final String path;
  final int totalBytes;

  /// Bytes available to the current (unprivileged) user.
  final int freeBytes;
  final VolumeKind kind;

  /// Volume holding the user's home folder – the natural download target.
  final bool isPrimary;
  final bool isReadOnly;
  final String fileSystem;

  /// Derived: `totalBytes - freeBytes`.
  int get usedBytes {
    final used = totalBytes - freeBytes;
    return used < 0 ? 0 : used;
  }

  /// 0..100
  double get usagePercent =>
      totalBytes <= 0 ? 0 : (usedBytes * 100.0 / totalBytes).clamp(0.0, 100.0);

  bool get isFixed => kind == VolumeKind.fixed;
  bool get isRemovable => kind == VolumeKind.removable;
  bool get isNetwork => kind == VolumeKind.network;

  DiskVolume _withPrimary(bool primary) => DiskVolume(
        id: id,
        label: label,
        path: path,
        totalBytes: totalBytes,
        freeBytes: freeBytes,
        kind: kind,
        isPrimary: primary,
        isReadOnly: isReadOnly,
        fileSystem: fileSystem,
      );

  @override
  bool operator ==(Object other) =>
      other is DiskVolume &&
      other.id == id &&
      other.label == label &&
      other.path == path &&
      other.totalBytes == totalBytes &&
      other.freeBytes == freeBytes &&
      other.kind == kind &&
      other.isPrimary == isPrimary &&
      other.isReadOnly == isReadOnly &&
      other.fileSystem == fileSystem;

  @override
  int get hashCode => Object.hash(id, label, path, totalBytes, freeBytes, kind,
      isPrimary, isReadOnly, fileSystem);

  @override
  String toString() => 'DiskVolume($id "$label" $path ${kind.name} '
      'total=$totalBytes free=$freeBytes '
      '${usagePercent.toStringAsFixed(1)}%${isPrimary ? ' primary' : ''})';
}

/// What works on the current OS / session. Hide or disable UI for `false`.
@immutable
class SystemCapabilities {
  const SystemCapabilities({
    this.canOpenFile = false,
    this.canRevealInPlace = false,
    this.canRevealParent = false,
    this.canCopyToClipboard = true,
    this.canListVolumes = false,
    this.canScheduleShutdown = false,
    this.canCancelShutdown = false,
    this.canSleep = false,
    this.canLaunchAtStartup = false,
    this.hasTray = false,
    this.hasTrayMenu = false,
    this.hasNativeTrayProgress = false,
    this.trayLeftClickRestores = false,
    this.canMinimizeToTray = false,
    this.canFlashWindow = false,
  });

  final bool canOpenFile;

  /// File manager opens with the file selected.
  final bool canRevealInPlace;

  /// Fallback: only the containing folder can be opened.
  final bool canRevealParent;
  final bool canCopyToClipboard;
  final bool canListVolumes;
  bool get canWatchVolumes => canListVolumes;
  final bool canScheduleShutdown;
  final bool canCancelShutdown;
  final bool canSleep;
  final bool canLaunchAtStartup;
  final bool hasTray;
  final bool hasTrayMenu;

  /// Windows: real taskbar progress. macOS / Linux: emulated as text.
  final bool hasNativeTrayProgress;

  /// False on Linux AppIndicator (left click always opens the menu).
  final bool trayLeftClickRestores;
  final bool canMinimizeToTray;
  final bool canFlashWindow;

  Map<String, bool> toMap() => {
        'canOpenFile': canOpenFile,
        'canRevealInPlace': canRevealInPlace,
        'canRevealParent': canRevealParent,
        'canCopyToClipboard': canCopyToClipboard,
        'canListVolumes': canListVolumes,
        'canWatchVolumes': canWatchVolumes,
        'canScheduleShutdown': canScheduleShutdown,
        'canCancelShutdown': canCancelShutdown,
        'canSleep': canSleep,
        'canLaunchAtStartup': canLaunchAtStartup,
        'hasTray': hasTray,
        'hasTrayMenu': hasTrayMenu,
        'hasNativeTrayProgress': hasNativeTrayProgress,
        'trayLeftClickRestores': trayLeftClickRestores,
        'canMinimizeToTray': canMinimizeToTray,
        'canFlashWindow': canFlashWindow,
      };

  @override
  String toString() => 'SystemCapabilities(${toMap()})';
}

/// One entry of the tray context menu.
@immutable
class TrayMenuItem {
  const TrayMenuItem({
    required this.id,
    required this.label,
    this.enabled = true,
    this.checked = false,
  }) : isSeparator = false;

  const TrayMenuItem.separator()
      : id = '',
        label = '',
        enabled = false,
        checked = false,
        isSeparator = true;

  final String id;
  final String label;
  final bool enabled;
  final bool checked;
  final bool isSeparator;

  int get _flags =>
      (enabled ? SI_MENU_ENABLED : 0) |
      (checked ? SI_MENU_CHECKED : 0) |
      (isSeparator ? SI_MENU_SEPARATOR : 0);
}
