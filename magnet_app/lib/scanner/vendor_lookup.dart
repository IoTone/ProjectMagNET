// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// Minimal offline lookup of Bluetooth SIG "Company Identifiers"
/// (the 16-bit id in a BLE manufacturer-data advertisement) to a
/// vendor name. This is a deliberately small starter table — extend
/// it from the official assignments at
/// https://www.bluetooth.com/specifications/assigned-numbers/ as
/// needed. Offline-first: shipped in-app, never fetched.
class VendorLookup {
  VendorLookup._();

  static const Map<int, String> _companies = <int, String>{
    0x0006: 'Microsoft',
    0x004C: 'Apple, Inc.',
    0x0075: 'Samsung Electronics',
    0x00E0: 'Google',
    0x0059: 'Nordic Semiconductor',
    0x0157: 'Anhui Huami (Amazfit)',
    0x0171: 'Amazon',
    0x0499: 'Ruuvi Innovations',
    0x05A7: 'Sonos',
    0x0087: 'Garmin',
    0x004F: 'APT (Qualcomm)',
    0x0078: 'Nike',
    0x0001: 'Ericsson',
    0x000D: 'Texas Instruments',
    0x0822: 'Adafruit',
    0x0A12: 'Espressif',
  };

  /// Vendor name for a Bluetooth company id, or null if unknown.
  static String? forCompanyId(int? companyId) =>
      companyId == null ? null : _companies[companyId];
}
