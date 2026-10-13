// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// The operator's signing identity — this phone's claim to administer a fleet.
///
/// Nodes keep an allow-list of public keys (design proposal §11.1.7). A
/// privileged command — rotate the channel epoch, move the fleet — only
/// executes if it carries a signature from a key on that list; everything else
/// is dropped *before* it runs. That is what stops any channel member from
/// hijacking the mesh.
///
/// This class is the counterpart: generate a keypair once, keep the private
/// half in the platform keystore, hand the public half to every node we
/// provision. Afterwards this phone — and only this phone — can issue fleet
/// commands to those nodes.
///
/// **Curve: ECDSA P-256**, matching the firmware. The spec originally said
/// Ed25519, but ESP-IDF 5.3.1's mbedTLS has no EdDSA at all; P-256 is native
/// there and hardware-accelerated on the C6 (design proposal, rev 2.2).
///
/// Implemented on `pointycastle` rather than `cryptography`: the latter's
/// P-256 is a platform-binding shim whose pure-Dart path throws
/// `UnimplementedError`, so it cannot be unit-tested and would fail wherever
/// the native binding is absent.
library;

import 'dart:convert';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter/foundation.dart' show visibleForTesting;
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:pointycastle/export.dart';

class OperatorIdentity {
  OperatorIdentity._(this._private, this.publicKeyHex, this.fingerprint);

  static const String _kKey = 'magnet.operator.p256.d';
  static const FlutterSecureStorage _store = FlutterSecureStorage(
    iOptions: IOSOptions(accessibility: KeychainAccessibility.first_unlock),
  );

  final ECPrivateKey _private;

  /// Uncompressed SEC1 point, `04 ‖ X ‖ Y`, lowercase hex — exactly the 130
  /// characters the node's `ADMIN ADD` verb expects.
  final String publicKeyHex;

  /// Short human-checkable form (8 hex chars), for confirming that the key on
  /// a node is the key in this phone.
  final String fingerprint;

  static ECDomainParameters get _curve => ECCurve_secp256r1();

  /// Load the stored identity, creating one on first run.
  static Future<OperatorIdentity> loadOrCreate() async {
    final String? stored = await _store.read(key: _kKey);
    if (stored != null) {
      return _fromScalar(_bytesToBigInt(base64Decode(stored)));
    }
    final ECPrivateKey priv = generateKeyPair();
    await _store.write(
        key: _kKey, value: base64Encode(_bigIntToBytes(priv.d!, 32)));
    return _fromScalar(priv.d!);
  }

  /// Whether this device has ever created an identity.
  static Future<bool> exists() async => await _store.read(key: _kKey) != null;

  /// Destroy the identity. Nodes already enrolled keep the stale public key on
  /// their allow-list — they must be re-provisioned, or the key removed there.
  static Future<void> forget() => _store.delete(key: _kKey);

  /// Sign `message`, returning raw `r ‖ s` (64 bytes) — the form
  /// `mn_verify()` reads straight out of the envelope trailer.
  Uint8List sign(List<int> message) {
    final ECDSASigner signer =
        ECDSASigner(SHA256Digest(), HMac(SHA256Digest(), 64))
          ..init(
            true,
            ParametersWithRandom(
              PrivateKeyParameter<ECPrivateKey>(_private),
              _seededRandom(),
            ),
          );
    final ECSignature sig =
        signer.generateSignature(Uint8List.fromList(message)) as ECSignature;
    return Uint8List.fromList(<int>[
      ..._bigIntToBytes(sig.r, 32),
      ..._bigIntToBytes(sig.s, 32),
    ]);
  }

  /// The node-side command that enrolls this operator.
  String get adminAddCommand => 'ADMIN ADD $publicKeyHex';

  // ---- construction helpers ----

  static ECPrivateKey generateKeyPair() {
    final ECKeyGenerator gen = ECKeyGenerator()
      ..init(ParametersWithRandom(
          ECKeyGeneratorParameters(_curve), _seededRandom()));
    return gen.generateKeyPair().privateKey;
  }

  /// Build from a private scalar, bypassing the keystore — tests, and the
  /// restore path.
  @visibleForTesting
  static OperatorIdentity fromScalar(BigInt d) => _fromScalar(d);

  static OperatorIdentity _fromScalar(BigInt d) {
    final ECPoint q = (_curve.G * d)!;
    final ECPrivateKey priv = ECPrivateKey(d, _curve);
    // getEncoded(false) is SEC1 uncompressed: 0x04 ‖ X(32) ‖ Y(32).
    final String hex = _hex(q.getEncoded(false));
    return OperatorIdentity._(priv, hex, hex.substring(2, 10));
  }

  static SecureRandom _seededRandom() {
    final FortunaRandom r = FortunaRandom();
    final Random seed = Random.secure();
    r.seed(KeyParameter(Uint8List.fromList(
        List<int>.generate(32, (_) => seed.nextInt(256)))));
    return r;
  }

  static String _hex(List<int> b) =>
      b.map((int x) => x.toRadixString(16).padLeft(2, '0')).join();

  /// Fixed-width big-endian encoding. A scalar or coordinate with leading zero
  /// bytes must still occupy its full width, or the node reads a short key.
  static Uint8List _bigIntToBytes(BigInt v, int width) {
    final Uint8List out = Uint8List(width);
    BigInt x = v;
    for (int i = width - 1; i >= 0; i--) {
      out[i] = (x & BigInt.from(0xff)).toInt();
      x = x >> 8;
    }
    return out;
  }

  static BigInt _bytesToBigInt(List<int> b) {
    BigInt v = BigInt.zero;
    for (final int x in b) {
      v = (v << 8) | BigInt.from(x);
    }
    return v;
  }
}
