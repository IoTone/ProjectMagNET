// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:package_info_plus/package_info_plus.dart';

/// Global app state. Owns app-metadata lookup and exposes ChangeNotifier
/// hooks for the rest of the UI.
class AppState extends ChangeNotifier {
  static PackageInfo? packageInfo;
  static String appversion = 'UNKNOWN';
  static String fullappname = 'UNKNOWN';

  AppState() {
    // Fire and forget; consumers should call [initAppInfo] and await it
    // when they need the values to be ready.
    initAppInfo();
  }

  Future<void> initAppInfo() async {
    packageInfo = await PackageInfo.fromPlatform();
    final PackageInfo info = packageInfo!;
    appversion = '${info.version}.${info.buildNumber}';
    fullappname = '${info.appName}$appversion';
    notifyListeners();
  }

  /// Notify listeners that the active screen has changed.
  void changeToNewScreen() => notifyListeners();

  String getAppVersion() => appversion;
  String getAppName() => fullappname;
}
