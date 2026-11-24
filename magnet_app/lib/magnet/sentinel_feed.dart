// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// Room presence from Sentinel IR nodes (M5StickCPlus_Hive_Sentinel_IR).
///
/// A Sentinel is a PIR sensor on WiFi. It publishes to the MagNET MQTT
/// broker, so unlike Hanasu nodes it is reachable from any network, with no
/// BLE or companion node in between:
///
///   `iotj/magnet/sentinel/{id}/state`  retained: state + stats; the broker
///                                     swaps in {"online":false} (last will)
///                                     when the node drops off
///   `iotj/magnet/sentinel/{id}/event`  enter | present | clear | rollover | reset
///   `iotj/magnet/sentinel/{id}/activity`  retained: motion now, hold
///                                     countdown, last 5 min in 10-s bins
///   `iotj/magnet/sentinel/{id}/history`   retained: occupied seconds per
///                                     15 min over the last 24 h
///
/// Payload contract: the Sentinel README ("MQTT"). The feed lives above
/// navigation (like [MeshSession]) so events keep arriving while the user is
/// on other screens. It only connects once [start] is called, the first time
/// the Sentinels screen opens.
library;
import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'sentinel_mqtt_io.dart'
    if (dart.library.js_interop) 'sentinel_mqtt_web.dart';

enum SentinelLinkState { idle, connecting, connected, reconnecting, error }

/// Entries and occupied seconds for one calendar period.
class SentinelStat {
  const SentinelStat(this.entries, this.occupiedS);
  final int entries;
  final int occupiedS;
}

const List<String> kSentinelPeriods = <String>['hour', 'day', 'week', 'month'];

/// One decoded Sentinel payload (state or event share the same shape).
class SentinelMessage {
  SentinelMessage({
    required this.id,
    required this.ev,
    required this.state,
    required this.online,
    required this.at,
    required this.clockKnown,
    required this.sinceS,
    required this.stats,
    this.visitS,
    this.period,
    this.label,
    this.entries,
    this.occupiedS,
    this.periods = const <String>[],
    this.retained = false,
  });

  final String id;

  /// `state` | `enter` | `present` | `clear` | `rollover` | `reset`
  final String ev;

  /// `warmup` | `clear` | `present` | `offline`
  final String state;
  final bool online;

  /// Device time when its clock is set, otherwise the time it arrived here.
  final DateTime at;
  final bool clockKnown;
  final int sinceS;
  final Map<String, SentinelStat> stats;
  final int? visitS;
  final String? period;
  final String? label;
  final int? entries;
  final int? occupiedS;
  final List<String> periods;
  final bool retained;

  /// The `stats` object shared by state, event and (fw 0.3.0+) activity
  /// payloads. Empty when absent or malformed.
  static Map<String, SentinelStat> statsFrom(Object? raw) {
    final Map<String, SentinelStat> out = <String, SentinelStat>{};
    if (raw is! Map<String, dynamic>) return out;
    for (final String p in kSentinelPeriods) {
      final Object? s = raw[p];
      if (s is Map<String, dynamic>) {
        out[p] = SentinelStat(
            (s['entries'] as num?)?.toInt() ?? 0, (s['occ_s'] as num?)?.toInt() ?? 0);
      }
    }
    return out;
  }

  /// Parse one MQTT message. Returns null for anything that is not a
  /// Sentinel payload (wrong topic shape, bad JSON, cleared retained topic).
  static SentinelMessage? parse(String base, String topic, String payload,
      {bool retained = false, DateTime? now}) {
    if (!topic.startsWith('$base/')) return null;
    final List<String> parts = topic.substring(base.length + 1).split('/');
    if (parts.length != 2 || (parts[1] != 'state' && parts[1] != 'event')) {
      return null;
    }
    final Object? decoded;
    try {
      decoded = jsonDecode(payload);
    } on FormatException {
      return null;
    }
    if (decoded is! Map<String, dynamic>) return null;
    final Map<String, dynamic> m = decoded;

    int? asInt(Object? v) => v is num ? v.toInt() : null;
    final Map<String, SentinelStat> stats = statsFrom(m['stats']);
    final int? ts = asInt(m['ts']);
    final bool online = m['online'] != false;
    return SentinelMessage(
      id: (m['id'] as String?) ?? parts[0],
      ev: (m['ev'] as String?) ?? parts[1],
      state: online ? ((m['state'] as String?) ?? 'unknown') : 'offline',
      online: online,
      at: ts != null
          ? DateTime.fromMillisecondsSinceEpoch(ts * 1000)
          : (now ?? DateTime.now()),
      clockKnown: ts != null,
      sinceS: asInt(m['since_s']) ?? 0,
      stats: stats,
      visitS: asInt(m['visit_s']),
      period: m['period'] as String?,
      label: m['label'] as String?,
      entries: asInt(m['entries']),
      occupiedS: asInt(m['occ_s']),
      periods: (m['periods'] as List<dynamic>?)?.whereType<String>().toList() ??
          const <String>[],
      retained: retained,
    );
  }
}

/// What the app knows about one Sentinel.
class SentinelDevice {
  SentinelDevice(this.id);
  final String id;
  String state = 'unknown';
  bool online = false;
  bool clockKnown = false;

  /// When [state] began, as best we can tell.
  DateTime? stateSince;
  /// Last time the Sentinel sent anything (device clock when it has one).
  DateTime? lastHeard;
  Map<String, SentinelStat> stats = <String, SentinelStat>{};

  /// Newest first, capped at [SentinelFeed.maxEvents].
  final List<SentinelMessage> events = <SentinelMessage>[];

  /// From `/activity`: is the PIR seeing motion right now.
  bool active = false;

  /// When the last `/activity` message was sent (device clock if known).
  DateTime? activityAt;
  int? lastMotionS;
  int? clearInS;
  int holdS = 300;

  /// Seconds without any motion when [activityAt] was sent, and the limit
  /// past which the Sentinel asks for its sensor to be checked. A PIR with a
  /// loose wire reads exactly like an empty room; long silence is the tell.
  int? quietS;
  int quietAfterS = 0;

  /// Percent of each 10-s bin with motion, oldest first; the newest bin
  /// ends at [activityAt].
  List<int> recentPct = const <int>[];

  /// Occupied seconds per 15-min bin, oldest first; [historyEnd] is the
  /// start of the newest (still filling) bin.
  List<int> historyS = const <int>[];
  DateTime? historyEnd;

  bool get present => online && state == 'present';

  /// Seconds until the room reports clear, counted from [now]; the
  /// Sentinel restarts this at every sighting. Null when not present.
  /// Seconds since the last `/activity` message. Never negative: the phone's
  /// clock can run a little behind the Sentinel's.
  int _sinceActivity(DateTime now) {
    final int s = now.difference(activityAt!).inSeconds;
    return s < 0 ? 0 : s;
  }

  int? clearInAt(DateTime now) {
    if (!present || clearInS == null || activityAt == null) return null;
    if (active) return holdS;
    final int left = clearInS! - _sinceActivity(now);
    return left < 0 ? 0 : left;
  }

  /// How long the sensor has gone without motion, run on to [now].
  int? quietAt(DateTime now) {
    if (quietS == null || activityAt == null) return null;
    if (active) return 0;
    return quietS! + _sinceActivity(now);
  }

  /// True when the sensor has been silent past its limit. Counted here as
  /// well as on the device, so the warning appears on time even though the
  /// Sentinel publishes nothing while the room is quiet.
  bool checkSensorAt(DateTime now) {
    final int? q = quietAt(now);
    return online && quietAfterS > 0 && q != null && q >= quietAfterS;
  }

  int? lastMotionAgoAt(DateTime now) {
    if (lastMotionS == null || activityAt == null) return null;
    if (active) return 0;
    return lastMotionS! + _sinceActivity(now);
  }

  /// [recentPct] / [historyS] shifted to [now], so a quiet or offline
  /// Sentinel's chart still scrolls instead of freezing.
  List<int> recentAt(DateTime now) =>
      _shift(recentPct, activityAt, now, const Duration(seconds: 10));
  List<int> historyAt(DateTime now) =>
      _shift(historyS, historyEnd, now, const Duration(minutes: 15));

  static List<int> _shift(List<int> v, DateTime? end, DateTime now, Duration bin) {
    if (v.isEmpty || end == null) return v;
    final int n = now.difference(end).inMicroseconds ~/ bin.inMicroseconds;
    if (n <= 0) return v;
    if (n >= v.length) return List<int>.filled(v.length, 0);
    return <int>[...v.sublist(n), ...List<int>.filled(n, 0)];
  }
}

class SentinelFeed extends ChangeNotifier {
  SentinelFeed(
      {MqttClient Function(Uri broker, String clientId)? clientFactory,
      this.connectOnStart = true})
      : _factory = clientFactory ?? makeSentinelMqttClient;

  /// False in widget tests: [start] then never opens a network connection.
  final bool connectOnStart;

  static const String defaultBroker = 'mqtt://broker.hivemq.com:1883';
  static const String base = 'iotj/magnet/sentinel';
  static const String _prefBroker = 'sentinel.broker';
  static const int maxEvents = 50;

  final MqttClient Function(Uri, String) _factory;
  MqttClient? _client;
  StreamSubscription<List<MqttReceivedMessage<MqttMessage>>>? _sub;
  Timer? _retry;
  int _attempt = 0;
  bool _started = false;
  bool _disposed = false;

  String broker = defaultBroker;
  SentinelLinkState link = SentinelLinkState.idle;
  String? lastError;
  final Map<String, SentinelDevice> _devices = <String, SentinelDevice>{};

  List<SentinelDevice> get devices =>
      _devices.values.toList()..sort((a, b) => a.id.compareTo(b.id));

  int get presentCount => _devices.values.where((d) => d.present).length;

  int checkSensorCount(DateTime now) =>
      _devices.values.where((d) => d.checkSensorAt(now)).length;

  /// Load the saved broker and connect. Safe to call more than once.
  Future<void> start() async {
    if (_started) return;
    _started = true;
    try {
      final SharedPreferences p = await SharedPreferences.getInstance();
      broker = p.getString(_prefBroker) ?? defaultBroker;
    } catch (_) {/* keep the default */}
    if (connectOnStart) await _connect();
  }

  Future<void> setBroker(String uri) async {
    final String b = uri.trim().isEmpty ? defaultBroker : uri.trim();
    broker = b;
    try {
      final SharedPreferences p = await SharedPreferences.getInstance();
      await p.setString(_prefBroker, b);
    } catch (_) {}
    _devices.clear();
    await _teardown();
    _attempt = 0;
    _started = true;
    await _connect();
  }

  Future<void> reconnect() async {
    await _teardown();
    _attempt = 0;
    await _connect();
  }

  Future<void> _connect() async {
    if (_disposed) return;
    final Uri? uri = Uri.tryParse(broker.contains('://') ? broker : 'mqtt://$broker');
    if (uri == null || uri.host.isEmpty) {
      _setLink(SentinelLinkState.error, 'Not a broker address: $broker');
      return;
    }
    _setLink(_attempt == 0 ? SentinelLinkState.connecting : SentinelLinkState.reconnecting);
    final String clientId =
        'magnet_app_${Random().nextInt(0x7fffffff).toRadixString(16).padLeft(8, '0')}';
    final MqttClient c = _factory(uri, clientId);
    c.logging(on: false);
    c.keepAlivePeriod = 30;
    c.autoReconnect = true;
    c.resubscribeOnAutoReconnect = true;
    c.onAutoReconnect = () => _setLink(SentinelLinkState.reconnecting);
    c.onAutoReconnected = () => _setLink(SentinelLinkState.connected);
    c.connectionMessage =
        MqttConnectMessage().withClientIdentifier(clientId).startClean();
    _client = c;
    try {
      await c.connect();
    } catch (e) {
      c.disconnect();
      _client = null;
      _scheduleRetry('$e');
      return;
    }
    if (c.connectionStatus?.state != MqttConnectionState.connected) {
      _client = null;
      _scheduleRetry('broker refused: ${c.connectionStatus?.returnCode}');
      return;
    }
    _attempt = 0;
    _sub = c.updates?.listen(_onMessages);
    c.subscribe('$base/+/#', MqttQos.atLeastOnce);
    _setLink(SentinelLinkState.connected);
  }

  void _scheduleRetry(String why) {
    if (_disposed) return;
    _attempt++;
    _setLink(SentinelLinkState.error, why);
    final int secs = min(60, 2 << min(_attempt, 5));
    _retry?.cancel();
    _retry = Timer(Duration(seconds: secs), _connect);
  }

  void _onMessages(List<MqttReceivedMessage<MqttMessage>> msgs) {
    for (final MqttReceivedMessage<MqttMessage> m in msgs) {
      final MqttMessage p = m.payload;
      if (p is! MqttPublishMessage) continue;
      final String text = utf8.decode(p.payload.message, allowMalformed: true);
      ingest(m.topic, text, retained: p.header?.retain ?? false);
    }
  }

  /// Apply one MQTT message. Public so tests can drive the feed directly.
  void ingest(String topic, String payload, {bool retained = false, DateTime? now}) {
    if (topic.endsWith('/activity') || topic.endsWith('/history')) {
      if (_ingestSeries(topic, payload, now ?? DateTime.now())) notifyListeners();
      return;
    }
    final SentinelMessage? m =
        SentinelMessage.parse(base, topic, payload, retained: retained, now: now);
    if (m == null) return;
    final SentinelDevice d = _devices.putIfAbsent(m.id, () => SentinelDevice(m.id));
    final DateTime rx = now ?? DateTime.now();
    // A retained state may be hours old by the time it arrives; when the
    // device stamped it, date it by the device clock, not by arrival.
    final DateTime sent = m.clockKnown ? m.at : rx;
    if (m.stats.isNotEmpty) d.stats = m.stats;
    if (d.lastHeard == null || sent.isAfter(d.lastHeard!)) d.lastHeard = sent;
    if (m.ev == 'state') {
      d.online = m.online;
      if (m.online) {
        d.clockKnown = m.clockKnown;
        if (m.state != d.state || d.stateSince == null) {
          d.stateSince = sent.subtract(Duration(seconds: m.sinceS));
        }
      }
      d.state = m.state;
    } else {
      d.online = true;
      if (m.state != d.state) d.stateSince = sent;
      d.state = m.state;
      d.clockKnown = m.clockKnown;
      d.events.insert(0, m);
      if (d.events.length > maxEvents) d.events.removeLast();
    }
    notifyListeners();
  }

  bool _ingestSeries(String topic, String payload, DateTime rx) {
    if (!topic.startsWith('$base/')) return false;
    final List<String> parts = topic.substring(base.length + 1).split('/');
    if (parts.length != 2) return false;
    final Object? decoded;
    try {
      decoded = jsonDecode(payload);
    } on FormatException {
      return false;
    }
    if (decoded is! Map<String, dynamic>) return false;
    final Map<String, dynamic> m = decoded;
    int? asInt(Object? v) => v is num ? v.toInt() : null;
    List<int> ints(Object? v) =>
        v is List ? v.map((Object? e) => asInt(e) ?? 0).toList() : const <int>[];
    final int? ts = asInt(m['ts']);
    final DateTime sent = ts != null ? DateTime.fromMillisecondsSinceEpoch(ts * 1000) : rx;
    final SentinelDevice d =
        _devices.putIfAbsent((m['id'] as String?) ?? parts[0], () => SentinelDevice(parts[0]));
    if (d.lastHeard == null || sent.isAfter(d.lastHeard!)) d.lastHeard = sent;
    if (parts[1] == 'activity') {
      d.active = m['active'] == true;
      d.activityAt = sent;
      d.lastMotionS = asInt(m['last_motion_s']);
      d.clearInS = asInt(m['clear_in_s']);
      d.holdS = asInt(m['hold_s']) ?? d.holdS;
      d.quietS = asInt(m['quiet_s']);
      d.quietAfterS = asInt(m['quiet_after_s']) ?? 0;
      // fw 0.3.0+ repeats the counters here so they stay fresh between events.
      final Map<String, SentinelStat> counters = SentinelMessage.statsFrom(m['stats']);
      if (counters.isNotEmpty) d.stats = counters;
      d.recentPct = ints(m['bins_pct']);
      final String? st = m['state'] as String?;
      if (st != null && d.online) d.state = st;
    } else {
      d.historyS = ints(m['bins_s']);
      final int? end = asInt(m['end']);
      d.historyEnd = end != null ? DateTime.fromMillisecondsSinceEpoch(end * 1000) : null;
    }
    return true;
  }

  void _setLink(SentinelLinkState s, [String? err]) {
    link = s;
    lastError = err;
    if (!_disposed) notifyListeners();
  }

  Future<void> _teardown() async {
    _retry?.cancel();
    await _sub?.cancel();
    _sub = null;
    _client?.autoReconnect = false;
    _client?.disconnect();
    _client = null;
    link = SentinelLinkState.idle;
  }

  @override
  void dispose() {
    _disposed = true;
    _teardown();
    super.dispose();
  }
}
