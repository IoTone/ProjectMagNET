// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:math' as math;

/// Coarse signal-strength → distance / radial-position helpers shared
/// by the radar view. RSSI-based distance is only ever a rough
/// estimate (multipath, antenna gain, and TX power all confound it),
/// so this is presented as an indicator, never a measurement.
class SignalDistance {
  SignalDistance._();

  /// Reference RSSI at 1 m for a typical phone-class radio (dBm).
  static const double refRssiAt1m = -59;

  /// Log-distance path-loss exponent. ~2.0 free space, 2.5–4 indoors.
  static const double pathLossExponent = 2.5;

  /// Rough distance estimate in metres from an RSSI sample. [refRssi]
  /// defaults to a phone-class 1 m reference; pass a device's
  /// advertised TX-power-at-1 m when available for a better guess.
  static double approxMeters(int rssi, {double? refRssi}) {
    final double ref = refRssi ?? refRssiAt1m;
    final double exp = (ref - rssi) / (10.0 * pathLossExponent);
    return math.pow(10, exp).toDouble();
  }

  /// Normalised radial fraction in [0, 1] for placing a blip on the
  /// radar: 0 = centre (strongest), 1 = rim (weakest). Maps a usable
  /// RSSI window (~-40 strong … -100 weak) onto the ring radius.
  static double radialFraction(int rssi, {int strong = -40, int weak = -100}) {
    if (rssi >= strong) return 0.0;
    if (rssi <= weak) return 1.0;
    return (strong - rssi) / (strong - weak);
  }

  /// Three-band classification for badges / colour.
  static SignalBand band(int rssi) {
    if (rssi >= -65) return SignalBand.strong;
    if (rssi >= -85) return SignalBand.medium;
    return SignalBand.weak;
  }
}

enum SignalBand { strong, medium, weak }
