// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter_test/flutter_test.dart';
import 'package:magnet_app/scanner/scanned_access_point.dart';
import 'package:magnet_app/scanner/scanned_device.dart';
import 'package:magnet_app/scanner/vendor_lookup.dart';

void main() {
  group('ScannedAccessPoint', () {
    ScannedAccessPoint ap(int freq) => ScannedAccessPoint(
        ssid: 'net', bssid: 'aa', rssi: -50, frequencyMhz: freq,
        lastSeen: DateTime(2026));

    test('band classification', () {
      expect(ap(2412).band, '2.4 GHz');
      expect(ap(5180).band, '5 GHz');
      expect(ap(5955).band, '6 GHz');
    });

    test('channel derivation (2.4 + 5 GHz)', () {
      expect(ap(2412).channel, 1);
      expect(ap(2437).channel, 6);
      expect(ap(2484).channel, 14);
      expect(ap(5180).channel, 36);
    });

    test('hidden ssid displays as (hidden)', () {
      expect(ScannedAccessPoint(
              ssid: '', bssid: 'aa', rssi: -50, lastSeen: DateTime(2026))
          .displayName, '(hidden)');
    });
  });

  group('ScannedDevice', () {
    test('companyId is the first manufacturer key; displayName falls back',
        () {
      final ScannedDevice d = ScannedDevice(
        id: 'X',
        name: '',
        rssi: -60,
        firstSeen: DateTime(2026),
        lastSeen: DateTime(2026),
        manufacturerData: <int, List<int>>{0x004C: <int>[1, 2]},
      );
      expect(d.companyId, 0x004C);
      expect(d.displayName, 'X');
      expect(d.isNamed, isFalse);
    });
  });

  group('VendorLookup', () {
    test('known + unknown company ids', () {
      expect(VendorLookup.forCompanyId(0x004C), 'Apple, Inc.');
      expect(VendorLookup.forCompanyId(0xFFFF), isNull);
      expect(VendorLookup.forCompanyId(null), isNull);
    });
  });
}
