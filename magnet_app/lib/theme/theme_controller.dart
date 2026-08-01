// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'tokens.dart';

/// User-customizable appearance & accessibility. Holds the active
/// theme preset (default [AppThemePreset.midnight]), a font-size scale
/// layered on top of the OS text scale, and accessibility flags.
/// Persisted via shared_preferences; the rest of the app reads only
/// this (single source of truth).
class ThemeController extends ChangeNotifier {
  static const String _kPreset = 'app.preset';
  static const String _kFontScale = 'app.fontScale';
  static const String _kHighContrast = 'app.highContrast';
  static const String _kReduceMotion = 'app.reduceMotion';

  AppThemePreset _preset = AppThemePreset.midnight;
  double _fontScale = 1.0;
  bool _highContrast = false;
  bool _reduceMotion = false;

  AppThemePreset get preset => _preset;
  double get fontScale => _fontScale;
  bool get highContrast => _highContrast;
  bool get reduceMotion => _reduceMotion;

  AppTokens get tokens => kAppThemes[_preset]!;

  /// Forced to the high-contrast Midnight token set when
  /// [highContrast] is on, regardless of the chosen preset.
  ThemeData get theme => buildAppTheme(
        _highContrast ? kAppThemes[AppThemePreset.midnight]! : tokens,
      );

  Future<void> load() async {
    final SharedPreferences p = await SharedPreferences.getInstance();
    _preset = presetFromId(p.getString(_kPreset));
    _fontScale = (p.getDouble(_kFontScale) ?? 1.0).clamp(0.8, 1.6);
    _highContrast = p.getBool(_kHighContrast) ?? false;
    _reduceMotion = p.getBool(_kReduceMotion) ?? false;
    notifyListeners();
  }

  Future<void> _save(void Function(SharedPreferences p) write) async {
    final SharedPreferences p = await SharedPreferences.getInstance();
    write(p);
  }

  Future<void> setPreset(AppThemePreset v) async {
    if (v == _preset) return;
    _preset = v;
    notifyListeners();
    await _save((SharedPreferences p) => p.setString(_kPreset, v.name));
  }

  Future<void> setFontScale(double v) async {
    final double c = v.clamp(0.8, 1.6);
    if (c == _fontScale) return;
    _fontScale = c;
    notifyListeners();
    await _save((SharedPreferences p) => p.setDouble(_kFontScale, c));
  }

  Future<void> setHighContrast(bool v) async {
    _highContrast = v;
    notifyListeners();
    await _save((SharedPreferences p) => p.setBool(_kHighContrast, v));
  }

  Future<void> setReduceMotion(bool v) async {
    _reduceMotion = v;
    notifyListeners();
    await _save((SharedPreferences p) => p.setBool(_kReduceMotion, v));
  }
}
