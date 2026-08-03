// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:magnet_app/magnet/credentials.dart';
import 'package:magnet_app/magnet/wordlist.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('generation', () {
    test('seed phrase is >=12 words from the wordlist, space-separated', () {
      final String phrase = MagnetCredentials.generateSeedPhrase();
      final List<String> words = phrase.split(' ');
      expect(words.length, MagnetCredentials.defaultWords);
      expect(words.length, greaterThanOrEqualTo(12)); // firmware Path C gate
      for (final String w in words) {
        expect(effShortWordlist.contains(w), isTrue, reason: w);
      }
      // The firmware HKDFs the raw string — no double spaces, no padding.
      expect(phrase.contains('  '), isFalse);
      expect(phrase.trim(), phrase);
    });

    test('two phrases are different (sanity, not proof)', () {
      expect(MagnetCredentials.generateSeedPhrase(),
          isNot(MagnetCredentials.generateSeedPhrase()));
    });

    test('qr secret is qr: + base64 of exactly 32 bytes', () {
      final String s = MagnetCredentials.generateQrSecret();
      expect(s.startsWith('qr:'), isTrue);
      expect(base64Decode(s.substring(3)), hasLength(32));
    });
  });

  group('path detection mirrors mn_cred_derive', () {
    test('qr: prefix is Path A', () {
      final CredInfo i =
          MagnetCredentials.describe(MagnetCredentials.generateQrSecret());
      expect(i.path, CredPath.qrSecret);
      expect(i.pathLetter, 'A');
      expect(i.bits, 256);
    });

    test('12+ words is Path C', () {
      final CredInfo i = MagnetCredentials.describe(
          MagnetCredentials.generateSeedPhrase());
      expect(i.path, CredPath.seedPhrase);
      expect(i.pathLetter, 'C');
      expect(i.bits, greaterThanOrEqualTo(128));
    });

    test('11 words is still a passphrase (Path B)', () {
      final String eleven =
          List<String>.filled(11, 'word').join(' ');
      expect(MagnetCredentials.describe(eleven).path, CredPath.passphrase);
    });

    test('short typed text is Path B with a pessimistic estimate', () {
      final CredInfo i =
          MagnetCredentials.describe('correct horse battery staple');
      expect(i.path, CredPath.passphrase);
      expect(i.bits, lessThan(128));
    });
  });

  group('provision log', () {
    setUp(() =>
        SharedPreferences.setMockInitialValues(<String, Object>{}));

    test('add + load round-trips, newest first', () async {
      await ProvisionLog.add(ProvisionRecord(
          deviceId: 'AA', nodeId: '11111111', name: 'first',
          channelLabel: 'qr-channel', selector: 'b3be', path: 'A',
          at: DateTime(2026, 8, 1)));
      await ProvisionLog.add(ProvisionRecord(
          deviceId: 'BB', nodeId: '22222222', name: 'second',
          channelLabel: 'acorn', selector: '7770', path: 'C',
          at: DateTime(2026, 8, 2)));

      final List<ProvisionRecord> all = await ProvisionLog.load();
      expect(all, hasLength(2));
      expect(all.first.name, 'second');
      expect(all.last.selector, 'b3be');
      expect(all.first.at.day, 2);
    });
  });
}
