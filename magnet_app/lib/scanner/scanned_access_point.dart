// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// A WiFi access point observed during a scan. Keyed by [bssid] (the
/// AP's MAC). On Android these come from a full nearby-AP scan; on
/// iOS only the currently-connected network is available (Apple gives
/// no third-party AP-scanning API), so iOS yields at most one entry.
class ScannedAccessPoint {
  ScannedAccessPoint({
    required this.ssid,
    required this.bssid,
    required this.rssi,
    required this.lastSeen,
    this.frequencyMhz,
    this.capabilities = '',
    this.isCurrentConnection = false,
  });

  /// Network name. May be empty for hidden networks.
  final String ssid;

  /// AP hardware address (MAC). Stable key.
  final String bssid;

  /// Signal level in dBm.
  final int rssi;

  /// Centre frequency in MHz (e.g. 2412, 5180), when known.
  final int? frequencyMhz;

  /// Raw security/capabilities string (e.g. "[WPA2-PSK-CCMP][ESS]").
  final String capabilities;

  /// True when this is the device's own current WiFi connection
  /// (the only thing iOS can report).
  final bool isCurrentConnection;

  final DateTime lastSeen;

  String get displayName => ssid.isEmpty ? '(hidden)' : ssid;

  /// Coarse 2.4 / 5 / 6 GHz band label from the centre frequency.
  String get band {
    final int? f = frequencyMhz;
    if (f == null) return '';
    if (f >= 2400 && f < 2500) return '2.4 GHz';
    if (f >= 5000 && f < 5900) return '5 GHz';
    if (f >= 5925) return '6 GHz';
    return '';
  }

  /// WiFi channel number derived from the centre frequency (2.4 +
  /// 5 GHz). Returns null when it can't be resolved.
  int? get channel {
    final int? f = frequencyMhz;
    if (f == null) return null;
    if (f == 2484) return 14;
    if (f >= 2412 && f <= 2472) return (f - 2407) ~/ 5;
    if (f >= 5180 && f <= 5885) return (f - 5000) ~/ 5;
    return null;
  }
}
