part of '../system_integrator.dart';

/// Edge-triggered low-space detector used by [SystemIntegrator.onLowSpace].
///
/// Fires once when a volume's free space drops below [threshold] (also when
/// it is already low on the first observation) and re-arms only after free
/// space recovers to `threshold + hysteresis`.
class LowSpaceDetector {
  LowSpaceDetector({required this.threshold, this.hysteresis = 0})
      : assert(threshold >= 0),
        assert(hysteresis >= 0);

  final int threshold;
  final int hysteresis;
  final Set<String> _low = <String>{};

  /// Returns the volumes that just crossed below the threshold.
  List<DiskVolume> update(Iterable<DiskVolume> volumes) {
    final fired = <DiskVolume>[];
    for (final v in volumes) {
      if (_low.contains(v.id)) {
        if (v.freeBytes >= threshold + hysteresis) _low.remove(v.id);
      } else if (v.freeBytes < threshold) {
        _low.add(v.id);
        fired.add(v);
      }
    }
    return fired;
  }

  bool isLow(String volumeId) => _low.contains(volumeId);
  void reset() => _low.clear();
}
