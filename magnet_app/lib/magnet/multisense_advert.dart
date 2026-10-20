// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// Decoder for the MagNET capability advert (manufacturer data), first used by
/// the Multisensor (ArtemisNano_Hive_Multisense README §5.1).
///
/// Company id 0xFFFF (Bluetooth SIG "testing") until a real id is registered.
/// Header, 6 bytes:
///   [0]    format version (hi nibble) · role number (lo nibble; 15 = Multisensor)
///   [1..2] sensor bitmap, little-endian (see [MultisenseAdvert.sensorNames])
///   [3]    battery %, 0xFF = no gauge
///   [4]    reading sequence number
///   [5]    installed role-bundle tag, 0 = none
/// Readings, 8 more bytes (scan response; Android merges it over the advert):
///   [6..7]   lux, u16 LE, 0xFFFF = none
///   [8..9]   CCT in K, u16 LE, 0xFFFF = none
///   [10..11] yaw in degrees, s16 LE, 0x8000 = none
///   [12]     stability 0..4, 0xFF = none
///   [13]     significant-motion count, low byte
class MultisenseAdvert {
  const MultisenseAdvert({
    required this.version,
    required this.role,
    required this.sensors,
    required this.sequence,
    required this.bundleTag,
    this.batteryPct,
    this.lux,
    this.cctK,
    this.yawDeg,
    this.stability,
    this.sigMotion,
  });

  static const int companyTest = 0xFFFF;
  static const int roleMultisensor = 15;

  /// Bit i of the sensor bitmap.
  static const List<String> sensorNames = <String>[
    'presence',
    'lux',
    'color',
    'time',
    'battery',
    'sound',
    'orientation',
  ];

  static const List<String> stabilityNames = <String>[
    'unknown',
    'on-table',
    'stationary',
    'stable',
    'motion',
  ];

  final int version;
  final int role;
  final List<String> sensors;
  final int sequence;
  final int bundleTag;
  final int? batteryPct;

  /// Present only when the scan response was merged in.
  final int? lux;
  final int? cctK;
  final int? yawDeg;
  final String? stability;
  final int? sigMotion;

  bool get hasReadings => lux != null || cctK != null || yawDeg != null;

  String get roleName => role == roleMultisensor ? 'multisensor' : 'role $role';

  /// Parse from a scan's manufacturer data map (company id already split off,
  /// as flutter_blue_plus delivers it). Null when absent or malformed.
  static MultisenseAdvert? fromManufacturerData(Map<int, List<int>> mfg) {
    final List<int>? b = mfg[companyTest];
    if (b == null || b.length < 6) return null;
    final int version = b[0] >> 4;
    if (version != 1) return null; // unknown format: don't guess

    final int bitmap = b[1] | (b[2] << 8);
    final List<String> sensors = <String>[
      for (int i = 0; i < sensorNames.length; i++)
        if ((bitmap & (1 << i)) != 0) sensorNames[i],
    ];

    int u16(int at) => b[at] | (b[at + 1] << 8);
    int? lux, cct, yaw, sig;
    String? stab;
    if (b.length >= 14) {
      final int l = u16(6), c = u16(8), y = u16(10);
      lux = l == 0xFFFF ? null : l;
      cct = c == 0xFFFF ? null : c;
      yaw = y == 0x8000 ? null : (y >= 0x8000 ? y - 0x10000 : y);
      stab = b[12] == 0xFF
          ? null
          : (b[12] < stabilityNames.length ? stabilityNames[b[12]] : '${b[12]}');
      sig = b[13];
    }

    return MultisenseAdvert(
      version: version,
      role: b[0] & 0x0F,
      sensors: sensors,
      batteryPct: b[3] == 0xFF ? null : b[3],
      sequence: b[4],
      bundleTag: b[5],
      lux: lux,
      cctK: cct,
      yawDeg: yaw,
      stability: stab,
      sigMotion: sig,
    );
  }

  /// One-line summary for a device list row.
  String summary() {
    final List<String> parts = <String>[roleName];
    if (batteryPct != null) parts.add('$batteryPct%');
    if (lux != null) parts.add('$lux lx');
    if (cctK != null && cctK! > 0) parts.add('$cctK K');
    if (yawDeg != null) parts.add('yaw $yawDeg°');
    if (stability != null) parts.add(stability!);
    return parts.join(' · ');
  }
}
