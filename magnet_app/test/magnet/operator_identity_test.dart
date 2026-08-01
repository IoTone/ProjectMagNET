// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter_test/flutter_test.dart';
import 'package:magnet_app/magnet/operator_identity.dart';
import 'package:pointycastle/export.dart';

void main() {
  group('operator identity', () {
    test('public key is SEC1 uncompressed — exactly what ADMIN ADD parses',
        () {
      final ECPrivateKey k = OperatorIdentity.generateKeyPair();
      final OperatorIdentity id = OperatorIdentity.fromScalar(k.d!);

      // The firmware reads 130 hex chars into a 65-byte point and rejects
      // anything else outright, so this length is load-bearing.
      expect(id.publicKeyHex.length, 130);
      expect(id.publicKeyHex.startsWith('04'), isTrue);
      expect(RegExp(r'^[0-9a-f]+$').hasMatch(id.publicKeyHex), isTrue);
      expect(id.adminAddCommand, 'ADMIN ADD ${id.publicKeyHex}');
    });

    test('encoding is fixed-width even when a coordinate has leading zeros',
        () {
      for (int i = 0; i < 30; i++) {
        final OperatorIdentity id =
            OperatorIdentity.fromScalar(OperatorIdentity.generateKeyPair().d!);
        expect(id.publicKeyHex.length, 130,
            reason: 'key $i produced a short encoding');
      }
    });

    test('signature is raw r-concat-s, 64 bytes — the envelope trailer form',
        () {
      final OperatorIdentity id =
          OperatorIdentity.fromScalar(OperatorIdentity.generateKeyPair().d!);
      for (int i = 0; i < 8; i++) {
        expect(id.sign(<int>[1, 2, 3, i]).length, 64);
      }
    });

    test('the same scalar always yields the same key and fingerprint', () {
      final ECPrivateKey k = OperatorIdentity.generateKeyPair();
      final OperatorIdentity a = OperatorIdentity.fromScalar(k.d!);
      final OperatorIdentity b = OperatorIdentity.fromScalar(k.d!);
      expect(a.publicKeyHex, b.publicKeyHex);
      expect(a.fingerprint, b.fingerprint);
      expect(a.fingerprint.length, 8);
      expect(a.fingerprint, a.publicKeyHex.substring(2, 10));
    });

    test('distinct keys do not collide', () {
      final Set<String> seen = <String>{};
      for (int i = 0; i < 10; i++) {
        seen.add(OperatorIdentity.fromScalar(
                OperatorIdentity.generateKeyPair().d!)
            .publicKeyHex);
      }
      expect(seen.length, 10);
    });
  });
}
