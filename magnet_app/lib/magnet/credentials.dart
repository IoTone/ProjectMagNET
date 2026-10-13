// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// M1 credential helpers (SCOPE.md M1, design proposal §11.4): generate
/// high-entropy channel credentials in-app so a typed passphrase is the
/// fallback, not the path.
///
/// Two generated forms, matching the firmware's `mn_cred_derive` detection:
///  - Path A: `qr:` + base64(32 random bytes) — full 256-bit secret.
///  - Path C: >=12 space-separated words → the node HKDFs the RAW phrase
///    (no wordlist is enforced on-node; ours is the EFF short list purely
///    for generation quality). 13 words × ~10.34 bits ≈ 134 bits.
///
/// Anything else the user types is Path B (PBKDF2-stretched passphrase) —
/// allowed, but the UI warns because typed entropy is human entropy.
library;
import 'dart:convert';
import 'dart:math';

import 'package:shared_preferences/shared_preferences.dart';

import 'wordlist.dart';

/// Which derivation path the firmware will pick for a credential, plus a
/// rough entropy estimate for the UI.
enum CredPath { qrSecret, seedPhrase, passphrase }

class CredInfo {
  const CredInfo(this.path, this.bits);
  final CredPath path;

  /// Approximate entropy in bits — exact for generated credentials, a
  /// pessimistic guess (charset-based) for typed ones.
  final int bits;

  String get pathLetter => switch (path) {
        CredPath.qrSecret => 'A',
        CredPath.seedPhrase => 'C',
        CredPath.passphrase => 'B',
      };
}

class MagnetCredentials {
  static final Random _rng = Random.secure();

  /// ~10.34 bits per EFF-short-list word; 13 words ≈ 134 bits ≥ the 128-bit
  /// target. The firmware requires >=12 words for Path C.
  static const int defaultWords = 13;

  /// Generate a Path C seed phrase: space-separated, lowercase, memorable.
  static String generateSeedPhrase({int words = defaultWords}) {
    assert(words >= 12 && words <= 24);
    return List<String>.generate(
        words, (_) => effShortWordlist[_rng.nextInt(effShortWordlist.length)])
        .join(' ');
  }

  /// Generate a Path A secret: `qr:` + base64 of 32 random bytes.
  static String generateQrSecret() {
    final List<int> raw =
        List<int>.generate(32, (_) => _rng.nextInt(256));
    return 'qr:${base64Encode(raw)}';
  }

  /// Mirror the firmware's path detection (`mn_cred_derive`).
  static CredInfo describe(String cred) {
    final String c = cred.trim();
    if (c.startsWith('qr:')) return const CredInfo(CredPath.qrSecret, 256);
    final List<String> words =
        c.split(RegExp(r'\s+')).where((String w) => w.isNotEmpty).toList();
    if (words.length >= 12) {
      // Generated phrases get the wordlist rate; a typed 12-word phrase of
      // unknown origin gets the same optimistic figure — the path is what
      // matters (HKDF, no stretch), the UI labels it "if randomly chosen".
      return CredInfo(CredPath.seedPhrase,
          (words.length * 10.34).floor());
    }
    // Typed passphrase: pessimistic ~2.5 bits/char (human-chosen text).
    return CredInfo(CredPath.passphrase, (c.length * 2.5).floor());
  }
}

/// One provisioning event — enough to know the fleet after nodes go dark
/// (SCOPE M1: "Remember what we provisioned").
class ProvisionRecord {
  ProvisionRecord({
    required this.deviceId,
    required this.nodeId,
    required this.name,
    required this.channelLabel,
    required this.selector,
    required this.path,
    required this.at,
  });

  /// Platform BLE device id the node was provisioned through.
  final String deviceId;

  /// The node's MagNET device id (hex) at provisioning time. A factory
  /// reset mints a new one, so this dates the record rather than tracking
  /// the node forever.
  final String nodeId;
  final String name;

  /// Display label for the channel — never the credential itself. The
  /// credential is a KEY; it does not belong in an app-readable log.
  final String channelLabel;
  final String selector;
  final String path;
  final DateTime at;

  Map<String, Object> toJson() => <String, Object>{
        'deviceId': deviceId,
        'nodeId': nodeId,
        'name': name,
        'channelLabel': channelLabel,
        'selector': selector,
        'path': path,
        'at': at.toIso8601String(),
      };

  static ProvisionRecord fromJson(Map<String, dynamic> j) => ProvisionRecord(
        deviceId: j['deviceId'] as String? ?? '',
        nodeId: j['nodeId'] as String? ?? '',
        name: j['name'] as String? ?? '',
        channelLabel: j['channelLabel'] as String? ?? '',
        selector: j['selector'] as String? ?? '',
        path: j['path'] as String? ?? '',
        at: DateTime.tryParse(j['at'] as String? ?? '') ?? DateTime.now(),
      );
}

/// Append-only local log of provisioning events, newest first.
class ProvisionLog {
  static const String _key = 'provision.log';
  static const int _max = 100;

  static Future<List<ProvisionRecord>> load() async {
    final SharedPreferences p = await SharedPreferences.getInstance();
    final String? raw = p.getString(_key);
    if (raw == null) return <ProvisionRecord>[];
    try {
      return (jsonDecode(raw) as List<dynamic>)
          .map((dynamic e) =>
              ProvisionRecord.fromJson(e as Map<String, dynamic>))
          .toList();
    } catch (_) {
      return <ProvisionRecord>[];
    }
  }

  static Future<void> add(ProvisionRecord r) async {
    final List<ProvisionRecord> all = await load();
    all.insert(0, r);
    if (all.length > _max) all.removeRange(_max, all.length);
    final SharedPreferences p = await SharedPreferences.getInstance();
    await p.setString(
        _key, jsonEncode(all.map((ProvisionRecord e) => e.toJson()).toList()));
  }
}
