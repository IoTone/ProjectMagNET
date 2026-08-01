// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// BLE transport for the MagNET HCP service (design proposal §11.2.1).
///
/// A node advertises this service only while it is *unprovisioned* — once a
/// channel is set the firmware tears its BLE stack down and the radio belongs
/// to Thread alone. So an advertising node is, by definition, one waiting to
/// be configured.
library;
import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'hcp.dart';

/// Service and characteristic UUIDs (§11.2.1).
class MagnetUuids {
  static const String service = '6d61676e-2d68-6370-0001-000000000000';
  static const String cmd = '6d61676e-2d68-6370-0001-000000000001';
  static const String evt = '6d61676e-2d68-6370-0001-000000000002';

  /// Read-only, encryption-required. Reading it is how a host asks to bond:
  /// centrals pair when an *operation* needs encryption, and ignore a
  /// peripheral's bare security request.
  static const String auth = '6d61676e-2d68-6370-0001-000000000003';

  /// Advertised local-name prefix (`MagNET-<id lo16>`), the cheap filter
  /// before services are discovered — iOS often withholds service UUIDs in
  /// the advertisement until you connect.
  static const String namePrefix = 'MagNET-';

  static bool looksLikeNode({String? name, List<String> serviceUuids = const <String>[]}) {
    if (name != null && name.startsWith(namePrefix)) return true;
    return serviceUuids.any((String u) => u.toLowerCase() == service);
  }
}

/// Drives HCP over the node's GATT characteristics.
///
/// Framing note: the firmware treats each ATT write as a message boundary, so
/// a trailing newline is optional — but we send one anyway to stay identical
/// to the serial binding. Notifications are chunked at (MTU-3) by the node and
/// reassembled here on newline.
class MagnetBleTransport implements HcpTransport {
  MagnetBleTransport(this._device);

  final BluetoothDevice _device;
  BluetoothCharacteristic? _cmd;
  BluetoothCharacteristic? _evt;
  BluetoothCharacteristic? _auth;
  StreamSubscription<List<int>>? _notifySub;
  final StreamController<String> _lines = StreamController<String>.broadcast();
  final StringBuffer _rx = StringBuffer();

  @override
  Stream<String> get lines => _lines.stream;

  bool get isReady => _cmd != null && _evt != null;

  /// Connect, discover the MagNET service, and subscribe to notifications.
  /// Throws [StateError] when the peripheral isn't a MagNET node.
  Future<void> connect({Duration timeout = const Duration(seconds: 20)}) async {
    if (!_device.isConnected) {
      await _device.connect(timeout: timeout);
    }
    // A larger MTU means fewer notification chunks per line.
    try {
      await _device.requestMtu(247);
    } catch (_) {
      // Not supported on iOS (negotiated automatically) — harmless.
    }

    final List<BluetoothService> services = await _device.discoverServices();
    for (final BluetoothService s in services) {
      if (s.uuid.str.toLowerCase() != MagnetUuids.service) continue;
      for (final BluetoothCharacteristic c in s.characteristics) {
        final String u = c.uuid.str.toLowerCase();
        if (u == MagnetUuids.cmd) _cmd = c;
        if (u == MagnetUuids.evt) _evt = c;
        if (u == MagnetUuids.auth) _auth = c;
      }
    }
    if (!isReady) {
      throw StateError('not a MagNET node (HCP service not found)');
    }

    await _evt!.setNotifyValue(true);
    _notifySub = _evt!.onValueReceived.listen(_onChunk);
  }

  void _onChunk(List<int> data) {
    _rx.write(utf8.decode(data, allowMalformed: true));
    final String buf = _rx.toString();
    if (!buf.contains('\n')) return;
    final List<String> parts = buf.split('\n');
    _rx.clear();
    _rx.write(parts.removeLast()); // trailing partial line
    for (final String line in parts) {
      final String t = line.replaceAll('\r', '').trim();
      if (t.isNotEmpty) _lines.add(t);
    }
  }

  @override
  Future<void> send(String line) async {
    final BluetoothCharacteristic? c = _cmd;
    if (c == null) throw StateError('not connected');
    final List<int> bytes = utf8.encode('$line\n');
    // withoutResponse is faster but unreliable for long writes; the command
    // characteristic supports both, so prefer acknowledged writes.
    //
    // Android serialises GATT writes per characteristic. If a previous
    // operation is still settling the platform returns
    // ERROR_GATT_WRITE_REQUEST_BUSY immediately, and one slow response would
    // otherwise poison every command after it — so back off and retry.
    Object? last;
    for (int attempt = 0; attempt < 5; attempt++) {
      try {
        await c.write(bytes, withoutResponse: false);
        return;
      } catch (e) {
        last = e;
        if (!e.toString().contains('BUSY')) rethrow;
        await Future<void>.delayed(Duration(milliseconds: 120 * (attempt + 1)));
      }
    }
    throw StateError('write stayed busy: $last');
  }

  /// Bond with the node, so privileged verbs (`NAME`, `ADMIN ADD`,
  /// `CHANNEL SET`) are accepted instead of answering `E_NOT_BONDED`.
  ///
  /// Works by reading the encryption-required auth characteristic: that is an
  /// operation the central *must* encrypt, so it starts pairing. Returns true
  /// once the read succeeds, meaning the link is encrypted.
  Future<bool> bond({Duration timeout = const Duration(seconds: 45)}) async {
    final BluetoothCharacteristic? a = _auth;
    if (a == null) return false;            // firmware predates the auth char

    // Android: ask for the bond explicitly. Relying on an encrypted read to
    // trigger it makes the OS post a *notification* ("Tap to pair with …")
    // rather than a dialog; unattended, nobody taps it and SMP times out
    // (enc_change status=13). createBond() is the app-initiated path and
    // completes Just Works pairing without that detour. No iOS equivalent
    // exists — there the encrypted read below is the trigger.
    if (Platform.isAndroid) {
      try {
        if (!(await _device.bondState.first == BluetoothBondState.bonded)) {
          await _device.createBond();
        }
      } catch (_) {
        // already bonding, or the platform refused — the read still tries
      }
    }

    final DateTime deadline = DateTime.now().add(timeout);
    while (DateTime.now().isBefore(deadline)) {
      try {
        await a.read();
        return true;
      } catch (_) {
        // Pairing is in flight (or the user has yet to accept); the read
        // fails until the link is encrypted.
        await Future<void>.delayed(const Duration(seconds: 2));
      }
    }
    return false;
  }

  Future<void> dispose() async {
    await _notifySub?.cancel();
    await _lines.close();
    try {
      await _device.disconnect();
    } catch (_) {}
  }
}

/// Convenience: connect to a peripheral and hand back a live HCP client.
Future<(MagnetBleTransport, HcpClient)> connectToNode(
    BluetoothDevice device) async {
  final MagnetBleTransport t = MagnetBleTransport(device);
  await t.connect();
  return (t, HcpClient(t));
}
