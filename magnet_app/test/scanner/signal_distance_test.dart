// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter_test/flutter_test.dart';
import 'package:magnet_app/scanner/signal_distance.dart';

void main() {
  group('radialFraction', () {
    test('clamps at the strong/weak ends', () {
      expect(SignalDistance.radialFraction(-30), 0.0);
      expect(SignalDistance.radialFraction(-110), 1.0);
    });
    test('is monotonic increasing as signal weakens', () {
      double prev = -1;
      for (int rssi = -40; rssi >= -100; rssi -= 5) {
        final double f = SignalDistance.radialFraction(rssi);
        expect(f, greaterThanOrEqualTo(prev));
        prev = f;
      }
    });
  });

  group('band', () {
    test('thresholds', () {
      expect(SignalDistance.band(-50), SignalBand.strong);
      expect(SignalDistance.band(-75), SignalBand.medium);
      expect(SignalDistance.band(-95), SignalBand.weak);
    });
  });

  group('approxMeters', () {
    test('stronger signal → smaller distance', () {
      expect(SignalDistance.approxMeters(-50),
          lessThan(SignalDistance.approxMeters(-80)));
    });
    test('at the reference RSSI distance is ~1 m', () {
      expect(SignalDistance.approxMeters(-59), closeTo(1.0, 0.2));
    });
  });
}
