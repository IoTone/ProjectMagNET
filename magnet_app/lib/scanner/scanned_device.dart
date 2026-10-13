// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// A Bluetooth LE device observed during a scan. Keyed by [id] (the
/// platform device identifier — a MAC on Android, an opaque UUID on
/// iOS). Mutable: each fresh advertisement updates [rssi] and
/// [lastSeen] in place so the radar/list animate without rebuilding
/// the whole table.
class ScannedDevice {
  ScannedDevice({
    required this.id,
    required this.name,
    required this.rssi,
    required this.firstSeen,
    required this.lastSeen,
    this.txPowerLevel,
    Map<int, List<int>>? manufacturerData,
    List<String>? serviceUuids,
  })  : manufacturerData = manufacturerData ?? const <int, List<int>>{},
        serviceUuids = serviceUuids ?? const <String>[];

  /// Platform device id (Android MAC / iOS UUID). Stable per session.
  final String id;

  /// Advertised name, or empty when the device broadcasts none.
  String name;

  /// Latest RSSI in dBm (negative; closer to 0 = stronger).
  int rssi;

  /// Calibrated TX power at 1 m, when the device advertises it.
  int? txPowerLevel;

  /// Manufacturer-specific advertisement data, keyed by company id.
  /// The company id (first 2 bytes, little-endian) feeds the vendor
  /// lookup in the device-detail sheet.
  Map<int, List<int>> manufacturerData;

  /// Advertised GATT service UUIDs (lowercased strings).
  List<String> serviceUuids;

  final DateTime firstSeen;
  DateTime lastSeen;

  /// First advertised manufacturer company id, or null. The 16-bit
  /// Bluetooth SIG "Company Identifier" used for vendor attribution.
  int? get companyId =>
      manufacturerData.isEmpty ? null : manufacturerData.keys.first;

  String get displayName => name.isEmpty ? id : name;

  bool get isNamed => name.isNotEmpty;
}
