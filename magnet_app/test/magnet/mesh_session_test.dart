// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:magnet_app/magnet/hcp.dart';
import 'package:magnet_app/magnet/mesh_session.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// Scripted node: answers commands like the 0.5.0-ee firmware, and lets a
/// test inject unsolicited `!` events at any time.
class FakeNodeTransport implements HcpTransport {
  final StreamController<String> _lines = StreamController<String>.broadcast();
  final List<String> sent = <String>[];

  @override
  Stream<String> get lines => _lines.stream;

  void emit(String line) => _lines.add(line);

  @override
  Future<void> send(String line) async {
    sent.add(line);
    // "@aNNNN VERB rest…"
    final int sp = line.indexOf(' ');
    final String tag = line.substring(0, sp); // includes '@'
    final String body = line.substring(sp + 1);
    final String verb = body.split(' ').first;
    switch (verb) {
      case 'SUB':
        emit('$tag +OK');
        break;
      case 'STATUS':
        emit('$tag +OK state=READY role=router channel=magnet peers=2 '
            'name=probe id=e1256131 ble=up');
        break;
      case 'CHAT':
        emit('$tag +OK');
        break;
      case 'PEERS':
        emit('# peer 2ca44570 xray1 fdde::1 last-seen=12s ago');
        emit('# peer 46a359bf sdk-b fdde::2 last-seen=61s ago');
        emit('$tag +OK');
        break;
      case 'MESH':
        emit('# mesh role=router partition=0x1234abcd rloc16=0x5400 chan=24 pan=0x4d4e');
        emit('# mesh ml-eid=fd4d:4e00::abcd mcast=ff05::e139:9682');
        emit('# mesh leader-rloc=0x8c00 weight=64');
        emit('# mesh neighbor rloc16=0x8c00 rssi=-61 lqi=3 age=7s router');
        emit('# mesh neighbor rloc16=0x5401 rssi=-70 lqi=2 age=3s child');
        emit('$tag +OK');
        break;
      default:
        emit('$tag -ERR E_UNKNOWN_VERB $verb');
    }
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  late FakeNodeTransport transport;
  late MeshSession session;

  setUp(() {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    transport = FakeNodeTransport();
    session = MeshSession(linkFactory: (String id) async {
      return MeshLink(
        client: HcpClient(transport),
        dispose: () async {},
      );
    });
  });

  test('adopt connects, subscribes, and snapshots status + peers + topology',
      () async {
    await session.adopt('AA:BB:CC:DD:EE:FF', name: 'probe');
    // Let the fire-and-forget PEERS/MESH snapshots settle.
    await Future<void>.delayed(const Duration(milliseconds: 50));

    expect(session.state, MeshLinkState.connected);
    expect(transport.sent.any((String s) => s.contains('SUB all')), isTrue);
    expect(session.nodeStatus['ble'], 'up');
    expect(session.companionName, 'probe');

    expect(session.peers, hasLength(2));
    expect(session.peers.first.displayName, 'xray1');
    expect(session.peers.first.lastSeen, const Duration(seconds: 12));

    expect(session.topology.role, 'router');
    expect(session.topology.rloc16, '0x5400');
    expect(session.topology.neighbors, hasLength(2));
    expect(session.topology.neighbors.last.isChild, isTrue);
    expect(session.topology.neighbors.first.rssi, -61);

    // Remembered on disk.
    final SharedPreferences p = await SharedPreferences.getInstance();
    expect(p.getString('mesh.companion.id'), 'AA:BB:CC:DD:EE:FF');
  });

  test('mesh events land in the feed and touch the peer table', () async {
    await session.adopt('X', name: 'probe');
    await Future<void>.delayed(const Duration(milliseconds: 50));
    final int base = session.feed.length;

    transport.emit('!CHAT magnet 2ca44570 xray1 hello from the bench');
    transport.emit('!PEER_JOIN aabbccdd - fdde::9');
    transport.emit('!ROLE leader');
    transport.emit('!WARN default-channel-insecure');
    await Future<void>.delayed(const Duration(milliseconds: 20));

    final List<MeshFeedItem> added = session.feed.sublist(base);
    expect(added.map((MeshFeedItem i) => i.kind).toList(),
        <String>['chat', 'peer', 'role', 'warn']);
    expect(added[0].text, 'hello from the bench');
    expect(added[0].from, 'xray1');
    expect(session.nodeStatus['role'], 'leader');

    // The unnamed joiner appears in the table under its hex id.
    expect(session.peers.any((MeshPeer p) => p.id == 'aabbccdd'), isTrue);
    expect(
        session.peers
            .firstWhere((MeshPeer p) => p.id == 'aabbccdd')
            .displayName,
        'aabbccdd');
  });

  test('heartbeat updates status without spamming the feed', () async {
    await session.adopt('X');
    await Future<void>.delayed(const Duration(milliseconds: 50));
    final int base = session.feed.length;

    transport.emit('!HEARTBEAT READY 30 router 0');
    await Future<void>.delayed(const Duration(milliseconds: 20));

    expect(session.feed.length, base);
    expect(session.lastHeartbeat, isNotNull);
    expect(session.nodeStatus['state'], 'READY');
  });

  test('sendChat appends a local sent item (node never echoes us)', () async {
    await session.adopt('X');
    await Future<void>.delayed(const Duration(milliseconds: 50));

    await session.sendChat('  anyone home?  ');
    expect(transport.sent.any((String s) => s.endsWith('CHAT anyone home?')),
        isTrue);
    expect(session.feed.last.kind, 'sent');
    expect(session.feed.last.text, 'anyone home?');
  });

  test('forget clears the remembered companion', () async {
    await session.adopt('X', name: 'probe');
    await session.forget();
    expect(session.hasCompanion, isFalse);
    final SharedPreferences p = await SharedPreferences.getInstance();
    expect(p.getString('mesh.companion.id'), isNull);
  });
}
