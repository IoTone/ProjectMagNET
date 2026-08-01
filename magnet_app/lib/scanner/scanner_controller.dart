// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:async';

import 'package:flutter/foundation.dart';

import 'ble_scanner.dart';
import 'scan_event.dart';
import 'scanned_access_point.dart';
import 'scanned_device.dart';
import 'wifi_source.dart';

/// GATT connection state of a single device, for the detail UI.
enum DeviceConnection { disconnected, connecting, connected }

/// App-facing scan state, exposed via Provider. Fuses a [BleScanner]
/// and a [WifiSource] into one observable surface: the discovered BLE
/// devices, the visible WiFi access points, the radio/scan state, and
/// a rolling diagnostics log.
///
/// The two sources are injected so the whole controller is unit
/// testable with fakes — no Bluetooth/WiFi hardware required.
class ScannerController extends ChangeNotifier {
  ScannerController({required BleScanner ble, WifiSource? wifi})
      : _ble = ble,
        _wifi = wifi ?? NoopWifiSource() {
    _adapterSub = _ble.adapterState.listen(_onAdapter);
    _scanningSub = _ble.isScanning.listen(_onScanning);
  }

  final BleScanner _ble;
  WifiSource _wifi;

  StreamSubscription<List<BleAd>>? _adsSub;
  StreamSubscription<bool>? _scanningSub;
  StreamSubscription<BleAdapterState>? _adapterSub;
  StreamSubscription<List<ScannedAccessPoint>>? _apsSub;

  final Map<String, ScannedDevice> _devices = <String, ScannedDevice>{};
  final Map<String, ScannedAccessPoint> _aps = <String, ScannedAccessPoint>{};
  final List<ScanEvent> _log = <ScanEvent>[];
  static const int _logCap = 300;

  bool _scanning = false;
  BleAdapterState _adapter = BleAdapterState.unknown;

  /// Hide devices/APs weaker than this RSSI (dBm). -100 = show all.
  int _rssiFloor = -100;

  // Single active GATT connection (the device-detail flow connects one
  // device at a time).
  String? _connectingId;
  String? _connectedId;
  bool _connectFailed = false;
  List<GattService> _services = const <GattService>[];

  // --- Public surface ---

  /// Discovered BLE devices passing the RSSI floor, strongest first.
  List<ScannedDevice> get devices {
    final List<ScannedDevice> list = _devices.values
        .where((ScannedDevice d) => d.rssi >= _rssiFloor)
        .toList();
    list.sort((ScannedDevice a, ScannedDevice b) => b.rssi.compareTo(a.rssi));
    return List<ScannedDevice>.unmodifiable(list);
  }

  /// Visible WiFi access points passing the RSSI floor, strongest first.
  List<ScannedAccessPoint> get accessPoints {
    final List<ScannedAccessPoint> list = _aps.values
        .where((ScannedAccessPoint a) => a.rssi >= _rssiFloor)
        .toList();
    list.sort((ScannedAccessPoint a, ScannedAccessPoint b) =>
        b.rssi.compareTo(a.rssi));
    return List<ScannedAccessPoint>.unmodifiable(list);
  }

  int get bleCount => devices.length;
  int get wifiCount => accessPoints.length;

  int get rssiFloor => _rssiFloor;
  void setRssiFloor(int dbm) {
    final int v = dbm.clamp(-100, -40);
    if (v == _rssiFloor) return;
    _rssiFloor = v;
    notifyListeners();
  }
  bool get isScanning => _scanning;
  BleAdapterState get adapterState => _adapter;
  WifiCapability get wifiCapability => _wifi.capability;
  List<ScanEvent> get log => List<ScanEvent>.unmodifiable(_log.reversed);

  ScannedDevice? deviceById(String id) => _devices[id];

  /// GATT services discovered on the currently-connected device.
  List<GattService> get services => _services;

  /// Connection state of [id] for the device-detail UI.
  DeviceConnection connectionFor(String id) {
    if (_connectingId == id) return DeviceConnection.connecting;
    if (_connectedId == id) return DeviceConnection.connected;
    return DeviceConnection.disconnected;
  }

  /// True if the most recent connect attempt failed (cleared on the
  /// next attempt).
  bool get connectFailed => _connectFailed;

  /// Connect to a device and discover its GATT services.
  Future<void> connect(String id) async {
    if (_connectingId != null || _connectedId != null) return;
    _connectingId = id;
    _connectFailed = false;
    notifyListeners();
    try {
      _services = await _ble.connect(id);
      _connectedId = id;
    } catch (_) {
      _connectFailed = true;
      _services = const <GattService>[];
    } finally {
      _connectingId = null;
      notifyListeners();
    }
  }

  Future<void> disconnect() async {
    final String? id = _connectedId;
    if (id == null) return;
    try {
      await _ble.disconnect(id);
    } catch (_) {
      // ignore — treat as disconnected regardless
    }
    _connectedId = null;
    _services = const <GattService>[];
    notifyListeners();
  }

  /// Swap the WiFi source at runtime (e.g. when a relay proxy is
  /// discovered or permissions change). Re-subscribes if scanning.
  void setWifiSource(WifiSource source) {
    if (identical(source, _wifi)) return;
    final bool wasScanning = _scanning;
    _apsSub?.cancel();
    _wifi.dispose();
    _wifi = source;
    if (wasScanning) {
      unawaited(_wifi.start());
      _apsSub = _wifi.accessPoints.listen(_onAccessPoints);
    }
    notifyListeners();
  }

  Future<void> start() async {
    _adsSub ??= _ble.ads.listen(_onAds);
    _apsSub ??= _wifi.accessPoints.listen(_onAccessPoints);
    _record(ScanEventKind.started, 'scan started');
    await _ble.start();
    await _wifi.start();
  }

  Future<void> stop() async {
    await _ble.stop();
    await _wifi.stop();
    _record(ScanEventKind.stopped, 'scan stopped');
  }

  Future<void> toggle() => _scanning ? stop() : start();

  /// Clear all discovered devices/APs (keeps scanning if active).
  void clear() {
    _devices.clear();
    _aps.clear();
    _record(ScanEventKind.cleared, 'results cleared');
    notifyListeners();
  }

  // --- Stream handlers ---

  void _onAds(List<BleAd> ads) {
    final DateTime now = DateTime.now();
    // flutter_blue_plus emits the full current in-range set each
    // update, so reconcile: keep firstSeen for known ids, drop ones
    // that fell out of range.
    final Map<String, ScannedDevice> next = <String, ScannedDevice>{};
    for (final BleAd ad in ads) {
      final ScannedDevice? prev = _devices[ad.id];
      if (prev == null) {
        _record(ScanEventKind.bleFound,
            '${ad.name.isEmpty ? ad.id : ad.name} (${ad.rssi} dBm)');
      }
      next[ad.id] = ScannedDevice(
        id: ad.id,
        name: ad.name,
        rssi: ad.rssi,
        txPowerLevel: ad.txPowerLevel,
        manufacturerData: ad.manufacturerData,
        serviceUuids: ad.serviceUuids,
        firstSeen: prev?.firstSeen ?? now,
        lastSeen: now,
      );
    }
    _devices
      ..clear()
      ..addAll(next);
    notifyListeners();
  }

  void _onAccessPoints(List<ScannedAccessPoint> aps) {
    for (final ScannedAccessPoint ap in aps) {
      if (!_aps.containsKey(ap.bssid)) {
        _record(ScanEventKind.wifiFound,
            '${ap.displayName} (${ap.rssi} dBm)');
      }
    }
    _aps
      ..clear()
      ..addEntries(aps.map((ScannedAccessPoint a) =>
          MapEntry<String, ScannedAccessPoint>(a.bssid, a)));
    notifyListeners();
  }

  void _onScanning(bool v) {
    if (v == _scanning) return;
    _scanning = v;
    notifyListeners();
  }

  void _onAdapter(BleAdapterState s) {
    if (s == _adapter) return;
    _adapter = s;
    _record(ScanEventKind.adapter, 'bluetooth ${s.name}');
    notifyListeners();
  }

  void _record(ScanEventKind kind, String detail) {
    _log.add(ScanEvent(at: DateTime.now(), kind: kind, detail: detail));
    if (_log.length > _logCap) _log.removeAt(0);
  }

  @override
  void dispose() {
    if (_connectedId != null) unawaited(_ble.disconnect(_connectedId!));
    _adsSub?.cancel();
    _scanningSub?.cancel();
    _adapterSub?.cancel();
    _apsSub?.cancel();
    _ble.dispose();
    _wifi.dispose();
    super.dispose();
  }
}
