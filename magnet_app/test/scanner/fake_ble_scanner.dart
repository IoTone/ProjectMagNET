// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:async';

import 'package:magnet_app/scanner/ble_scanner.dart';

/// In-memory [BleScanner] for unit tests — drive the streams directly,
/// no Bluetooth hardware.
class FakeBleScanner implements BleScanner {
  final StreamController<List<BleAd>> _ads =
      StreamController<List<BleAd>>.broadcast();
  final StreamController<bool> _scanning = StreamController<bool>.broadcast();
  final StreamController<BleAdapterState> _adapter =
      StreamController<BleAdapterState>.broadcast();

  List<GattService> servicesToReturn = const <GattService>[];
  bool throwOnConnect = false;
  bool started = false;
  String? lastDisconnectedId;

  @override
  Stream<List<BleAd>> get ads => _ads.stream;
  @override
  Stream<bool> get isScanning => _scanning.stream;
  @override
  Stream<BleAdapterState> get adapterState => _adapter.stream;

  @override
  Future<void> start() async {
    started = true;
    _scanning.add(true);
  }

  @override
  Future<void> stop() async {
    started = false;
    _scanning.add(false);
  }

  @override
  Future<List<GattService>> connect(String id) async {
    if (throwOnConnect) throw Exception('connect failed');
    return servicesToReturn;
  }

  @override
  Future<void> disconnect(String id) async {
    lastDisconnectedId = id;
  }

  @override
  void dispose() {
    _ads.close();
    _scanning.close();
    _adapter.close();
  }

  // Test drivers.
  void emitAds(List<BleAd> ads) => _ads.add(ads);
  void emitAdapter(BleAdapterState s) => _adapter.add(s);
}
