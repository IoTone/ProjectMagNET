// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:async';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

/// One BLE advertisement, decoupled from the `flutter_blue_plus`
/// `ScanResult` type so [ScannerController] and its tests don't depend
/// on the plugin.
class BleAd {
  const BleAd({
    required this.id,
    required this.name,
    required this.rssi,
    this.txPowerLevel,
    this.manufacturerData = const <int, List<int>>{},
    this.serviceUuids = const <String>[],
  });

  final String id;
  final String name;
  final int rssi;
  final int? txPowerLevel;
  final Map<int, List<int>> manufacturerData;
  final List<String> serviceUuids;
}

/// Whether Bluetooth is usable right now.
enum BleAdapterState { unknown, off, unauthorized, on }

/// A discovered GATT service and its characteristic UUIDs.
class GattService {
  const GattService({required this.uuid, required this.characteristics});
  final String uuid;
  final List<String> characteristics;
}

/// Abstraction over BLE scanning so the controller can be driven by a
/// fake in tests. The platform implementation is [FlutterBlueScanner].
abstract class BleScanner {
  /// Batched scan results — emits the current set of in-range ads on
  /// every update.
  Stream<List<BleAd>> get ads;

  /// Whether a scan is currently running.
  Stream<bool> get isScanning;

  /// Adapter (radio) availability.
  Stream<BleAdapterState> get adapterState;

  Future<void> start();
  Future<void> stop();

  /// Connect to a device and return its discovered GATT services.
  Future<List<GattService>> connect(String id);

  /// Disconnect a previously-connected device.
  Future<void> disconnect(String id);

  void dispose();
}

/// Real BLE scanning via `flutter_blue_plus`.
class FlutterBlueScanner implements BleScanner {
  @override
  Stream<List<BleAd>> get ads =>
      FlutterBluePlus.scanResults.map((List<ScanResult> rs) =>
          rs.map(_fromResult).toList(growable: false));

  @override
  Stream<bool> get isScanning => FlutterBluePlus.isScanning;

  @override
  Stream<BleAdapterState> get adapterState =>
      FlutterBluePlus.adapterState.map(_mapAdapter);

  @override
  Future<void> start() async {
    // Continuous mode keeps RSSI fresh and prunes devices that fall
    // out of range, which is what the radar/list want.
    await FlutterBluePlus.startScan(
      continuousUpdates: true,
      removeIfGone: const Duration(seconds: 15),
    );
  }

  @override
  Future<void> stop() => FlutterBluePlus.stopScan();

  @override
  Future<List<GattService>> connect(String id) async {
    final BluetoothDevice device = BluetoothDevice.fromId(id);
    await device.connect(timeout: const Duration(seconds: 15));
    final List<BluetoothService> services = await device.discoverServices();
    return services
        .map((BluetoothService s) => GattService(
              uuid: s.uuid.str.toLowerCase(),
              characteristics: s.characteristics
                  .map((BluetoothCharacteristic c) => c.uuid.str.toLowerCase())
                  .toList(),
            ))
        .toList();
  }

  @override
  Future<void> disconnect(String id) =>
      BluetoothDevice.fromId(id).disconnect();

  @override
  void dispose() {}

  static BleAd _fromResult(ScanResult r) {
    final AdvertisementData a = r.advertisementData;
    final String name =
        a.advName.isNotEmpty ? a.advName : r.device.platformName;
    return BleAd(
      id: r.device.remoteId.str,
      name: name,
      rssi: r.rssi,
      txPowerLevel: a.txPowerLevel,
      manufacturerData: a.manufacturerData,
      serviceUuids:
          a.serviceUuids.map((Guid g) => g.str.toLowerCase()).toList(),
    );
  }

  static BleAdapterState _mapAdapter(BluetoothAdapterState s) {
    switch (s) {
      case BluetoothAdapterState.on:
        return BleAdapterState.on;
      case BluetoothAdapterState.off:
      case BluetoothAdapterState.turningOff:
        return BleAdapterState.off;
      case BluetoothAdapterState.unauthorized:
        return BleAdapterState.unauthorized;
      default:
        return BleAdapterState.unknown;
    }
  }
}
