// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart' show kIsWeb;
import 'package:network_info_plus/network_info_plus.dart';
import 'package:wifi_scan/wifi_scan.dart';

import 'scanned_access_point.dart';
import 'wifi_source.dart';

/// Pick the best WiFi source for the current platform:
/// - Android → [OnDeviceWifiSource] (full nearby-AP scan)
/// - iOS     → [ConnectedNetworkSource] (connected network only)
/// - other   → [NoopWifiSource]
WifiSource createWifiSource() {
  if (kIsWeb) return NoopWifiSource();
  if (Platform.isAndroid) return OnDeviceWifiSource();
  if (Platform.isIOS) return ConnectedNetworkSource();
  return NoopWifiSource();
}

/// Android full WiFi scan via the `wifi_scan` plugin. Polls on an
/// interval because the OS throttles scan requests (~4 per 2 min on
/// many devices), so results refresh slowly by design.
class OnDeviceWifiSource implements WifiSource {
  OnDeviceWifiSource({this.interval = const Duration(seconds: 8)});

  final Duration interval;
  final StreamController<List<ScannedAccessPoint>> _ctrl =
      StreamController<List<ScannedAccessPoint>>.broadcast();
  Timer? _timer;

  @override
  Stream<List<ScannedAccessPoint>> get accessPoints => _ctrl.stream;

  @override
  WifiCapability get capability => WifiCapability.fullScan;

  @override
  Future<void> start() async {
    await _tick();
    _timer ??= Timer.periodic(interval, (_) => _tick());
  }

  @override
  Future<void> stop() async {
    _timer?.cancel();
    _timer = null;
  }

  Future<void> _tick() async {
    try {
      final CanStartScan can = await WiFiScan.instance.canStartScan();
      if (can == CanStartScan.yes) {
        await WiFiScan.instance.startScan();
      }
      final CanGetScannedResults canGet =
          await WiFiScan.instance.canGetScannedResults();
      if (canGet != CanGetScannedResults.yes) return;
      final List<WiFiAccessPoint> results =
          await WiFiScan.instance.getScannedResults();
      final DateTime now = DateTime.now();
      _ctrl.add(<ScannedAccessPoint>[
        for (final WiFiAccessPoint ap in results)
          ScannedAccessPoint(
            ssid: ap.ssid,
            bssid: ap.bssid,
            rssi: ap.level,
            frequencyMhz: ap.frequency,
            capabilities: ap.capabilities,
            lastSeen: now,
          ),
      ]);
    } catch (_) {
      // Permission revoked, location off, etc. — emit nothing.
    }
  }

  @override
  void dispose() {
    _timer?.cancel();
    _ctrl.close();
  }
}

/// iOS (and any platform without AP scanning): reports only the
/// currently-connected network from `network_info_plus`. Apple gives
/// third-party apps no nearby-AP scan API, so this is the most WiFi
/// visibility available. RSSI isn't exposed, so a "connected =
/// strong" placeholder is used for radar placement.
class ConnectedNetworkSource implements WifiSource {
  ConnectedNetworkSource({this.interval = const Duration(seconds: 10)});

  final Duration interval;
  final NetworkInfo _info = NetworkInfo();
  final StreamController<List<ScannedAccessPoint>> _ctrl =
      StreamController<List<ScannedAccessPoint>>.broadcast();
  Timer? _timer;

  @override
  Stream<List<ScannedAccessPoint>> get accessPoints => _ctrl.stream;

  @override
  WifiCapability get capability => WifiCapability.connectedOnly;

  @override
  Future<void> start() async {
    await _tick();
    _timer ??= Timer.periodic(interval, (_) => _tick());
  }

  @override
  Future<void> stop() async {
    _timer?.cancel();
    _timer = null;
  }

  Future<void> _tick() async {
    try {
      final String? rawSsid = await _info.getWifiName();
      final String? bssid = await _info.getWifiBSSID();
      if (bssid == null && rawSsid == null) {
        _ctrl.add(const <ScannedAccessPoint>[]);
        return;
      }
      // network_info_plus wraps the SSID in quotes on some platforms.
      final String ssid = (rawSsid ?? '').replaceAll('"', '');
      _ctrl.add(<ScannedAccessPoint>[
        ScannedAccessPoint(
          ssid: ssid,
          bssid: bssid ?? ssid,
          rssi: -50, // placeholder — iOS exposes no RSSI
          isCurrentConnection: true,
          lastSeen: DateTime.now(),
        ),
      ]);
    } catch (_) {
      _ctrl.add(const <ScannedAccessPoint>[]);
    }
  }

  @override
  void dispose() {
    _timer?.cancel();
    _ctrl.close();
  }
}
