// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// Role 15 (Multisensor) readings, from either of the two places a node
/// publishes them.
///
/// A multisensor node broadcasts its readings in the advertisement so that
/// *any* listener gets them without connecting — that is the cheap radio mode
/// and the one the hive is built around. The same numbers are available at
/// higher precision over HCP (`SENSE`) once there is a link. This file decodes
/// both into the same shape so the UI does not care which it got.
///
/// Layout: `ArtemisNano_Hive_Multisense/README.md` §5.1, as built in
/// `src/hive_ble.c` (`build_payloads`).
library;

/// Company id the prototype advertises under: `0xFFFF` is the Bluetooth SIG
/// "for testing" identifier. A real SIG assignment replaces it before anything
/// ships, so keep the constant in one place.
const int kMultisenseCompanyId = 0xFFFF;

/// Sensors a node says it has. Detected by the firmware's boot-time bus probe,
/// not compiled in — a loose breakout clears its bit, which is exactly the
/// signal the UI wants to show.
class MsCaps {
  const MsCaps(this.bits);
  final int bits;

  bool get presence => bits & (1 << 0) != 0;
  bool get lux => bits & (1 << 1) != 0;
  bool get color => bits & (1 << 2) != 0;
  bool get time => bits & (1 << 3) != 0;
  bool get battery => bits & (1 << 4) != 0;
  bool get sound => bits & (1 << 5) != 0;
  bool get orientation => bits & (1 << 6) != 0;

  /// Short human list, in advert bit order.
  List<String> get names => <String>[
        if (presence) 'presence',
        if (lux) 'lux',
        if (color) 'color',
        if (time) 'time',
        if (battery) 'battery',
        if (sound) 'sound',
        if (orientation) 'motion',
      ];
}

/// How still the BNO085's classifier thinks the node is.
enum MsStability { unknown, onTable, stationary, stable, motion }

MsStability? _stabilityFromByte(int b) => switch (b) {
      0 => MsStability.unknown,
      1 => MsStability.onTable,
      2 => MsStability.stationary,
      3 => MsStability.stable,
      4 => MsStability.motion,
      _ => null, // 0xFF: no IMU fitted
    };

MsStability? _stabilityFromWord(String? s) => switch (s) {
      'unknown' => MsStability.unknown,
      'on-table' => MsStability.onTable,
      'stationary' => MsStability.stationary,
      'stable' => MsStability.stable,
      'motion' => MsStability.motion,
      _ => null,
    };

String stabilityLabel(MsStability s) => switch (s) {
      MsStability.unknown => 'unknown',
      MsStability.onTable => 'on table',
      MsStability.stationary => 'stationary',
      MsStability.stable => 'stable',
      MsStability.motion => 'moving',
    };

/// What a node says it *found* at boot, from the HCP `CAPS` reply's
/// `sensors=orientation:bno085,lux:opt4048,…` field.
///
/// This is the other half of the picture that [MultisenseReadings] alone
/// cannot give: `SENSE` omits a key both when a sensor was never detected and
/// when it was detected but its reads are failing. Those need different
/// actions — reseat the breakout vs. look at the driver — so the UI must not
/// collapse them into one blank space.
class MsSensorSet {
  const MsSensorSet(this.parts);

  /// Sensor kind (`lux`, `cct`, `orientation`, `motion`, `time`, `battery`)
  /// to the part that provides it (`opt4048`, `bno085`, …).
  final Map<String, String> parts;

  static const MsSensorSet unknown = MsSensorSet(<String, String>{});

  bool get isKnown => parts.isNotEmpty;
  bool has(String kind) => parts.containsKey(kind);

  /// Parse the `sensors=` field. Returns [unknown] when the node did not
  /// answer with one, which must not be mistaken for "found nothing".
  static MsSensorSet fromCaps(Map<String, String> caps) {
    final String? field = caps['sensors'];
    if (field == null || field.isEmpty) return unknown;
    final Map<String, String> out = <String, String>{};
    for (final String entry in field.split(',')) {
      final int i = entry.indexOf(':');
      if (i > 0) out[entry.substring(0, i)] = entry.substring(i + 1);
    }
    return MsSensorSet(out);
  }
}

/// One set of multisensor readings. Every field is nullable: a node reports
/// only the sensors it found, and the advert has sentinels for the rest.
class MultisenseReadings {
  const MultisenseReadings({
    this.formatVersion,
    this.role,
    this.caps = const MsCaps(0),
    this.seq,
    this.bundleTag,
    this.socPct,
    this.vbatVolts,
    this.tbatCelsius,
    this.lux,
    this.cct,
    this.cieX,
    this.cieY,
    this.yawDeg,
    this.pitchDeg,
    this.rollDeg,
    this.headingAccDeg,
    this.stability,
    this.sigMotion,
    this.timeIso,
    this.fromAdvert = false,
  });

  final int? formatVersion;

  /// Hive role number; 15 = Multisensor.
  final int? role;
  final MsCaps caps;

  /// Reading sequence number — distinguishes fresh data from a stale advert
  /// the phone is still holding in its scan cache.
  final int? seq;

  /// Low byte of the installed role-bundle hash; 0 = none (M7).
  final int? bundleTag;

  final int? socPct;
  final double? vbatVolts;

  /// Battery temperature; null when no thermistor is fitted (`tbat=na`).
  final double? tbatCelsius;

  /// Illuminance. From the advert this is rounded to whole lux and saturates
  /// at 65535; `SENSE` carries one decimal.
  final double? lux;

  /// Correlated colour temperature in kelvin.
  final double? cct;
  final double? cieX;
  final double? cieY;

  final double? yawDeg;
  final double? pitchDeg;
  final double? rollDeg;

  /// The BNO085's own estimate of its heading error. Reads ±180° until the
  /// magnetometer is calibrated by moving the board in a figure-8.
  final double? headingAccDeg;

  final MsStability? stability;

  /// Significant-motion events since boot. From the advert, only the low byte.
  final int? sigMotion;

  /// Wall clock from the node's RTC, as the node formatted it.
  final String? timeIso;

  /// True when this came from an advertisement rather than an HCP `SENSE`.
  final bool fromAdvert;

  bool get isMultisensor => role == 15;
  bool get hasAnyReading =>
      lux != null || cct != null || yawDeg != null || socPct != null;

  /// Decode the §5.1 manufacturer block.
  ///
  /// [manufacturerData] is the map `flutter_blue_plus` hands back: keyed by
  /// company id, with the id itself already stripped from the value.
  ///
  /// **The host concatenates the advertisement's block with the scan
  /// response's rather than replacing it** — measured on CoreBluetooth
  /// 2026-10-29, a 20-byte block carrying the 6-byte header *twice* followed
  /// by the 8 reading bytes:
  ///
  ///     1f 5e 00 62 84 00 | 1f 5e 00 62 84 00 | ff ff ff ff ab 00 03 05
  ///        header               header again       lux   cct  yaw  st sig
  ///
  /// So the readings are the **last 8 bytes**, not a fixed offset 6. That is
  /// right either way: a 14-byte block (scan response alone) puts them at 6,
  /// a 20-byte one at 12, and both are `length - 8`. Reading offset 6 blindly
  /// decodes the repeated header as a reading and yields confident nonsense
  /// (`lux` 24095 = the `1f 5e` of the second header copy).
  ///
  /// Returns null when this device carries no block we recognise.
  static MultisenseReadings? decodeAdvert(
      Map<int, List<int>> manufacturerData) {
    final List<int>? h = manufacturerData[kMultisenseCompanyId];
    if (h == null || h.length < 6) return null;

    final int format = h[0] >> 4;
    final int role = h[0] & 0x0F;
    // Only format 1 is defined. A newer node must not be shown as if its
    // bytes meant what they mean today.
    if (format != 1) return null;

    final int caps = h[1] | (h[2] << 8);
    final int soc = h[3];

    if (h.length < 14) {
      return MultisenseReadings(
        formatVersion: format,
        role: role,
        caps: MsCaps(caps),
        seq: h[4],
        bundleTag: h[5],
        socPct: soc == 0xFF ? null : soc,
        fromAdvert: true,
      );
    }

    final int o = h.length - 8;	// readings tail; see the note above
    final int luxRaw = h[o] | (h[o + 1] << 8);
    final int cctRaw = h[o + 2] | (h[o + 3] << 8);
    final int yawRaw = h[o + 4] | (h[o + 5] << 8);

    return MultisenseReadings(
      formatVersion: format,
      role: role,
      caps: MsCaps(caps),
      seq: h[4],
      bundleTag: h[5],
      socPct: soc == 0xFF ? null : soc,
      lux: luxRaw == 0xFFFF ? null : luxRaw.toDouble(),
      cct: cctRaw == 0xFFFF ? null : cctRaw.toDouble(),
      yawDeg: yawRaw == 0x8000 ? null : _s16(yawRaw).toDouble(),
      stability: _stabilityFromByte(h[o + 6]),
      sigMotion: h[o + 7],
      fromAdvert: true,
    );
  }

  /// Parse an HCP `SENSE` reply, already split into key=value pairs by
  /// `HcpClient.parseKv`. The node emits a key only for a sensor it has, so a
  /// missing key means "not fitted", not "zero".
  static MultisenseReadings fromSense(Map<String, String> kv) {
    final String? tbat = kv['tbat'];
    return MultisenseReadings(
      seq: int.tryParse(kv['seq'] ?? ''),
      socPct: int.tryParse(kv['soc'] ?? ''),
      vbatVolts: double.tryParse(kv['vbat'] ?? ''),
      tbatCelsius: tbat == null || tbat == 'na' ? null : double.tryParse(tbat),
      lux: double.tryParse(kv['lux'] ?? ''),
      cct: double.tryParse(kv['cct'] ?? ''),
      cieX: double.tryParse(kv['x'] ?? ''),
      cieY: double.tryParse(kv['y'] ?? ''),
      yawDeg: double.tryParse(kv['yaw'] ?? ''),
      pitchDeg: double.tryParse(kv['pitch'] ?? ''),
      rollDeg: double.tryParse(kv['roll'] ?? ''),
      headingAccDeg: double.tryParse(kv['hacc'] ?? ''),
      stability: _stabilityFromWord(kv['motion']),
      sigMotion: int.tryParse(kv['sigmo'] ?? ''),
      timeIso: kv['time'],
    );
  }

  static int _s16(int v) => v >= 0x8000 ? v - 0x10000 : v;

  /// One-line summary for a list row: only the readings that exist, in the
  /// order someone scanning a list wants them.
  String get summaryLine => <String>[
        if (lux != null) '${_num(lux!, lux! < 10 ? 1 : 0)} lx',
        if (cct != null) '${cct!.round()} K',
        if (yawDeg != null) '${yawDeg!.round()}°',
        if (socPct != null) '$socPct%',
      ].join('  ·  ');

  static String _num(double v, int places) => v.toStringAsFixed(places);
}
