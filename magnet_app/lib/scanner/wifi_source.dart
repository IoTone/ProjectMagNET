// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:async';

import 'scanned_access_point.dart';

/// How much WiFi visibility a source can provide on this platform.
enum WifiCapability {
  /// Full nearby-AP scan (Android with permissions; or a relay proxy).
  fullScan,

  /// Only the currently-connected network is observable (iOS).
  connectedOnly,

  /// No WiFi data available (permission denied, unsupported, etc.).
  unavailable,
}

/// Abstraction over WiFi observation. Concrete implementations are
/// platform-specific (Android full scan, iOS connected-only) plus the
/// optional mDNS relay proxy — the controller treats them uniformly.
abstract class WifiSource {
  Stream<List<ScannedAccessPoint>> get accessPoints;
  WifiCapability get capability;
  Future<void> start();
  Future<void> stop();
  void dispose();
}

/// Default source when WiFi is off or unsupported: emits nothing.
class NoopWifiSource implements WifiSource {
  @override
  Stream<List<ScannedAccessPoint>> get accessPoints =>
      const Stream<List<ScannedAccessPoint>>.empty();

  @override
  WifiCapability get capability => WifiCapability.unavailable;

  @override
  Future<void> start() async {}

  @override
  Future<void> stop() async {}

  @override
  void dispose() {}
}
