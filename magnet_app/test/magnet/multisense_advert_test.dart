// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter_test/flutter_test.dart';
import 'package:magnet_app/magnet/multisense_advert.dart';

void main() {
  // Captured from MagNET-7d8f (ArtemisNano_Hive_Multisense, fw 0.4.0-m4):
  // role 15, bitmap 0x5e, 96 %, seq 13, 6 lux, 2471 K, yaw -153, on-table.
  const List<int> header = <int>[0x1F, 0x5E, 0x00, 96, 13, 0];
  const List<int> readings = <int>[
    6, 0, // lux
    0xA7, 0x09, // 2471 K
    0x67, 0xFF, // -153
    1, // on-table
    0, // sig motion
  ];

  test('advert header alone (passive scan)', () {
    final MultisenseAdvert? a = MultisenseAdvert.fromManufacturerData(
        <int, List<int>>{0xFFFF: header});
    expect(a, isNotNull);
    expect(a!.role, MultisenseAdvert.roleMultisensor);
    expect(a.roleName, 'multisensor');
    expect(a.sensors, <String>['lux', 'color', 'time', 'battery', 'orientation']);
    expect(a.batteryPct, 96);
    expect(a.sequence, 13);
    expect(a.hasReadings, isFalse);
    expect(a.summary(), 'multisensor · 96%');
  });

  test('header + readings (scan response merged)', () {
    final MultisenseAdvert a = MultisenseAdvert.fromManufacturerData(
        <int, List<int>>{0xFFFF: <int>[...header, ...readings]})!;
    expect(a.lux, 6);
    expect(a.cctK, 2471);
    expect(a.yawDeg, -153);
    expect(a.stability, 'on-table');
    expect(a.sigMotion, 0);
    expect(a.summary(), 'multisensor · 96% · 6 lx · 2471 K · yaw -153° · on-table');
  });

  test('Android concatenated layout (captured on a Galaxy S20 FE)', () {
    // header(6) · FF FF · header+readings(14), exactly as flutter_blue_plus
    // delivered it: 94 %, seq 0x16, 0 lux, 2883 K, yaw -156, on-table.
    const List<int> android = <int>[
      0x1f, 0x5e, 0x00, 0x5e, 0x16, 0x00, //
      0xff, 0xff, //
      0x1f, 0x5e, 0x00, 0x5e, 0x16, 0x00, 0x00, 0x00, 0x43, 0x0b, 0x64, 0xff,
      0x01, 0x00,
    ];
    final MultisenseAdvert a = MultisenseAdvert.fromManufacturerData(
        <int, List<int>>{0xFFFF: android})!;
    expect(a.batteryPct, 94);
    expect(a.sequence, 0x16);
    expect(a.lux, 0);
    expect(a.cctK, 2883);
    expect(a.yawDeg, -156);
    expect(a.stability, 'on-table');
    expect(a.summary(), 'multisensor · 94% · 0 lx · 2883 K · yaw -156° · on-table');
  });

  test('"no reading" sentinels decode to null', () {
    final MultisenseAdvert a = MultisenseAdvert.fromManufacturerData(<int, List<int>>{
      0xFFFF: <int>[0x1F, 0x10, 0x00, 0xFF, 1, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF, 0],
    })!;
    expect(a.sensors, <String>['battery']);
    expect(a.batteryPct, isNull);
    expect(a.lux, isNull);
    expect(a.cctK, isNull);
    expect(a.yawDeg, isNull);
    expect(a.stability, isNull);
    expect(a.summary(), 'multisensor');
  });

  test('absent, short or unknown-version data is rejected', () {
    expect(MultisenseAdvert.fromManufacturerData(<int, List<int>>{}), isNull);
    expect(MultisenseAdvert.fromManufacturerData(<int, List<int>>{0x004C: header}), isNull);
    expect(MultisenseAdvert.fromManufacturerData(<int, List<int>>{0xFFFF: <int>[0x1F, 0]}),
        isNull);
    expect(
        MultisenseAdvert.fromManufacturerData(
            <int, List<int>>{0xFFFF: <int>[0x2F, 0, 0, 0, 0, 0]}),
        isNull);
  });
}
