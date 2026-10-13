// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart' show kIsWeb;
import 'package:flutter/widgets.dart';
import 'package:provider/provider.dart';

import '../perms/permissions_service.dart';
import '../scanner/scanner_controller.dart';

/// Shared start/stop entry point so the Dashboard and Devices screens
/// behave identically: ensure BLE (and, on Android, location)
/// permission, then toggle the scan.
class ScanControls {
  ScanControls._();

  static Future<void> toggle(BuildContext context) async {
    final ScannerController sc = context.read<ScannerController>();
    if (sc.isScanning) {
      await sc.stop();
      return;
    }
    final PermissionsService perms = context.read<PermissionsService>();
    await perms.requestBle();
    // Location is required for WiFi/BLE scan results on Android, and
    // for reading the connected WiFi SSID/BSSID on iOS.
    if (!kIsWeb && (Platform.isAndroid || Platform.isIOS)) {
      await perms.requestLocation();
    }
    await sc.start();
  }
}
