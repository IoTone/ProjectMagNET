// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:magnet_app/magnet/hcp.dart';

/// Scripted transport: replies to whatever the client sends, and can inject
/// unsolicited events at will — which is exactly the condition the sigil
/// framing exists to survive.
class FakeTransport implements HcpTransport {
  final StreamController<String> _in = StreamController<String>.broadcast();
  final List<String> sent = <String>[];
  String? Function(String tag, String verb)? responder;

  @override
  Stream<String> get lines => _in.stream;

  @override
  Future<void> send(String line) async {
    sent.add(line);
    final int sp = line.indexOf(' ');
    final String tag = line.substring(1, sp); // strip '@'
    final String verb = line.substring(sp + 1);
    final String? r = responder?.call(tag, verb);
    if (r != null) {
      scheduleMicrotask(() => _in.add(r));
    }
  }

  void inject(String line) => _in.add(line);
}

void main() {
  group('HCP framing', () {
    test('a command returns its own tagged response', () async {
      final FakeTransport t = FakeTransport()
        ..responder = (String tag, String verb) => '@$tag +OK state=READY';
      final HcpClient c = HcpClient(t);
      expect(await c.command('STATUS'), 'OK state=READY');
      await c.dispose();
    });

    test('events interleaved with a response do not satisfy the command',
        () async {
      final FakeTransport t = FakeTransport();
      t.responder = (String tag, String verb) {
        // an event arrives *before* the real reply
        scheduleMicrotask(() => t.inject('!CHAT magnet aabbccdd alice hi'));
        return '@$tag +OK state=READY';
      };
      final HcpClient c = HcpClient(t);
      final List<HcpEvent> seen = <HcpEvent>[];
      c.events.listen(seen.add);

      final String r = await c.command('STATUS');
      expect(r, 'OK state=READY', reason: 'must be the response, not the event');
      await Future<void>.delayed(Duration.zero);
      expect(seen.single.name, 'chat');
      expect(seen.single.text, 'hi');
      await c.dispose();
    });

    test('-ERR surfaces as HcpError with the stable code', () async {
      final FakeTransport t = FakeTransport()
        ..responder = (String tag, String verb) =>
            '@$tag -ERR E_RATE_LIMITED burst 8, 10/s';
      final HcpClient c = HcpClient(t);
      await expectLater(
        c.command('CHAT hi'),
        throwsA(isA<HcpError>()
            .having((HcpError e) => e.code, 'code', 'E_RATE_LIMITED')),
      );
      await c.dispose();
    });

    test('# comments are captured, not treated as responses', () async {
      final FakeTransport t = FakeTransport();
      t.responder = (String tag, String verb) {
        scheduleMicrotask(() {
          t.inject('# peer aabbccdd alice fdde::1');
          t.inject('@$tag +OK');
        });
        return null;
      };
      final HcpClient c = HcpClient(t);
      final List<Map<String, String>> peers = await c.peers();
      expect(peers.single['name'], 'alice');
      await c.dispose();
    });

    test('commands serialize so tags are never ambiguous', () async {
      final FakeTransport t = FakeTransport()
        ..responder = (String tag, String verb) => '@$tag +OK $verb';
      final HcpClient c = HcpClient(t);
      final List<String> rs = await Future.wait(<Future<String>>[
        c.command('A'),
        c.command('B'),
        c.command('C'),
      ]);
      expect(rs, <String>['OK A', 'OK B', 'OK C']);
      await c.dispose();
    });

    test('a silent node times out rather than hanging forever', () async {
      final FakeTransport t = FakeTransport(); // never responds
      final HcpClient c = HcpClient(t);
      await expectLater(
        c.command('STATUS', timeout: const Duration(milliseconds: 50)),
        throwsA(isA<HcpTimeout>()),
      );
      await c.dispose();
    });

    test('key=value parsing', () {
      final Map<String, String> kv =
          HcpClient.parseKv('OK state=READY role=leader peers=3');
      expect(kv['state'], 'READY');
      expect(kv['peers'], '3');
    });
  });

  group('event payloads', () {
    test('chat text starts after channel/id/name', () {
      final HcpEvent e = HcpEvent(
        'chat',
        <String>['magnet', 'aabbccdd', 'alice', 'hello', 'there'],
        '!CHAT magnet aabbccdd alice hello there',
      );
      expect(e.text, 'hello there');
    });

    test('dm text starts after id/name', () {
      final HcpEvent e = HcpEvent('dm', <String>['aabbccdd', 'alice', 'psst'],
          '!DM aabbccdd alice psst');
      expect(e.text, 'psst');
    });
  });
}
