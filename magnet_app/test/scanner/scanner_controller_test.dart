// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter_test/flutter_test.dart';
import 'package:magnet_app/scanner/ble_scanner.dart';
import 'package:magnet_app/scanner/scanner_controller.dart';

import 'fake_ble_scanner.dart';

BleAd _ad(String id, int rssi, {String name = ''}) =>
    BleAd(id: id, name: name, rssi: rssi);

Future<void> _tick() => Future<void>.delayed(Duration.zero);

void main() {
  group('ScannerController BLE ingest', () {
    test('start subscribes and ads populate devices (strongest first)',
        () async {
      final FakeBleScanner ble = FakeBleScanner();
      final ScannerController sc = ScannerController(ble: ble);
      await sc.start();
      ble.emitAds(<BleAd>[_ad('a', -70, name: 'A'), _ad('b', -50, name: 'B')]);
      await _tick();
      expect(sc.bleCount, 2);
      expect(sc.devices.first.id, 'b', reason: 'strongest first');
      expect(ble.started, isTrue);
      sc.dispose();
    });

    test('re-emitting a device preserves firstSeen, updates rssi', () async {
      final FakeBleScanner ble = FakeBleScanner();
      final ScannerController sc = ScannerController(ble: ble);
      await sc.start();
      ble.emitAds(<BleAd>[_ad('a', -70)]);
      await _tick();
      final DateTime firstSeen = sc.deviceById('a')!.firstSeen;
      ble.emitAds(<BleAd>[_ad('a', -55)]);
      await _tick();
      expect(sc.deviceById('a')!.rssi, -55);
      expect(sc.deviceById('a')!.firstSeen, firstSeen);
      sc.dispose();
    });

    test('a device that drops out of the batch is removed', () async {
      final FakeBleScanner ble = FakeBleScanner();
      final ScannerController sc = ScannerController(ble: ble);
      await sc.start();
      ble.emitAds(<BleAd>[_ad('a', -70), _ad('b', -60)]);
      await _tick();
      ble.emitAds(<BleAd>[_ad('a', -70)]);
      await _tick();
      expect(sc.bleCount, 1);
      expect(sc.deviceById('b'), isNull);
      sc.dispose();
    });

    test('rssi floor filters weak devices from list + count', () async {
      final FakeBleScanner ble = FakeBleScanner();
      final ScannerController sc = ScannerController(ble: ble);
      await sc.start();
      ble.emitAds(<BleAd>[_ad('strong', -50), _ad('weak', -90)]);
      await _tick();
      expect(sc.bleCount, 2);
      sc.setRssiFloor(-70);
      expect(sc.bleCount, 1);
      expect(sc.devices.single.id, 'strong');
      sc.dispose();
    });

    test('clear empties devices', () async {
      final FakeBleScanner ble = FakeBleScanner();
      final ScannerController sc = ScannerController(ble: ble);
      await sc.start();
      ble.emitAds(<BleAd>[_ad('a', -70)]);
      await _tick();
      sc.clear();
      expect(sc.bleCount, 0);
      sc.dispose();
    });

    test('adapter + scanning state propagate', () async {
      final FakeBleScanner ble = FakeBleScanner();
      final ScannerController sc = ScannerController(ble: ble);
      ble.emitAdapter(BleAdapterState.on);
      await _tick();
      expect(sc.adapterState, BleAdapterState.on);
      await sc.start();
      await _tick();
      expect(sc.isScanning, isTrue);
      await sc.stop();
      await _tick();
      expect(sc.isScanning, isFalse);
      sc.dispose();
    });
  });

  group('ScannerController connection', () {
    test('connect discovers services; disconnect clears', () async {
      final FakeBleScanner ble = FakeBleScanner()
        ..servicesToReturn = <GattService>[
          const GattService(uuid: '180f', characteristics: <String>['2a19'])
        ];
      final ScannerController sc = ScannerController(ble: ble);
      await sc.connect('a');
      expect(sc.connectionFor('a'), DeviceConnection.connected);
      expect(sc.services.single.uuid, '180f');
      await sc.disconnect();
      expect(sc.connectionFor('a'), DeviceConnection.disconnected);
      expect(sc.services, isEmpty);
      sc.dispose();
    });

    test('connect failure sets connectFailed, stays disconnected', () async {
      final FakeBleScanner ble = FakeBleScanner()..throwOnConnect = true;
      final ScannerController sc = ScannerController(ble: ble);
      await sc.connect('a');
      expect(sc.connectFailed, isTrue);
      expect(sc.connectionFor('a'), DeviceConnection.disconnected);
      sc.dispose();
    });
  });
}
