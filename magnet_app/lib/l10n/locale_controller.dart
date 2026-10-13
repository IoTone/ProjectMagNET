// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// R4 — picks the active locale for the app. `null` = follow the OS
/// (Flutter's default `Localizations.localeOf` resolution); otherwise
/// override with the explicit choice. Persisted in shared_preferences
/// under `app.locale`.
class LocaleController extends ChangeNotifier {
  LocaleController({Locale? initial}) : _locale = initial;

  static const String _key = 'app.locale';

  Locale? _locale;
  bool _loaded = false;

  Locale? get locale => _locale;
  bool get loaded => _loaded;

  /// Supported locales matching `lib/l10n/app_*.arb`.
  static const List<Locale> supported = <Locale>[
    Locale('en'),
    Locale('ja'),
  ];

  Future<void> load() async {
    final SharedPreferences p = await SharedPreferences.getInstance();
    final String? raw = p.getString(_key);
    _locale = (raw == null || raw.isEmpty) ? null : Locale(raw);
    _loaded = true;
    notifyListeners();
  }

  Future<void> set(Locale? v) async {
    if (v == _locale) return;
    _locale = v;
    notifyListeners();
    final SharedPreferences p = await SharedPreferences.getInstance();
    if (v == null) {
      await p.remove(_key);
    } else {
      await p.setString(_key, v.languageCode);
    }
  }
}
