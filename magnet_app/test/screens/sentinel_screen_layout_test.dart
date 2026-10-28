// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:magnet_app/gen/app_localizations.dart';
import 'package:magnet_app/magnet/sentinel_feed.dart';
import 'package:magnet_app/screens/sentinel_screen.dart';
import 'package:provider/provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

const String base = SentinelFeed.base;

String state(String st) =>
    '{"id":"3cf4","ev":"state","state":"$st","ts":${_ts()},"local":"","up_s":100,'
    '"since_s":0,"online":true,"stats":{"hour":{"entries":2,"occ_s":2400},'
    '"day":{"entries":12,"occ_s":24000},"week":{"entries":40,"occ_s":90000},'
    '"month":{"entries":160,"occ_s":360000}}}';

String activity({required bool active, required String st, int? clearIn, int lastMotion = 13}) =>
    '{"id":"3cf4","active":$active,"state":"$st","ts":${_ts()},"last_motion_s":$lastMotion,'
    '"clear_in_s":${clearIn ?? 'null'},"hold_s":300,"quiet_s":$lastMotion,"quiet_after_s":43200,'
    '"check_sensor":false,"bin_s":10,"bins_pct":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,40,90,100,60]}';

int _ts() => DateTime.now().millisecondsSinceEpoch ~/ 1000;

void main() {
  // The Sharp SH-53D: 720 x 1280 px at 2x = 360 dp wide.
  testWidgets('the card does not change height as motion comes and goes', (WidgetTester t) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    t.view.physicalSize = const Size(720, 1280);
    t.view.devicePixelRatio = 2;
    addTearDown(t.view.reset);

    final SentinelFeed feed = SentinelFeed(connectOnStart: false);
    feed.ingest('$base/3cf4/state', state('present'));
    await t.pumpWidget(ChangeNotifierProvider<SentinelFeed>.value(
      value: feed,
      child: const MaterialApp(
        locale: Locale('en'),
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: SentinelScreen(),
      ),
    ));

    final Map<String, double> y = <String, double>{};
    Future<void> measure(String label, String act, [String? st]) async {
      if (st != null) feed.ingest('$base/3cf4/state', state(st));
      feed.ingest('$base/3cf4/activity', act);
      await t.pump();
      y[label] = t.getTopLeft(find.text('Last 5 minutes')).dy;
    }

    await measure('motion now', activity(active: true, st: 'present', clearIn: 300, lastMotion: 0));
    await measure('counting down', activity(active: false, st: 'present', clearIn: 287));
    await measure('long countdown', activity(active: false, st: 'present', clearIn: 4000, lastMotion: 3999));
    await measure('clear', activity(active: false, st: 'clear', lastMotion: 400), 'clear');

    expect(y.values.toSet(), hasLength(1), reason: 'heading y per state: $y');
    // Timers left by the screen's 1 s tick.
    await t.pumpWidget(const SizedBox());
  });
}
