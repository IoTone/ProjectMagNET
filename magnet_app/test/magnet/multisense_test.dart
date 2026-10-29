// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter_test/flutter_test.dart';
import 'package:magnet_app/magnet/multisense.dart';

/// The bytes a Role-15 node actually broadcasts, built the way
/// `ArtemisNano_Hive_Multisense/src/hive_ble.c:build_payloads` builds them, so
/// a change on either side of the wire shows up here.
List<int> _rsp({
  int format = 1,
  int role = 15,
  int caps = 0x5e, // lux|color|time|battery|orientation, as the bench saw it
  int soc = 96,
  int seq = 7,
  int bundleTag = 0,
  int lux = 15,
  int cct = 3577,
  int yaw = -42,
  int stability = 1,
  int sigMotion = 3,
}) =>
    <int>[
      (format << 4) | role,
      caps & 0xFF,
      (caps >> 8) & 0xFF,
      soc,
      seq,
      bundleTag,
      lux & 0xFF,
      (lux >> 8) & 0xFF,
      cct & 0xFF,
      (cct >> 8) & 0xFF,
      yaw & 0xFF,
      (yaw >> 8) & 0xFF,
      stability,
      sigMotion,
    ];

void main() {
  group('advert decode', () {
    test('full scan response', () {
      final MultisenseReadings? r = MultisenseReadings.decodeAdvert(
          <int, List<int>>{0xFFFF: _rsp()});

      expect(r, isNotNull);
      expect(r!.isMultisensor, isTrue);
      expect(r.formatVersion, 1);
      expect(r.seq, 7);
      expect(r.socPct, 96);
      expect(r.lux, 15);
      expect(r.cct, 3577);
      expect(r.yawDeg, -42); // s16, so a negative yaw must survive
      expect(r.stability, MsStability.onTable);
      expect(r.sigMotion, 3);
      expect(r.fromAdvert, isTrue);
      expect(r.caps.lux, isTrue);
      expect(r.caps.color, isTrue);
      expect(r.caps.battery, isTrue);
      expect(r.caps.orientation, isTrue);
      expect(r.caps.presence, isFalse); // nothing fitted on this node
      expect(r.caps.sound, isFalse);
      expect(r.summaryLine, '15 lx  ·  3577 K  ·  -42°  ·  96%');
    });

    test('header-only block (the advert, before Android merges the response)',
        () {
      final MultisenseReadings? r = MultisenseReadings.decodeAdvert(
          <int, List<int>>{0xFFFF: _rsp().sublist(0, 6)});

      expect(r, isNotNull);
      expect(r!.socPct, 96);
      expect(r.caps.lux, isTrue);
      // No readings in six bytes — and they must not read as zero.
      expect(r.lux, isNull);
      expect(r.cct, isNull);
      expect(r.yawDeg, isNull);
    });

    test('host-concatenated block: readings are the last 8 bytes', () {
      // Captured off the air from MagNET-7d8f, 2026-10-29. CoreBluetooth
      // concatenates the advert's manufacturer block with the scan
      // response's instead of replacing it, so the 6-byte header appears
      // twice and the readings start at 12, not 6.
      final List<int> onAir = <int>[
        0x1f, 0x5e, 0x00, 0x62, 0x84, 0x00, // header
        0x1f, 0x5e, 0x00, 0x62, 0x84, 0x00, // header again
        0xff, 0xff, 0xff, 0xff, 0xab, 0x00, 0x03, 0x05, // readings
      ];

      final MultisenseReadings? r =
          MultisenseReadings.decodeAdvert(<int, List<int>>{0xFFFF: onAir});

      expect(r, isNotNull);
      expect(r!.role, 15);
      expect(r.socPct, 98);
      expect(r.seq, 0x84);
      expect(r.caps.lux, isTrue); // detected at boot...
      expect(r.lux, isNull); // ...but 0xFFFF: no reading
      expect(r.cct, isNull);
      expect(r.yawDeg, 171);
      expect(r.stability, MsStability.stable);
      expect(r.sigMotion, 5);
      // Decoding at offset 6 would read the repeated header as data and
      // produce lux 24095 / cct 25088 / yaw 122.
      expect(r.summaryLine, '171°  ·  98%');
    });

    test('sentinels mean "no reading", not zero', () {
      final MultisenseReadings? r =
          MultisenseReadings.decodeAdvert(<int, List<int>>{
        0xFFFF: _rsp(
            soc: 0xFF, lux: 0xFFFF, cct: 0xFFFF, yaw: 0x8000, stability: 0xFF),
      });

      expect(r, isNotNull);
      expect(r!.socPct, isNull);
      expect(r.lux, isNull);
      expect(r.cct, isNull);
      expect(r.yawDeg, isNull);
      expect(r.stability, isNull);
      expect(r.hasAnyReading, isFalse);
    });

    test('ignores other vendors, short blocks and unknown formats', () {
      expect(
          MultisenseReadings.decodeAdvert(
              const <int, List<int>>{0x004C: <int>[1, 2, 3]}),
          isNull);
      expect(
          MultisenseReadings.decodeAdvert(
              const <int, List<int>>{0xFFFF: <int>[1, 2]}),
          isNull);
      // Format 2 would mean something else; decoding it as format 1 would
      // print confident nonsense.
      expect(
          MultisenseReadings.decodeAdvert(
              <int, List<int>>{0xFFFF: _rsp(format: 2)}),
          isNull);
      expect(
          MultisenseReadings.decodeAdvert(const <int, List<int>>{}), isNull);
    });
  });

  group('CAPS sensor set', () {
    test('parses the sensors= field the node sends', () {
      final MsSensorSet set = MsSensorSet.fromCaps(<String, String>{
        'role': 'multisensor',
        'sensors': 'orientation:bno085,motion:bno085,lux:opt4048,'
            'cct:opt4048,time:rv1805,battery:lc709203f',
      });

      expect(set.isKnown, isTrue);
      expect(set.has('lux'), isTrue);
      expect(set.parts['lux'], 'opt4048');
      expect(set.parts['battery'], 'lc709203f');
      expect(set.has('sound'), isFalse);
    });

    test('a node whose OPT4048 never answered omits lux and cct', () {
      // This is the bench case: the firmware clears MS_CAP_LUX|MS_CAP_COLOR
      // when the probe or opt4048_init() fails, so CAPS simply has no entry.
      final MsSensorSet set = MsSensorSet.fromCaps(<String, String>{
        'sensors': 'orientation:bno085,motion:bno085,time:rv1805,'
            'battery:lc709203f',
      });

      expect(set.isKnown, isTrue);
      expect(set.has('lux'), isFalse);
      expect(set.has('cct'), isFalse);
      expect(set.has('orientation'), isTrue);
    });

    test('no sensors= field is "unknown", not "found nothing"', () {
      expect(MsSensorSet.fromCaps(const <String, String>{}).isKnown, isFalse);
      expect(
          MsSensorSet.fromCaps(const <String, String>{'sensors': ''}).isKnown,
          isFalse);
    });
  });

  group('SENSE decode', () {
    test('a full reply from the bench', () {
      final MultisenseReadings r = MultisenseReadings.fromSense(
        <String, String>{
          'seq': '12',
          'time': '2026-10-29T11:02:33',
          'vbat': '4.067',
          'soc': '87',
          'tbat': 'na',
          'lux': '15.2',
          'cct': '3577',
          'x': '0.3980',
          'y': '0.3790',
          'yaw': '-41.6',
          'pitch': '2.1',
          'roll': '0.4',
          'hacc': '180',
          'motion': 'on-table',
          'sigmo': '3',
        },
      );

      expect(r.seq, 12);
      expect(r.timeIso, '2026-10-29T11:02:33');
      expect(r.vbatVolts, 4.067);
      expect(r.socPct, 87);
      expect(r.tbatCelsius, isNull); // no thermistor fitted
      expect(r.lux, 15.2);
      expect(r.cieX, 0.398);
      expect(r.yawDeg, -41.6);
      expect(r.headingAccDeg, 180);
      expect(r.stability, MsStability.onTable);
      expect(r.sigMotion, 3);
      expect(r.fromAdvert, isFalse);
    });

    test('a node with only light fitted reports only light', () {
      final MultisenseReadings r = MultisenseReadings.fromSense(
          <String, String>{'seq': '4', 'lux': '0.3', 'cct': '2700'});

      expect(r.lux, 0.3);
      expect(r.socPct, isNull);
      expect(r.yawDeg, isNull);
      expect(r.stability, isNull);
      expect(r.summaryLine, '0.3 lx  ·  2700 K');
    });
  });
}
