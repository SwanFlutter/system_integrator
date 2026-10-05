import 'package:flutter_test/flutter_test.dart';
import 'package:system_integrator/system_integrator.dart';

DiskVolume vol(int free, {String id = 'C:'}) => DiskVolume(
    id: id, label: 'Disk', path: '$id\\', totalBytes: 1000, freeBytes: free);

void main() {
  group('LowSpaceDetector', () {
    test('fires exactly once when crossing, re-arms after hysteresis', () {
      final d = LowSpaceDetector(threshold: 100, hysteresis: 20);
      expect(d.update([vol(500)]), isEmpty);
      expect(d.update([vol(99)]), hasLength(1)); // crossed
      expect(d.update([vol(50)]), isEmpty); // still low: silent
      expect(d.update([vol(110)]), isEmpty); // below 120: not re-armed
      expect(d.update([vol(90)]), isEmpty);
      expect(d.update([vol(130)]), isEmpty); // re-armed
      expect(d.update([vol(80)]), hasLength(1)); // fires again
    });

    test('fires once if already low on first sample', () {
      final d = LowSpaceDetector(threshold: 100);
      expect(d.update([vol(10)]), hasLength(1));
      expect(d.update([vol(10)]), isEmpty);
    });

    test('volumes are tracked independently', () {
      final d = LowSpaceDetector(threshold: 100);
      expect(d.update([vol(10, id: 'C:'), vol(500, id: 'D:')]), hasLength(1));
      expect(d.update([vol(10, id: 'C:'), vol(50, id: 'D:')]).single.id, 'D:');
    });
  });

  test('DiskVolume derives used bytes and percent', () {
    final v = vol(400);
    expect(v.usedBytes, 600);
    expect(v.usagePercent, 60);
  });
}
