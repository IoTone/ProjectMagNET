// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// R21 / U12 — tracks whether the user has been through the first-run
/// intro (permissions explainer + initial Grant/Skip). One bool stored
/// in `SharedPreferences`. Two terminal states:
/// - `done == false` → show the intro screen instead of the main app.
/// - `done == true`  → main app boots straight to the shell.
///
/// While the pref is loading (`loaded == false`), the gate shows a
/// tiny splash placeholder; this is almost always a single frame in
/// practice but explicit so tests don't need to race the load.
class FirstRunController extends ChangeNotifier {
  FirstRunController({bool? initialDone})
      : _done = initialDone ?? false,
        _loaded = initialDone != null;

  /// Test helper: pre-loaded controller with a chosen [done] state.
  factory FirstRunController.preloaded({required bool done}) =>
      FirstRunController(initialDone: done);

  static const String _key = 'app.firstRun.done';
  bool _done;
  bool _loaded;

  bool get loaded => _loaded;
  bool get done => _done;

  Future<void> load() async {
    if (_loaded) return;
    final SharedPreferences p = await SharedPreferences.getInstance();
    _done = p.getBool(_key) ?? false;
    _loaded = true;
    notifyListeners();
  }

  Future<void> markDone() async {
    if (_done) return;
    final SharedPreferences p = await SharedPreferences.getInstance();
    await p.setBool(_key, true);
    _done = true;
    notifyListeners();
  }

  /// Test/debug only: wipes the flag so the intro shows again next
  /// launch. Exposed via App settings → "Show intro again on next
  /// launch" so testers can verify the flow.
  Future<void> reset() async {
    final SharedPreferences p = await SharedPreferences.getInstance();
    await p.remove(_key);
    _done = false;
    notifyListeners();
  }
}
