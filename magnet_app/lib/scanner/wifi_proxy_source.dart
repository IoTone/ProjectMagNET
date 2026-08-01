// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:nsd/nsd.dart';

import 'scanned_access_point.dart';
import 'wifi_source.dart';

/// WiFi source backed by an external **relay proxy** (a Mac or Linux
/// box on the same LAN running tools/wifi-proxy). The proxy scans WiFi
/// locally and serves the result as JSON; it advertises itself over
/// Bonjour/mDNS as `_iotscan._tcp`. This is the way to get real
/// nearby-WiFi scanning on iOS, where Apple blocks on-device AP scans.
///
/// Discovery → resolve host:port → poll `GET /aps` → map to
/// [ScannedAccessPoint]. Caveat: the proxy reports the *proxy's* RF
/// vicinity, not the phone's.
class ProxyWifiSource implements WifiSource {
  ProxyWifiSource({
    this.serviceType = '_iotscan._tcp',
    this.path = '/aps',
    this.interval = const Duration(seconds: 6),
  });

  final String serviceType;
  final String path;
  final Duration interval;

  final StreamController<List<ScannedAccessPoint>> _ctrl =
      StreamController<List<ScannedAccessPoint>>.broadcast();
  final HttpClient _http = HttpClient()
    ..connectionTimeout = const Duration(seconds: 3);

  Discovery? _discovery;
  Timer? _timer;
  String? _host;
  int? _port;

  @override
  Stream<List<ScannedAccessPoint>> get accessPoints => _ctrl.stream;

  @override
  WifiCapability get capability => WifiCapability.fullScan;

  /// Host:port of the resolved proxy, for a UI status line, or null.
  String? get endpoint =>
      (_host != null && _port != null) ? '$_host:$_port' : null;

  @override
  Future<void> start() async {
    _discovery = await startDiscovery(serviceType);
    _discovery!.addListener(_onServices);
    _timer ??= Timer.periodic(interval, (_) => _poll());
  }

  void _onServices() {
    final Discovery? d = _discovery;
    if (d == null || d.services.isEmpty) return;
    final Service s = d.services.first;
    if (s.host != null && s.port != null) {
      _host = s.host;
      _port = s.port;
    }
  }

  Future<void> _poll() async {
    final String? host = _host;
    final int? port = _port;
    if (host == null || port == null) return;
    try {
      final HttpClientRequest req =
          await _http.getUrl(Uri.parse('http://$host:$port$path'));
      final HttpClientResponse resp = await req.close();
      if (resp.statusCode != 200) return;
      final String body = await resp.transform(utf8.decoder).join();
      final dynamic doc = jsonDecode(body);
      if (doc is! List) return;
      final DateTime now = DateTime.now();
      _ctrl.add(<ScannedAccessPoint>[
        for (final dynamic e in doc)
          if (e is Map<String, dynamic>)
            ScannedAccessPoint(
              ssid: (e['ssid'] ?? '').toString(),
              bssid: (e['bssid'] ?? '').toString(),
              rssi: (e['rssi'] as num?)?.toInt() ?? -100,
              frequencyMhz: (e['freq'] as num?)?.toInt(),
              capabilities: (e['security'] ?? '').toString(),
              lastSeen: now,
            ),
      ]);
    } catch (_) {
      // proxy offline / unreachable — emit nothing this tick.
    }
  }

  @override
  Future<void> stop() async {
    _timer?.cancel();
    _timer = null;
    final Discovery? d = _discovery;
    _discovery = null;
    if (d != null) {
      d.removeListener(_onServices);
      try {
        await stopDiscovery(d);
      } catch (_) {}
    }
  }

  @override
  void dispose() {
    unawaited(stop());
    _http.close(force: true);
    _ctrl.close();
  }
}
