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
import 'package:wakelock_plus/wakelock_plus.dart';

import 'hcp.dart';

/// Why a bond attempt ended. The caller must distinguish "the user said no"
/// from "the prompt was never answered" — they need different UI.
enum BondOutcome {
  /// Already bonded from a previous session; nothing to do.
  alreadyBonded,

  /// The user accepted the system prompt and the link is encrypted.
  bonded,

  /// The user actively dismissed or denied the system pairing request.
  declined,

  /// Nobody answered the prompt in time.
  timedOut,

  /// This firmware has no auth characteristic, so bonding cannot be triggered.
  unsupported,
}

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

  /// Live bond state, straight from the platform. The UI must follow this
  /// rather than any in-app affordance: **only the operating system's own
  /// pairing prompt can complete a bond.** An in-app "pair" button is worse
  /// than useless — a user who taps it instead of the system prompt believes
  /// they have answered, while the real request goes unanswered and the link
  /// dies. Show progress, never a substitute action.
  Stream<BluetoothBondState> get bondState => _device.bondState;

  /// Trigger bonding and wait for the user to answer the system prompt.
  ///
  /// Returns why it ended so the caller can tell "declined" from "ignored".
  Future<BondOutcome> bond({
    Duration timeout = const Duration(seconds: 120),
  }) async {
    final BluetoothCharacteristic? a = _auth;
    if (a == null) return BondOutcome.unsupported;

    if (await _device.bondState.first == BluetoothBondState.bonded) {
      return BondOutcome.alreadyBonded;
    }

    // Hold the screen awake: Android's prompt does not survive the display
    // dimming, and a torn-down prompt reads to the node as a plain timeout.
    try {
      await WakelockPlus.enable();
    } catch (_) {}

    final Completer<BondOutcome> done = Completer<BondOutcome>();
    DateTime? bondingSince;
    final StreamSubscription<BluetoothBondState> sub =
        _device.bondState.listen((BluetoothBondState s) {
      if (s == BluetoothBondState.bonded) {
        if (!done.isCompleted) done.complete(BondOutcome.bonded);
      } else if (s == BluetoothBondState.bonding) {
        bondingSince = DateTime.now();        // prompt is up
      } else if (s == BluetoothBondState.none && bondingSince != null) {
        // `bonding -> none` is ambiguous: the user refused, *or* nobody
        // answered and Android expired the request on its own. It expires at
        // almost exactly 30 s (measured on SH-53D: BONDING 00:59:51.857 ->
        // NONE 01:00:21.864), so treat a collapse at that mark as unanswered.
        // The difference matters — "you declined" is the wrong thing to tell
        // someone who never saw a prompt.
        final Duration held = DateTime.now().difference(bondingSince!);
        final bool expired = held >= const Duration(seconds: 28);
        if (!done.isCompleted) {
          done.complete(expired ? BondOutcome.timedOut : BondOutcome.declined);
        }
      }
    });

    // Ask for the bond. On Android this is the app-initiated path; elsewhere
    // reading the encryption-required characteristic is the trigger.
    if (Platform.isAndroid) {
      try {
        await _device.createBond();
      } catch (_) {/* already bonding, or the platform refused */}
    } else {
      unawaited(a.read().catchError((_) => <int>[]));
    }

    BondOutcome outcome;
    try {
      outcome = await done.future.timeout(timeout);
    } on TimeoutException {
      outcome = BondOutcome.timedOut;
    } finally {
      await sub.cancel();
      await releaseWakelock();
    }
    return outcome;
  }

  /// Drop this phone's half of a bond the node no longer holds.
  ///
  /// A node keeps its BLE *address* across `FACTORY RESET` (it advertises with
  /// `BLE_OWN_ADDR_PUBLIC`, i.e. the MAC) but loses its keys. The phone is then
  /// left believing it is bonded to something that has forgotten it, and
  /// nothing recovers on its own: Android keeps offering an LTK the node cannot
  /// match, `bond()` short-circuits to [BondOutcome.alreadyBonded] so no system
  /// prompt is ever raised, and every privileged verb answers `E_NOT_BONDED`
  /// forever. Observed 2026-08-02 — the node logged `enc_change status=13`.
  ///
  /// This matters because `FACTORY RESET` is the *documented* recovery path:
  /// without this the act of recovering a node makes it unprovisionable by the
  /// phone that recovered it.
  ///
  /// `removeBond()` tears the connection down, so the caller must reconnect
  /// before using the transport again. Android only — iOS gives an app no way
  /// to forget a pairing, so there the user must do it in Settings.
  Future<bool> clearStaleBond() async {
    if (!Platform.isAndroid) return false;
    try {
      await _device.removeBond();
      await _device.bondState
          .firstWhere((BluetoothBondState s) => s == BluetoothBondState.none)
          .timeout(const Duration(seconds: 10));
      return true;
    } catch (_) {
      return false;
    }
  }

  /// Reconnect after [clearStaleBond], then bond afresh. Returns the outcome
  /// of the new pairing — which *will* raise a system prompt, because the
  /// platform no longer thinks it knows this node.
  Future<BondOutcome> recoverStaleBondAndRebond({
    Duration timeout = const Duration(seconds: 120),
  }) async {
    if (!await clearStaleBond()) return BondOutcome.unsupported;
    _cmd = _evt = _auth = null;
    await _notifySub?.cancel();
    _notifySub = null;
    await connect();
    return bond(timeout: timeout);
  }

  /// Release the screen lock taken for bonding.
  Future<void> releaseWakelock() async {
    try {
      await WakelockPlus.disable();
    } catch (_) {}
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
