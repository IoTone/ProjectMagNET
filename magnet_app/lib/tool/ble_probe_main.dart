// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// Headless hardware pass for the BLE path (SCOPE.md M1).
///
/// Everything in `magnet/magnet_ble.dart` + `magnet/hcp.dart` was written from
/// the firmware source and verified only by unit tests against a fake
/// transport. This drives the *real* stack against a real node and prints a
/// pass/fail report, so the BLE path is proven before any UI is built on it.
///
///     flutter run -d macos -t lib/tool/ble_probe_main.dart
///
/// Needs an unprovisioned node in range (one still on the `magnet` channel —
/// a provisioned node tears its BLE stack down and will not appear).
library;

import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import '../magnet/hcp.dart';
import '../magnet/magnet_ble.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const _ProbeApp());
}

/// Live report. The probe runs on a device in someone's hand — a blank screen
/// while it waits on a permission or pairing prompt is indistinguishable from
/// a hang, so every line lands here as well as in the log.
final ValueNotifier<List<String>> _lines = ValueNotifier<List<String>>(<String>[]);
final ValueNotifier<bool> _busy = ValueNotifier<bool>(true);

class _ProbeApp extends StatefulWidget {
  const _ProbeApp();
  @override
  State<_ProbeApp> createState() => _ProbeAppState();
}

class _ProbeAppState extends State<_ProbeApp> {
  @override
  void initState() {
    super.initState();
    unawaited(_run());
  }

  @override
  Widget build(BuildContext context) => MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: ThemeData.dark(useMaterial3: true),
        home: Scaffold(
          appBar: AppBar(
            title: const Text('MagNET BLE probe'),
            bottom: PreferredSize(
              preferredSize: const Size.fromHeight(3),
              child: ValueListenableBuilder<bool>(
                valueListenable: _busy,
                builder: (_, bool b, __) =>
                    b ? const LinearProgressIndicator(minHeight: 3)
                      : const SizedBox(height: 3),
              ),
            ),
          ),
          body: ValueListenableBuilder<List<String>>(
            valueListenable: _lines,
            builder: (_, List<String> ls, __) => ListView.builder(
              padding: const EdgeInsets.all(12),
              itemCount: ls.length,
              itemBuilder: (BuildContext _, int i) {
                final String l = ls[i];
                final bool fail = l.startsWith('FAIL');
                final bool pass = l.startsWith('PASS');
                return Padding(
                  padding: const EdgeInsets.symmetric(vertical: 3),
                  child: Text(
                    l,
                    style: TextStyle(
                      fontFamily: 'monospace',
                      fontSize: 12,
                      color: fail
                          ? Colors.redAccent
                          : pass
                              ? Colors.greenAccent
                              : Colors.white70,
                    ),
                  ),
                );
              },
            ),
          ),
        ),
      );
}

final List<(String, bool, String)> _results = <(String, bool, String)>[];

/// Report a line. `stdout` reaches the console on desktop but is not wired to
/// logcat on Android, where only print()/debugPrint surface in `flutter run` —
/// so route everything through print and tag it for easy filtering.
// ignore: avoid_print — this IS the tool's output
void _out(String line) {
  // Split first: a multi-line string would leave continuation lines untagged
  // in logcat and impossible to filter.
  for (final String l in line.split('\n')) {
    // ignore: avoid_print
    print('[probe] $l');
  }
  _lines.value = <String>[..._lines.value, line];
}

void _check(String label, bool ok, [String detail = '']) {
  _results.add((label, ok, detail));
  _out('${ok ? "PASS" : "FAIL"}  $label${detail.isEmpty ? "" : "  $detail"}');
}

Future<void> _run() async {
  _out('\n=== MagNET BLE hardware pass ===');
  try {
    // 0. Android gates BLE behind runtime permissions; without them a scan
    //    returns nothing and gives no error, which looks exactly like "no
    //    node in range". macOS/iOS need none of this.
    if (Platform.isAndroid) {
      final Map<Permission, PermissionStatus> r =
          await <Permission>[
        Permission.bluetoothScan,
        Permission.bluetoothConnect,
      ].request();
      final bool granted = (r[Permission.bluetoothScan]?.isGranted ?? false) &&
          (r[Permission.bluetoothConnect]?.isGranted ?? false);
      _check('android BLE permissions granted', granted,
          r.entries.map((MapEntry<Permission, PermissionStatus> e) =>
              '${e.key.toString().split('.').last}=${e.value.name}').join(' '));
      if (!granted) {
        _out('  (grant them on the device, then re-run)');
        return _summary();
      }
      // Android 12+ still ties BLE scanning to Location *Services* unless the
      // app declares neverForLocation. Granting the permission is not enough:
      // with the system toggle off, a scan returns zero results and no error.
      // With neverForLocation declared on BLUETOOTH_SCAN, BLE results no
      // longer depend on location at all. Reported for context only — the
      // WiFi features still use it.
      final ServiceStatus loc = await Permission.location.serviceStatus;
      _out('  (location services: ${loc.name} — not required for BLE here)');
    }

    // 1. adapter
    if (await FlutterBluePlus.isSupported == false) {
      _check('bluetooth supported', false, 'no adapter');
      return _summary();
    }
    final BluetoothAdapterState st =
        await FlutterBluePlus.adapterState.firstWhere(
            (BluetoothAdapterState s) => s != BluetoothAdapterState.unknown);
    _check('adapter on', st == BluetoothAdapterState.on, '$st');
    if (st != BluetoothAdapterState.on) return _summary();

    // 2. scan for a node
    //
    // Android throttles an app to ~5 scan starts per 30 s. Past that the OS
    // returns an EMPTY result set with no error at all — indistinguishable
    // from an empty room, and easy to hit when re-running a probe like this.
    // So: if a scan sees literally nothing, wait out the window and try once
    // more before believing it.
    _out('\n--- scanning for a MagNET node ---');
    BluetoothDevice? node;
    String advName = '';
    final Set<String> allSeen = <String>{};
    final List<String> namedSeen = <String>[];
    final StreamSubscription<List<ScanResult>> sub =
        FlutterBluePlus.scanResults.listen((List<ScanResult> rs) {
      for (final ScanResult r in rs) {
        final String n = r.advertisementData.advName.isNotEmpty
            ? r.advertisementData.advName
            : r.device.platformName;
        if (allSeen.add(r.device.remoteId.str) && n.isNotEmpty) {
          namedSeen.add(n);
        }
        final List<String> uuids = r.advertisementData.serviceUuids
            .map((Guid g) => g.str.toLowerCase())
            .toList();
        if (node == null && MagnetUuids.looksLikeNode(name: n, serviceUuids: uuids)) {
          node = r.device;
          advName = n;
          _out('  found "$n"  ${r.device.remoteId.str}  '
              'rssi ${r.rssi}  services=$uuids');
        }
      }
    });
    Future<void> scanOnce(int secs) async {
      await FlutterBluePlus.startScan(timeout: Duration(seconds: secs));
      await FlutterBluePlus.isScanning.where((bool s) => s == false).first;
    }

    await scanOnce(12);
    if (allSeen.isEmpty && Platform.isAndroid) {
      _out('  0 devices — likely Android scan throttling; waiting 35s '
          'for the window to clear, then retrying once');
      await Future<void>.delayed(const Duration(seconds: 35));
      await scanOnce(15);
    }
    await sub.cancel();

    // Distinguish "the radio is scanning but our node is silent" from "the
    // scan itself returned nothing", which are very different faults.
    _check('scan returns results', allSeen.isNotEmpty,
        '${allSeen.length} device(s) in range');
    if (namedSeen.isNotEmpty) {
      _out('  named devices: ${namedSeen.take(8).join(", ")}');
    }
    _check('node discovered', node != null,
        node == null
            ? (allSeen.isEmpty
                ? 'scan saw nothing at all — check the system bluetooth log'
                : 'other devices visible but no MagNET-* node — is one unprovisioned and in range?')
            : advName);
    if (node == null) return _summary();

    // 3. connect + discover + subscribe (the whole risky path)
    final MagnetBleTransport t = MagnetBleTransport(node!);
    try {
      await t.connect();
      _check('connect + HCP service discovered', true, MagnetUuids.service);
    } catch (e) {
      _check('connect + HCP service discovered', false, '$e');
      return _summary();
    }

    final HcpClient hcp = HcpClient(t);
    final List<String> traffic = <String>[];
    hcp.traffic.listen(traffic.add);

    // 4. the round trip that proves framing over GATT
    try {
      final Map<String, String> s = await hcp.status();
      _check('STATUS round trip', s.containsKey('state'), '$s');
      _check('node is READY', s['state'] == 'READY', s['state'] ?? '?');
    } catch (e) {
      _check('STATUS round trip', false, '$e');
    }

    try {
      final Map<String, String> w = await hcp.whoami();
      _check('WHOAMI parses', w.containsKey('id'), '$w');
    } catch (e) {
      _check('WHOAMI parses', false, '$e');
    }

    // 5. a long response — exercises notification chunking + reassembly
    try {
      final Map<String, String> c = await hcp.caps();
      _check('CAPS reassembles across notifications',
          (c['verbs'] ?? '').contains('CHANNEL'),
          '${c['verbs']?.length ?? 0} chars of verbs');
    } catch (e) {
      _check('CAPS reassembles across notifications', false, '$e');
    }

    // 6. channel info, and confirm no secret leaks over the link
    try {
      final Map<String, String> ch = await hcp.channel();
      _check('CHANNEL SHOW', ch.containsKey('selector'), '$ch');
      final String all = traffic.join(' ').toLowerCase();
      _check('no secret material on the wire',
          !all.contains('root') && !all.contains('idkey'), '');
    } catch (e) {
      _check('CHANNEL SHOW', false, '$e');
    }

    // 7. an error path — the client must surface E_* not hang
    try {
      await hcp.command('BOGUS');
      _check('unknown verb raises', false, 'no error raised');
    } on HcpError catch (e) {
      _check('unknown verb raises HcpError', e.code == 'E_UNKNOWN_VERB', e.code);
    } catch (e) {
      _check('unknown verb raises HcpError', false, '$e');
    }

    // 8. bonded write — NAME changes config, so it needs the encrypted link
    try {
      await hcp.setName('probe');
      final Map<String, String> w = await hcp.whoami();
      _check('privileged verb (NAME) applied', w['name'] == 'probe', '${w['name']}');
    } on HcpError catch (e) {
      // Unbonded, this is the *correct* answer — a clean, actionable code
      // rather than an ATT error the host cannot interpret.
      _check('privileged verb gated cleanly', e.code == 'E_NOT_BONDED', e.code);
    } catch (e) {
      _check('privileged verb (NAME)', false, '$e');
    }

    _out('\n--- traffic sample ---');
    for (final String l in traffic.take(14)) {
      _out('  $l');
    }

    await hcp.dispose();
    await t.dispose();
  } catch (e, s) {
    _out('probe aborted: $e\n$s');
  }
  _summary();
}

void _summary() {
  final int pass = _results.where(((String, bool, String) r) => r.$2).length;
  _out('\n=== $pass/${_results.length} checks passed ===');
  for (final (String label, bool ok, String d) in _results) {
    if (!ok) _out('  FAILED: $label  $d');
  }
  _out('(probe complete)');
  _busy.value = false;
}
