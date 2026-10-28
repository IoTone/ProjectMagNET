// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter_test/flutter_test.dart';
import 'package:magnet_app/magnet/sentinel_feed.dart';

const String base = SentinelFeed.base;

// Captured from MagNET-Sentinel-3cf4 (fw 0.1.0) on 2026-10-26.
const String liveState =
    '{"id":"3cf4","ev":"state","state":"clear","ts":1793076026,'
    '"local":"2026-10-26T21:40:26-0700","up_s":30,"since_s":0,"online":true,'
    '"stats":{"hour":{"entries":1,"occ_s":2368},"day":{"entries":1,"occ_s":2368},'
    '"week":{"entries":1,"occ_s":2368},"month":{"entries":1,"occ_s":2368}}}';

String ev(String kind, String state, {String extra = '', int? ts = 1793076100}) =>
    '{"id":"3cf4","ev":"$kind","state":"$state","ts":${ts ?? 'null'},"local":"",'
    '"up_s":100,"since_s":0$extra,"stats":{"hour":{"entries":2,"occ_s":2400},'
    '"day":{"entries":2,"occ_s":2400},"week":{"entries":2,"occ_s":2400},'
    '"month":{"entries":2,"occ_s":2400}}}';

void main() {
  group('SentinelMessage.parse', () {
    test('decodes a live state payload', () {
      final SentinelMessage m =
          SentinelMessage.parse(base, '$base/3cf4/state', liveState, retained: true)!;
      expect(m.id, '3cf4');
      expect(m.ev, 'state');
      expect(m.state, 'clear');
      expect(m.online, isTrue);
      expect(m.clockKnown, isTrue);
      expect(m.at, DateTime.fromMillisecondsSinceEpoch(1793076026000));
      expect(m.stats['hour']!.entries, 1);
      expect(m.stats['month']!.occupiedS, 2368);
      expect(m.retained, isTrue);
    });

    test('the last will reads as offline', () {
      final SentinelMessage m = SentinelMessage.parse(base, '$base/3cf4/state',
          '{"id":"3cf4","ev":"state","state":"offline","online":false}')!;
      expect(m.online, isFalse);
      expect(m.state, 'offline');
      expect(m.stats, isEmpty);
    });

    test('ts null means the device clock is not set; arrival time is used', () {
      final DateTime now = DateTime(2026, 10, 26, 21, 0);
      final SentinelMessage m = SentinelMessage.parse(
          base, '$base/3cf4/event', ev('enter', 'present', ts: null), now: now)!;
      expect(m.clockKnown, isFalse);
      expect(m.at, now);
    });

    test('rollover and reset fields', () {
      final SentinelMessage r = SentinelMessage.parse(base, '$base/3cf4/event',
          ev('rollover', 'clear',
              extra: ',"period":"hour","label":"2026-10-26T21","entries":3,"occ_s":900'))!;
      expect(r.period, 'hour');
      expect(r.label, '2026-10-26T21');
      expect(r.entries, 3);
      expect(r.occupiedS, 900);
      final SentinelMessage z = SentinelMessage.parse(
          base, '$base/3cf4/event', ev('reset', 'clear', extra: ',"periods":["day","week"]'))!;
      expect(z.periods, <String>['day', 'week']);
    });

    test('ignores other topics, bad JSON and cleared retained topics', () {
      expect(SentinelMessage.parse(base, 'iotj/axrawe/updates/3cf4', liveState), isNull);
      expect(SentinelMessage.parse(base, '$base/3cf4/other', liveState), isNull);
      expect(SentinelMessage.parse(base, '$base/3cf4/state', 'not json'), isNull);
      expect(SentinelMessage.parse(base, '$base/3cf4/state', ''), isNull);
      expect(SentinelMessage.parse(base, '$base/3cf4/state', '[1,2]'), isNull);
    });
  });

  group('SentinelFeed.ingest', () {
    test('state, then a visit: enter → present → clear', () {
      final SentinelFeed f = SentinelFeed();
      final DateTime t0 = DateTime(2026, 10, 26, 21, 40);
      f.ingest('$base/3cf4/state', liveState, retained: true, now: t0);
      SentinelDevice d = f.devices.single;
      expect(d.online, isTrue);
      expect(d.state, 'clear');
      expect(d.present, isFalse);
      expect(d.events, isEmpty);
      expect(f.presentCount, 0);

      f.ingest('$base/3cf4/event', ev('enter', 'present'), now: t0);
      d = f.devices.single;
      expect(d.present, isTrue);
      expect(f.presentCount, 1);
      expect(d.stats['day']!.entries, 2);
      expect(d.stateSince, DateTime.fromMillisecondsSinceEpoch(1793076100000));

      f.ingest('$base/3cf4/event', ev('present', 'present', extra: ',"visit_s":300'), now: t0);
      f.ingest('$base/3cf4/event', ev('clear', 'clear', extra: ',"visit_s":420'), now: t0);
      expect(f.devices.single.events.map((SentinelMessage e) => e.ev),
          <String>['clear', 'present', 'enter']);
      expect(f.presentCount, 0);
    });

    test('a stale retained state is dated by the device clock', () {
      final SentinelFeed f = SentinelFeed();
      // Sent at 21:40:26 device time with since_s 0; arrives 13 min later.
      final DateTime rx = DateTime.fromMillisecondsSinceEpoch(1793076026000)
          .add(const Duration(minutes: 13));
      f.ingest('$base/3cf4/state', liveState, retained: true, now: rx);
      final SentinelDevice d = f.devices.single;
      expect(d.stateSince, DateTime.fromMillisecondsSinceEpoch(1793076026000));
      expect(d.lastHeard, DateTime.fromMillisecondsSinceEpoch(1793076026000));
    });

    test('offline last will keeps the stats and events', () {
      final SentinelFeed f = SentinelFeed();
      f.ingest('$base/3cf4/event', ev('enter', 'present'));
      f.ingest('$base/3cf4/state', '{"id":"3cf4","ev":"state","state":"offline","online":false}');
      final SentinelDevice d = f.devices.single;
      expect(d.online, isFalse);
      expect(d.present, isFalse);
      expect(d.stats['hour']!.entries, 2);
      expect(d.events, hasLength(1));
    });

    test('events are capped and devices sorted by id', () {
      final SentinelFeed f = SentinelFeed();
      for (int i = 0; i < SentinelFeed.maxEvents + 5; i++) {
        f.ingest('$base/3cf4/event', ev('present', 'present'));
      }
      f.ingest('$base/0aa1/state', liveState.replaceAll('3cf4', '0aa1'));
      expect(f.devices.map((SentinelDevice d) => d.id), <String>['0aa1', '3cf4']);
      expect(f.devices.last.events, hasLength(SentinelFeed.maxEvents));
    });
  });

  group('activity + history (fw 0.2.0)', () {
    // Captured from 3cf4 during an 8 s pir-sim with a 30 s hold.
    const String act =
        '{"id":"3cf4","active":false,"state":"present","ts":1793077607,'
        '"last_motion_s":10,"clear_in_s":20,"hold_s":30,"bin_s":10,"bins_pct":'
        '[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,55,25,0]}';
    final DateTime sent = DateTime.fromMillisecondsSinceEpoch(1793077607000);

    test('countdown and last motion run on from the message time', () {
      final SentinelFeed f = SentinelFeed();
      f.ingest('$base/3cf4/state', ev('enter', 'present').replaceFirst('"ev":"enter"', '"ev":"state"'));
      f.ingest('$base/3cf4/activity', act, now: sent);
      final SentinelDevice d = f.devices.single;
      expect(d.active, isFalse);
      expect(d.clearInAt(sent), 20);
      expect(d.clearInAt(sent.add(const Duration(seconds: 15))), 5);
      expect(d.clearInAt(sent.add(const Duration(seconds: 90))), 0);
      expect(d.lastMotionAgoAt(sent.add(const Duration(seconds: 5))), 15);
      expect(d.recentPct.length, 30);
    });

    test('while motion is on, the hold is full and "ago" is zero', () {
      final SentinelFeed f = SentinelFeed();
      f.ingest('$base/3cf4/state', ev('enter', 'present').replaceFirst('"ev":"enter"', '"ev":"state"'));
      f.ingest('$base/3cf4/activity', act.replaceFirst('"active":false', '"active":true'), now: sent);
      final SentinelDevice d = f.devices.single;
      expect(d.clearInAt(sent.add(const Duration(seconds: 40))), 30);
      expect(d.lastMotionAgoAt(sent.add(const Duration(seconds: 40))), 0);
    });

    test('series scroll with time when no update arrives', () {
      final SentinelFeed f = SentinelFeed();
      f.ingest('$base/3cf4/activity', act, now: sent);
      final SentinelDevice d = f.devices.single;
      final List<int> later = d.recentAt(sent.add(const Duration(seconds: 25)));
      expect(later.sublist(25, 27), <int>[55, 25]);           // shifted two bins left
      expect(d.recentAt(sent.add(const Duration(minutes: 6))).every((int v) => v == 0), isTrue);

      final List<int> bins = List<int>.filled(96, 0)..[95] = 420;
      f.ingest('$base/3cf4/history',
          '{"id":"3cf4","ts":1793077615,"bin_s":900,"end":1793077200,"bins_s":$bins}');
      final DateTime end = DateTime.fromMillisecondsSinceEpoch(1793077200000);
      expect(d.historyAt(end.add(const Duration(minutes: 5)))[95], 420);
      expect(d.historyAt(end.add(const Duration(minutes: 31)))[93], 420);
    });

    test('history with an unknown clock has no end and does not shift', () {
      final SentinelFeed f = SentinelFeed();
      f.ingest('$base/3cf4/history', '{"id":"3cf4","ts":null,"bin_s":900,"end":null,"bins_s":[1,2,3]}');
      final SentinelDevice d = f.devices.single;
      expect(d.historyEnd, isNull);
      expect(d.historyAt(DateTime(2030)), <int>[1, 2, 3]);
    });
  });

  group('check sensor', () {
    final DateTime sent = DateTime.fromMillisecondsSinceEpoch(1793077607000);
    String quiet(int q, {bool active = false}) =>
        '{"id":"3cf4","active":$active,"state":"clear","ts":1793077607,"last_motion_s":$q,'
        '"clear_in_s":null,"hold_s":300,"quiet_s":$q,"quiet_after_s":43200,'
        '"check_sensor":${q >= 43200},"bin_s":10,"bins_pct":[]}';

    SentinelFeed online() {
      final SentinelFeed f = SentinelFeed();
      f.ingest('$base/3cf4/state', liveState);
      return f;
    }

    test('warns once silence passes the limit, counting on from the message', () {
      final SentinelFeed f = online();
      f.ingest('$base/3cf4/activity', quiet(43000), now: sent);
      final SentinelDevice d = f.devices.single;
      expect(d.checkSensorAt(sent), isFalse);
      expect(d.checkSensorAt(sent.add(const Duration(seconds: 200))), isTrue);
      expect(f.checkSensorCount(sent.add(const Duration(seconds: 200))), 1);
    });

    test('motion clears it; offline does not warn', () {
      final SentinelFeed f = online();
      f.ingest('$base/3cf4/activity', quiet(50000), now: sent);
      expect(f.devices.single.checkSensorAt(sent), isTrue);
      f.ingest('$base/3cf4/activity', quiet(0, active: true), now: sent);
      expect(f.devices.single.checkSensorAt(sent.add(const Duration(hours: 1))), isFalse);
      f.ingest('$base/3cf4/activity', quiet(50000), now: sent);
      f.ingest('$base/3cf4/state', '{"id":"3cf4","ev":"state","state":"offline","online":false}');
      expect(f.devices.single.checkSensorAt(sent), isFalse);
    });

    test('firmware without quiet fields never warns', () {
      final SentinelFeed f = online();
      f.ingest('$base/3cf4/activity',
          '{"id":"3cf4","active":false,"state":"clear","ts":1793077607,"bins_pct":[]}', now: sent);
      expect(f.devices.single.checkSensorAt(sent.add(const Duration(days: 3))), isFalse);
    });
  });

  test('a phone clock behind the Sentinel never shows negative time', () {
    final SentinelFeed f = SentinelFeed();
    f.ingest('$base/3cf4/state', liveState);
    final DateTime sent = DateTime.fromMillisecondsSinceEpoch(1793077607000);
    f.ingest('$base/3cf4/activity',
        '{"id":"3cf4","active":false,"state":"present","ts":1793077607,"last_motion_s":0,'
        '"clear_in_s":30,"hold_s":30,"quiet_s":0,"quiet_after_s":43200,"bins_pct":[]}',
        now: sent);
    final SentinelDevice d = f.devices.single;
    final DateTime behind = sent.subtract(const Duration(seconds: 1));
    expect(d.lastMotionAgoAt(behind), 0);
    expect(d.clearInAt(behind), 30);
  });

  test('counters carried in /activity replace stale ones', () {
    final SentinelFeed f = SentinelFeed();
    f.ingest('$base/3cf4/state', liveState);
    f.ingest('$base/3cf4/activity',
        '{"id":"3cf4","active":true,"state":"present","ts":1793077607,"bins_pct":[],'
        '"stats":{"hour":{"entries":0,"occ_s":42},"day":{"entries":5,"occ_s":4000},'
        '"week":{"entries":5,"occ_s":4000},"month":{"entries":5,"occ_s":4000}}}');
    expect(f.devices.single.stats['hour']!.occupiedS, 42);
    expect(f.devices.single.stats['day']!.entries, 5);
  });
}
