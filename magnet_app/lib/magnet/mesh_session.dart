// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// M3 — the live window into the mesh (SCOPE.md §M3).
///
/// One node on the bench runs the `esp32c6_ble_resident` companion build and
/// keeps BLE up after provisioning; this session owns the app's persistent
/// connection to it. Everything the mesh carries — chat, peers joining,
/// role changes, warnings — arrives here as `!` events and becomes a feed
/// the UI can render live.
///
/// Companion selection is remembered by **platform device id** (the BLE
/// address/identifier), never by advertised name: the `MagNET-XXXX` name
/// snapshots the device id at BLE start and macOS/iOS cache it per
/// peripheral, so the name can lie (see firmware-idf/README.md).
library;
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'hcp.dart';
import 'magnet_ble.dart';

/// Connection lifecycle of the companion link.
enum MeshLinkState { idle, connecting, connected, error }

/// One rendered line of the live feed.
class MeshFeedItem {
  MeshFeedItem(this.kind, this.text,
      {this.from, DateTime? at, this.raw = '', this.backfill = false})
      : at = at ?? DateTime.now();

  /// `chat` | `dm` | `sent` | `peer` | `role` | `state` | `warn` | `info`
  final String kind;
  final String text;

  /// Display name (or hex id) of the sender, for chat/dm items.
  final String? from;
  final DateTime at;
  final String raw;

  /// True for chat replayed from the companion's ring (`!RCHAT`) — it
  /// happened while this phone was away, so the timestamp is arrival, not
  /// origin.
  final bool backfill;
}

class MeshPeer {
  MeshPeer({required this.id, required this.name, required this.ipv6,
      this.lastSeen});
  final String id;
  final String name;
  final String ipv6;
  final Duration? lastSeen;

  /// The node prints `-` for a peer that has not announced a name yet.
  String get displayName => (name.isEmpty || name == '-') ? id : name;
}

class MeshNeighbor {
  MeshNeighbor({required this.rloc16, required this.rssi, required this.lqi,
      required this.age, required this.isChild});
  final String rloc16;
  final int rssi;
  final int lqi;
  final Duration age;
  final bool isChild;
}

/// Parsed `MESH` output (`# mesh …` comment lines).
class MeshTopology {
  MeshTopology({this.role = '', this.partition = '', this.rloc16 = '',
      this.channel = '', this.pan = '', this.mlEid = '', this.mcast = '',
      this.leaderRloc = '', this.neighbors = const <MeshNeighbor>[]});

  final String role;
  final String partition;
  final String rloc16;
  final String channel;
  final String pan;
  final String mlEid;
  final String mcast;
  final String leaderRloc;
  final List<MeshNeighbor> neighbors;

  static MeshTopology parse(List<String> comments) {
    String role = '', partition = '', rloc16 = '', channel = '', pan = '';
    String mlEid = '', mcast = '', leaderRloc = '';
    final List<MeshNeighbor> nbs = <MeshNeighbor>[];
    for (final String c in comments) {
      if (!c.startsWith('mesh')) continue;
      final Map<String, String> kv = HcpClient.parseKv(c);
      if (c.startsWith('mesh neighbor ')) {
        nbs.add(MeshNeighbor(
          rloc16: kv['rloc16'] ?? '?',
          rssi: int.tryParse(kv['rssi'] ?? '') ?? 0,
          lqi: int.tryParse(kv['lqi'] ?? '') ?? 0,
          age: Duration(
              seconds: int.tryParse(
                      (kv['age'] ?? '0s').replaceAll(RegExp(r'[^0-9]'), '')) ??
                  0),
          isChild: c.trimRight().endsWith('child'),
        ));
        continue;
      }
      role = kv['role'] ?? role;
      partition = kv['partition'] ?? partition;
      rloc16 = kv['rloc16'] ?? rloc16;
      channel = kv['chan'] ?? channel;
      pan = kv['pan'] ?? pan;
      mlEid = kv['ml-eid'] ?? mlEid;
      mcast = kv['mcast'] ?? mcast;
      leaderRloc = kv['leader-rloc'] ?? leaderRloc;
    }
    return MeshTopology(role: role, partition: partition, rloc16: rloc16,
        channel: channel, pan: pan, mlEid: mlEid, mcast: mcast,
        leaderRloc: leaderRloc, neighbors: nbs);
  }
}

/// What [MeshSession] needs from a connected companion: an HCP client, an
/// optional link-liveness stream, and a teardown. The BLE factory builds one
/// from a device id; tests inject fakes.
class MeshLink {
  MeshLink({required this.client, this.connected, required this.dispose});
  final HcpClient client;
  final Stream<bool>? connected;
  final Future<void> Function() dispose;
}

typedef MeshLinkFactory = Future<MeshLink> Function(String deviceId);

Future<MeshLink> _bleLinkFactory(String deviceId) async {
  final BluetoothDevice dev = BluetoothDevice.fromId(deviceId);
  final (MagnetBleTransport t, HcpClient client) = await connectToNode(dev);
  return MeshLink(
    client: client,
    connected: dev.connectionState
        .map((BluetoothConnectionState s) =>
            s == BluetoothConnectionState.connected),
    dispose: () async {
      await client.dispose();
      await t.dispose();
    },
  );
}

/// Owns the companion connection and everything M3 renders: the live event
/// feed, the peer table, node status, and mesh topology.
class MeshSession extends ChangeNotifier {
  MeshSession({MeshLinkFactory? linkFactory})
      : _linkFactory = linkFactory ?? _bleLinkFactory;

  static const String _prefId = 'mesh.companion.id';
  static const String _prefName = 'mesh.companion.name';
  static const int _feedMax = 500;

  final MeshLinkFactory _linkFactory;
  MeshLink? _link;
  StreamSubscription<HcpEvent>? _evtSub;
  StreamSubscription<bool>? _connSub;

  MeshLinkState _state = MeshLinkState.idle;
  String? _error;
  String? _companionId;
  String? _companionName;
  final List<MeshFeedItem> _feed = <MeshFeedItem>[];
  List<MeshPeer> _peers = <MeshPeer>[];
  Map<String, String> _status = <String, String>{};
  MeshTopology _topology = MeshTopology();
  DateTime? _lastHeartbeat;

  MeshLinkState get state => _state;
  String? get error => _error;
  String? get companionId => _companionId;
  String? get companionName => _companionName;
  bool get hasCompanion => _companionId != null;
  List<MeshFeedItem> get feed => List<MeshFeedItem>.unmodifiable(_feed);
  List<MeshPeer> get peers => List<MeshPeer>.unmodifiable(_peers);
  Map<String, String> get nodeStatus => Map<String, String>.unmodifiable(_status);
  MeshTopology get topology => _topology;
  DateTime? get lastHeartbeat => _lastHeartbeat;

  /// Restore the remembered companion (id + display name) from disk.
  Future<void> load() async {
    final SharedPreferences p = await SharedPreferences.getInstance();
    _companionId = p.getString(_prefId);
    _companionName = p.getString(_prefName);
    notifyListeners();
  }

  /// Pick (and remember) a companion node, then connect to it.
  Future<void> adopt(String deviceId, {String? name}) async {
    _companionId = deviceId;
    _companionName = name;
    final SharedPreferences p = await SharedPreferences.getInstance();
    await p.setString(_prefId, deviceId);
    if (name != null) {
      await p.setString(_prefName, name);
    } else {
      await p.remove(_prefName);
    }
    await connect();
  }

  /// Forget the remembered companion and drop any live link.
  Future<void> forget() async {
    await disconnect();
    _companionId = null;
    _companionName = null;
    final SharedPreferences p = await SharedPreferences.getInstance();
    await p.remove(_prefId);
    await p.remove(_prefName);
    notifyListeners();
  }

  /// Connect to the remembered companion and start the live feed.
  Future<void> connect() async {
    final String? id = _companionId;
    if (id == null || _state == MeshLinkState.connecting) return;
    await _teardown();
    _state = MeshLinkState.connecting;
    _error = null;
    notifyListeners();

    try {
      final MeshLink link = await _linkFactory(id);
      _link = link;
      _evtSub = link.client.events.listen(_onEvent);
      _connSub = link.connected?.listen((bool up) {
        if (!up && _state == MeshLinkState.connected) {
          _addFeed(MeshFeedItem('info', 'link lost — reconnect to resume'));
          _state = MeshLinkState.error;
          _error = 'link lost';
          notifyListeners();
        }
      });

      // Everything the node can tell us, from the start: subscribe first so
      // no event slips between the initial snapshot and the live stream.
      await link.client.subscribe(<String>['all']);
      _status = await link.client.status();
      _companionName = _status['name'] ?? _companionName;
      _state = MeshLinkState.connected;
      _addFeed(MeshFeedItem(
          'info', 'connected to ${_companionName ?? id} — live'));
      notifyListeners();

      // The phone is the only participant with a real clock, so hand it over
      // before anything else uses it. Cheap, idempotent, every connect.
      unawaited(_seedClock(link));

      // Backfill what the channel said while we were away: the companion
      // replays its recent ring as !RCHAT events (fw ≥ 0.6.0-eg).
      unawaited(_backfill(link));

      // Best-effort snapshots; the feed is already live if these are slow.
      unawaited(refreshPeers());
      unawaited(refreshTopology());
    } catch (e) {
      _state = MeshLinkState.error;
      _error = '$e';
      await _teardown();
      notifyListeners();
    }
  }

  Future<void> disconnect() async {
    await _teardown();
    _state = MeshLinkState.idle;
    notifyListeners();
  }

  /// Seed the companion's clock from the phone.
  ///
  /// A Thread-only mesh has no border router, so it has no NTP and no clock of
  /// its own — the phone is the only participant that knows the real time. The
  /// companion takes this as stratum 0 and multicasts it to the whole channel,
  /// so one command anchors every node (see the reference design's
  /// `docs/MESH-TIME.md`).
  ///
  /// Doing it on EVERY connect is the point, not laziness. Node clocks live in
  /// RAM, so a node that reboots comes back with none and adopts from whichever
  /// neighbour answers — one hop further from a real source each time. Left
  /// alone that ratchets up until the mesh refuses to distribute time at all.
  /// A re-seed collapses the whole mesh back to stratum 0/1 in well under a
  /// second, and the phone reconnects far more often than nodes reboot, so
  /// this keeps the mesh permanently anchored for the cost of one line.
  Future<void> _seedClock(MeshLink link) async {
    try {
      final DateTime now = DateTime.now();
      final int epoch = now.millisecondsSinceEpoch ~/ 1000;
      // Minutes east of UTC — JST 540, PDT -420. The firmware keeps only the
      // time of day, so this is what makes it read as local rather than UTC.
      final int tzMinutes = now.timeZoneOffset.inMinutes;
      await link.client.command('TIME SET $epoch $tzMinutes');
    } catch (_) {
      // Firmware without the TIME verb answers E_UNKNOWN_VERB, and a link that
      // dropped mid-handshake throws. Neither is worth surfacing: the mesh just
      // carries on reporting uptime instead of a wall clock.
    }
  }

  /// Ask the companion to replay its ring (`RECENT`, no argument). The
  /// !RCHAT events flow through [_onEvent] like live traffic; older firmware
  /// answers E_UNKNOWN_VERB and the feed simply stays live-only.
  Future<void> _backfill(MeshLink link) async {
    try {
      await link.client.command('RECENT');
    } catch (_) {}
  }

  Future<void> _teardown() async {
    await _evtSub?.cancel();
    _evtSub = null;
    await _connSub?.cancel();
    _connSub = null;
    final MeshLink? l = _link;
    _link = null;
    if (l != null) {
      try {
        await l.dispose();
      } catch (_) {}
    }
  }

  void _onEvent(HcpEvent e) {
    switch (e.name) {
      case 'chat':
        // !CHAT <channel> <idhex> <name> <text…>
        _addFeed(MeshFeedItem('chat', e.text,
            from: e.fields.length > 2 ? e.fields[2] : e.fields.elementAtOrNull(1),
            raw: e.raw));
        _touchPeer(e.fields.elementAtOrNull(1), e.fields.elementAtOrNull(2));
        break;
      case 'rchat':
        // !RCHAT <channel> <idhex> <name> <text…> — ring replay. Skip
        // anything the feed already shows (live catch, local echo, or an
        // earlier backfill of the same reconnect session).
        final String rtext = e.text;
        final bool seen = _feed.any((MeshFeedItem f) =>
            f.text == rtext &&
            (f.kind == 'chat' || f.kind == 'sent'));
        if (seen) break;
        _addFeed(MeshFeedItem('chat', rtext,
            from: e.fields.length > 2 ? e.fields[2] : e.fields.elementAtOrNull(1),
            raw: e.raw,
            backfill: true));
        break;
      case 'dm':
        // !DM <idhex> <name> <text…>
        _addFeed(MeshFeedItem('dm', e.text,
            from: e.fields.length > 1 ? e.fields[1] : e.fields.elementAtOrNull(0),
            raw: e.raw));
        _touchPeer(e.fields.elementAtOrNull(0), e.fields.elementAtOrNull(1));
        break;
      case 'peer_join':
        // !PEER_JOIN <idhex> <name|-> <ipv6>
        final String id = e.fields.elementAtOrNull(0) ?? '?';
        final String name = e.fields.elementAtOrNull(1) ?? '-';
        _addFeed(MeshFeedItem('peer',
            '${name == '-' ? id : name} joined', raw: e.raw));
        _touchPeer(id, name, ipv6: e.fields.elementAtOrNull(2));
        break;
      case 'peer_leave':
        _addFeed(MeshFeedItem('peer',
            '${e.fields.elementAtOrNull(1) ?? e.fields.elementAtOrNull(0) ?? '?'} left',
            raw: e.raw));
        break;
      case 'role':
        _status['role'] = e.fields.elementAtOrNull(0) ?? '';
        _addFeed(MeshFeedItem('role', 'node role: ${_status['role']}', raw: e.raw));
        break;
      case 'state':
        _status['state'] = e.fields.elementAtOrNull(0) ?? '';
        _addFeed(MeshFeedItem('state', 'node state: ${_status['state']}', raw: e.raw));
        break;
      case 'warn':
        _addFeed(MeshFeedItem('warn', e.text, raw: e.raw));
        break;
      case 'heartbeat':
        // !HEARTBEAT <state> <uptime> <role> <peers> — status, not feed noise.
        _lastHeartbeat = DateTime.now();
        final String? st = e.fields.elementAtOrNull(0);
        final String? role = e.fields.elementAtOrNull(2);
        if (st != null && st.isNotEmpty) _status['state'] = st;
        if (role != null && role.isNotEmpty) _status['role'] = role;
        notifyListeners();
        return; // notify already done, skip _addFeed
      default:
        _addFeed(MeshFeedItem('info', e.raw, raw: e.raw));
    }
  }

  /// A peer we heard from is alive right now — reflect that without waiting
  /// for the next PEERS poll.
  void _touchPeer(String? id, String? name, {String? ipv6}) {
    if (id == null) return;
    final int i = _peers.indexWhere((MeshPeer p) => p.id == id);
    final MeshPeer updated = MeshPeer(
      id: id,
      name: (name == null || name == '-')
          ? (i >= 0 ? _peers[i].name : '-')
          : name,
      ipv6: ipv6 ?? (i >= 0 ? _peers[i].ipv6 : ''),
      lastSeen: Duration.zero,
    );
    if (i >= 0) {
      _peers[i] = updated;
    } else {
      _peers = <MeshPeer>[..._peers, updated];
    }
  }

  void _addFeed(MeshFeedItem item) {
    _feed.add(item);
    if (_feed.length > _feedMax) {
      _feed.removeRange(0, _feed.length - _feedMax);
    }
    notifyListeners();
  }

  /// Send chat to the whole channel. The node does not echo our own multicast
  /// back, so append a local `sent` item on success.
  Future<void> sendChat(String text) async {
    final MeshLink? l = _link;
    if (l == null || text.trim().isEmpty) return;
    await l.client.chat(text.trim());
    _addFeed(MeshFeedItem('sent', text.trim(),
        from: _status['name'] ?? 'me'));
  }

  /// Re-read the peer table (`# peer <id> <name> <ipv6> last-seen=<N>s ago`).
  Future<void> refreshPeers() async {
    final MeshLink? l = _link;
    if (l == null) return;
    try {
      final (_, List<String> comments) =
          await l.client.commandCaptured('PEERS');
      final List<MeshPeer> out = <MeshPeer>[];
      for (final String c in comments) {
        if (!c.startsWith('peer ')) continue;
        final List<String> p = c.split(RegExp(r'\s+'));
        if (p.length < 4) continue;
        Duration? seen;
        final Match? m = RegExp(r'last-seen=(\d+)s').firstMatch(c);
        if (m != null) seen = Duration(seconds: int.parse(m.group(1)!));
        out.add(MeshPeer(id: p[1], name: p[2], ipv6: p[3], lastSeen: seen));
      }
      _peers = out;
      notifyListeners();
    } catch (_) {/* snapshot only — the live feed still works */}
  }

  /// Re-read mesh topology (`MESH` → `# mesh …` lines).
  Future<void> refreshTopology() async {
    final MeshLink? l = _link;
    if (l == null) return;
    try {
      final (_, List<String> comments) =
          await l.client.commandCaptured('MESH');
      _topology = MeshTopology.parse(comments);
      notifyListeners();
    } catch (_) {}
  }

  Future<void> refreshStatus() async {
    final MeshLink? l = _link;
    if (l == null) return;
    try {
      _status = await l.client.status();
      notifyListeners();
    } catch (_) {}
  }

  /// Run an arbitrary HCP verb on the companion and return the `+OK` body
  /// plus the `#` comments it produced (M4 test console). Throws
  /// [StateError] when no link is up; [HcpError]/[HcpTimeout] pass through
  /// so the console can show the real failure.
  Future<(String, List<String>)> run(String line,
      {Duration timeout = const Duration(seconds: 10)}) {
    final MeshLink? l = _link;
    if (l == null || _state != MeshLinkState.connected) {
      throw StateError('not connected to the companion node');
    }
    return l.client.commandCaptured(line, timeout: timeout);
  }

  /// Live `#` commentary from the companion (STRESS progress and friends).
  Stream<String>? get comments => _link?.client.comments;

  @override
  void dispose() {
    _teardown();
    super.dispose();
  }
}
