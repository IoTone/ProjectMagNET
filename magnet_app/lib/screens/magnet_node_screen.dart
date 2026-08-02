// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// Connect to a MagNET node over BLE and configure it.
///
/// A node advertises its HCP service only while unprovisioned, so anything
/// that shows up here is waiting to be given a channel. Setting one is the
/// last thing this screen can do: the firmware tears BLE down immediately
/// afterwards and the node disappears from the air (by design — §11.2.1).
library;
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../magnet/hcp.dart';
import '../magnet/magnet_ble.dart';
import '../magnet/operator_identity.dart';

class MagnetNodeScreen extends StatefulWidget {
  const MagnetNodeScreen({super.key, required this.deviceId, this.deviceName});

  final String deviceId;
  final String? deviceName;

  @override
  State<MagnetNodeScreen> createState() => _MagnetNodeScreenState();
}

class _MagnetNodeScreenState extends State<MagnetNodeScreen> {
  MagnetBleTransport? _transport;
  HcpClient? _hcp;
  StreamSubscription<String>? _trafficSub;

  final List<String> _console = <String>[];
  final TextEditingController _credCtl = TextEditingController();
  final TextEditingController _nameCtl = TextEditingController();

  bool _connecting = true;
  String? _error;
  Map<String, String> _status = <String, String>{};
  Map<String, String> _channel = <String, String>{};
  Map<String, String> _who = <String, String>{};
  OperatorIdentity? _operator;
  int _adminCount = -1;          // -1 = not read yet

  @override
  void initState() {
    super.initState();
    _connect();
  }

  @override
  void dispose() {
    _trafficSub?.cancel();
    _hcp?.dispose();
    _transport?.dispose();
    _credCtl.dispose();
    _nameCtl.dispose();
    super.dispose();
  }

  void _log(String line) {
    if (!mounted) return;
    setState(() {
      _console.add(line);
      if (_console.length > 300) _console.removeRange(0, _console.length - 300);
    });
  }

  Future<void> _connect() async {
    setState(() {
      _connecting = true;
      _error = null;
    });
    try {
      final BluetoothDevice dev = BluetoothDevice.fromId(widget.deviceId);
      final MagnetBleTransport t = MagnetBleTransport(dev);
      await t.connect();
      final HcpClient c = HcpClient(t);
      _trafficSub = c.traffic.listen(_log);
      _transport = t;
      _hcp = c;
      // The operator key is this phone's claim to administer the fleet; it is
      // created on first use and lives in the keystore from then on.
      _operator = await OperatorIdentity.loadOrCreate();
      await _refresh();
    } catch (e) {
      if (mounted) setState(() => _error = '$e');
    } finally {
      if (mounted) setState(() => _connecting = false);
    }
  }

  Future<void> _refresh() async {
    final HcpClient? c = _hcp;
    if (c == null) return;
    try {
      final Map<String, String> s = await c.status();
      final Map<String, String> w = await c.whoami();
      final Map<String, String> ch = await c.channel();
      if (!mounted) return;
      setState(() {
        _status = s;
        _who = w;
        _channel = ch;
        _nameCtl.text = w['name'] == '-' ? '' : (w['name'] ?? '');
      });
      await _readAdmins();
    } catch (e) {
      _log('! $e');
    }
  }

  /// Count the keys on the node's allow-list. An empty list means nobody can
  /// issue privileged commands to this node — safe, but also unmanageable.
  Future<void> _readAdmins() async {
    final HcpClient? c = _hcp;
    if (c == null) return;
    try {
      await c.command('ADMIN LIST');
      final int n =
          c.lastComments.where((String l) => l.startsWith('admin key')).length;
      if (mounted) setState(() => _adminCount = n);
    } catch (_) {
      if (mounted) setState(() => _adminCount = -1);
    }
  }

  Future<void> _enrollOperator() async {
    final OperatorIdentity? op = _operator;
    if (op == null) return;
    if (!await _ensureBonded()) return;
    await _guard(() async {
      await _hcp!.command(op.adminAddCommand);
      await _readAdmins();
      _snack('this phone is now an admin of ${op.fingerprint}');
    }, 'ADMIN ADD');
  }

  Future<void> _guard(Future<void> Function() action, String label) async {
    try {
      await action();
    } on HcpError catch (e) {
      _snack('$label failed: ${e.code}');
    } catch (e) {
      _snack('$label failed: $e');
    }
  }

  void _snack(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context)
        .showSnackBar(SnackBar(content: Text(msg)));
  }

  /// Ensure the link is bonded before a privileged verb.
  ///
  /// Blocks on the platform's own bond state. There is deliberately no
  /// in-app "pair" button: only the OS prompt can complete a bond, and
  /// offering a substitute makes users answer the wrong thing and silently
  /// leaves the real request unanswered.
  Future<bool> _ensureBonded() async {
    final MagnetBleTransport? t = _transport;
    if (t == null) return false;

    bool done = false;
    BondOutcome? outcome;

    unawaited(t.bond().then((BondOutcome o) {
      outcome = o;
      done = true;
      if (mounted) Navigator.of(context, rootNavigator: true).maybePop();
    }));

    // Give the platform a moment; if it bonds instantly there is no need to
    // put a dialog in the user's face at all.
    await Future<void>.delayed(const Duration(milliseconds: 400));
    if (!done && mounted) {
      await showDialog<void>(
        context: context,
        barrierDismissible: false,
        builder: (BuildContext ctx) => PopScope(
          canPop: false,
          child: AlertDialog(
            title: const Text('Accept the pairing request'),
            content: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                const Text(
                  'Your phone is showing a Bluetooth pairing request for this '
                  'node — it may appear as a notification. Accept it to '
                  'continue.',
                ),
                const SizedBox(height: 16),
                StreamBuilder<BluetoothBondState>(
                  stream: t.bondState,
                  builder: (_, AsyncSnapshot<BluetoothBondState> snap) {
                    final String s = switch (snap.data) {
                      BluetoothBondState.bonded => 'Paired',
                      BluetoothBondState.bonding => 'Waiting for you to accept…',
                      _ => 'Requesting…',
                    };
                    return Row(
                      children: <Widget>[
                        const SizedBox(
                            width: 16,
                            height: 16,
                            child: CircularProgressIndicator(strokeWidth: 2)),
                        const SizedBox(width: 12),
                        Text(s),
                      ],
                    );
                  },
                ),
              ],
            ),
            actions: <Widget>[
              TextButton(
                onPressed: () => Navigator.pop(ctx),
                child: const Text('Cancel'),
              ),
            ],
          ),
        ),
      );
    }

    switch (outcome) {
      case BondOutcome.bonded:
      case BondOutcome.alreadyBonded:
        return true;
      case BondOutcome.declined:
        _snack('Pairing was declined — the node cannot be configured without it');
        return false;
      case BondOutcome.timedOut:
        _snack('No answer to the pairing request. Try again and accept the '
            'prompt on your phone.');
        return false;
      case BondOutcome.unsupported:
        _snack('This node firmware does not support pairing');
        return false;
      case null:
        return false;   // cancelled while still in flight
    }
  }

  Future<void> _setName() async {
    final String n = _nameCtl.text.trim();
    if (n.isEmpty) return;
    if (!await _ensureBonded()) return;
    await _guard(() async {
      await _hcp!.setName(n);
      await _refresh();
      _snack('name set to $n');
    }, 'NAME');
  }

  Future<void> _provision() async {
    final String cred = _credCtl.text.trim();
    if (cred.length < 4) {
      _snack('credential must be at least 4 characters');
      return;
    }
    final bool? go = await showDialog<bool>(
      context: context,
      builder: (BuildContext ctx) => AlertDialog(
        title: const Text('Provision this node?'),
        content: const Text(
          'The node will derive its channel key, then shut its Bluetooth '
          'radio down and hand the antenna to the mesh.\n\n'
          'It will disappear from this app. To provision it again you must '
          'clear its stored settings.',
        ),
        actions: <Widget>[
          TextButton(
              onPressed: () => Navigator.pop(ctx, false),
              child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(ctx, true),
              child: const Text('Provision')),
        ],
      ),
    );
    if (go != true) return;

    // Enrol the operator key FIRST. CHANNEL SET tears the node's BLE stack
    // down, so anything not done before it cannot be done at all over this
    // link — including handing the node its admin key.
    final OperatorIdentity? op = _operator;
    if (!await _ensureBonded()) return;
    if (op != null && _adminCount <= 0) {
      try {
        await _hcp!.command(op.adminAddCommand);
        await _readAdmins();
        _log('# operator ${op.fingerprint} enrolled as admin');
      } catch (e) {
        _log('! could not enrol operator key: $e');
      }
    }

    _snack('deriving key — a typed passphrase takes a few seconds…');
    await _guard(() async {
      final Map<String, String> r = await _hcp!.setChannel(cred);
      _log('# provisioned: $r');
      if (!mounted) return;
      await showDialog<void>(
        context: context,
        builder: (BuildContext ctx) => AlertDialog(
          title: const Text('Provisioned'),
          content: Text(
            'Channel selector ${r['selector'] ?? '?'} '
            '(derivation path ${r['path'] ?? '?'}).\n\n'
            'Bluetooth is now off on this node; it is meshing on Thread.',
          ),
          actions: <Widget>[
            FilledButton(
                onPressed: () => Navigator.pop(ctx),
                child: const Text('Done')),
          ],
        ),
      );
      if (mounted) Navigator.of(context).pop();
    }, 'CHANNEL SET');
  }

  @override
  Widget build(BuildContext context) {
    final ColorScheme cs = Theme.of(context).colorScheme;
    final String title = widget.deviceName?.isNotEmpty == true
        ? widget.deviceName!
        : widget.deviceId;

    return Scaffold(
      appBar: AppBar(
        title: Text(title),
        actions: <Widget>[
          IconButton(
            tooltip: 'Refresh',
            onPressed: _hcp == null ? null : _refresh,
            icon: const Icon(Icons.refresh),
          ),
        ],
      ),
      body: _connecting
          ? const Center(child: CircularProgressIndicator())
          : _error != null
              ? _ErrorView(error: _error!, onRetry: _connect)
              : ListView(
                  padding: const EdgeInsets.all(16),
                  children: <Widget>[
                    _StatusCard(status: _status, who: _who, channel: _channel, cs: cs),
                    const SizedBox(height: 16),
                    _Section('Display name'),
                    Row(
                      children: <Widget>[
                        Expanded(
                          child: TextField(
                            controller: _nameCtl,
                            maxLength: 16,
                            decoration: const InputDecoration(
                              hintText: 'e.g. kitchen',
                              counterText: '',
                              border: OutlineInputBorder(),
                            ),
                          ),
                        ),
                        const SizedBox(width: 8),
                        FilledButton(onPressed: _setName, child: const Text('Set')),
                      ],
                    ),
                    const SizedBox(height: 20),
                    _Section('Fleet admin'),
                    _AdminCard(
                      fingerprint: _operator?.fingerprint,
                      adminCount: _adminCount,
                      onEnroll: _enrollOperator,
                      cs: cs,
                    ),
                    const SizedBox(height: 20),
                    _Section('Channel credential'),
                    Text(
                      'A passphrase, a 12–24 word seed phrase, or a qr: secret. '
                      'Every node sharing this credential — and only those — can '
                      'read the channel.',
                      style: TextStyle(color: cs.onSurfaceVariant, fontSize: 12),
                    ),
                    const SizedBox(height: 8),
                    TextField(
                      controller: _credCtl,
                      minLines: 1,
                      maxLines: 3,
                      decoration: const InputDecoration(
                        hintText: 'correct horse battery staple',
                        border: OutlineInputBorder(),
                      ),
                    ),
                    const SizedBox(height: 10),
                    FilledButton.icon(
                      onPressed: _provision,
                      icon: const Icon(Icons.lock),
                      label: const Text('Provision channel'),
                    ),
                    const SizedBox(height: 24),
                    _Section('Diagnostics'),
                    Wrap(
                      spacing: 8,
                      children: <Widget>[
                        OutlinedButton(
                          onPressed: () => _guard(() async {
                            final bool ok = await _hcp!.selftest();
                            _snack(ok ? 'selftest PASS' : 'selftest FAIL');
                          }, 'SELFTEST'),
                          child: const Text('Selftest'),
                        ),
                        OutlinedButton(
                          onPressed: () => _guard(() async {
                            final Map<String, String> s = await _hcp!.stats();
                            _log('# stats $s');
                          }, 'STATS'),
                          child: const Text('Stats'),
                        ),
                        OutlinedButton(
                          onPressed: () => _guard(() async {
                            final List<Map<String, String>> p =
                                await _hcp!.peers();
                            _log('# peers ${p.length}: $p');
                          }, 'PEERS'),
                          child: const Text('Peers'),
                        ),
                        OutlinedButton(
                          onPressed: () => _guard(() async {
                            final Map<String, String> c = await _hcp!.caps();
                            _log('# caps $c');
                          }, 'CAPS'),
                          child: const Text('Caps'),
                        ),
                      ],
                    ),
                    const SizedBox(height: 20),
                    _Section('Console'),
                    _Console(lines: _console, cs: cs),
                  ],
                ),
    );
  }
}

class _StatusCard extends StatelessWidget {
  const _StatusCard({
    required this.status,
    required this.who,
    required this.channel,
    required this.cs,
  });

  final Map<String, String> status;
  final Map<String, String> who;
  final Map<String, String> channel;
  final ColorScheme cs;

  @override
  Widget build(BuildContext context) {
    final String state = status['state'] ?? '?';
    final bool ready = state == 'READY';
    final bool isDefault = (channel['name'] ?? '') == 'magnet';
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Row(
              children: <Widget>[
                Icon(ready ? Icons.check_circle : Icons.hourglass_top,
                    color: ready ? Colors.green : cs.onSurfaceVariant, size: 20),
                const SizedBox(width: 8),
                Text(state, style: const TextStyle(fontWeight: FontWeight.bold)),
                const Spacer(),
                Text(status['role'] ?? '', style: TextStyle(color: cs.onSurfaceVariant)),
              ],
            ),
            const SizedBox(height: 10),
            _kv('Device id', who['id'] ?? '?'),
            _kv('Firmware', who['fw'] ?? '?'),
            _kv('Channel', channel['name'] ?? '?'),
            _kv('Selector', channel['selector'] ?? '?'),
            _kv('Peers', status['peers'] ?? '0'),
            if (isDefault) ...<Widget>[
              const SizedBox(height: 10),
              Row(
                children: <Widget>[
                  const Icon(Icons.warning_amber, color: Colors.amber, size: 18),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      'On the well-known default channel — anyone nearby can '
                      'read its traffic. Provision a credential below.',
                      style: TextStyle(fontSize: 12, color: cs.onSurfaceVariant),
                    ),
                  ),
                ],
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _kv(String k, String v) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 2),
        child: Row(
          children: <Widget>[
            SizedBox(width: 96, child: Text(k, style: const TextStyle(fontSize: 12))),
            Expanded(
              child: Text(v,
                  style: const TextStyle(
                      fontSize: 12, fontFamily: 'monospace')),
            ),
          ],
        ),
      );
}

class _AdminCard extends StatelessWidget {
  const _AdminCard({
    required this.fingerprint,
    required this.adminCount,
    required this.onEnroll,
    required this.cs,
  });

  final String? fingerprint;
  final int adminCount;
  final VoidCallback onEnroll;
  final ColorScheme cs;

  @override
  Widget build(BuildContext context) {
    final bool none = adminCount == 0;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Row(
              children: <Widget>[
                Icon(none ? Icons.gpp_maybe : Icons.verified_user,
                    size: 20,
                    color: none ? Colors.amber : Colors.green),
                const SizedBox(width: 8),
                Text(
                  adminCount < 0
                      ? 'Allow-list unknown'
                      : none
                          ? 'No admin keys'
                              : '$adminCount admin key${adminCount == 1 ? "" : "s"}',
                  style: const TextStyle(fontWeight: FontWeight.bold),
                ),
              ],
            ),
            const SizedBox(height: 8),
            Text(
              none
                  ? 'Nobody can send this node a privileged command — rotating '
                      'its keys or moving it to another channel would need a '
                      'cable. Enrol this phone to manage it over the mesh.'
                  : 'Privileged commands are only executed if signed by a key '
                      'on this list.',
              style: TextStyle(fontSize: 12, color: cs.onSurfaceVariant),
            ),
            const SizedBox(height: 10),
            Row(
              children: <Widget>[
                Expanded(
                  child: Text(
                    'This phone: ${fingerprint ?? "…"}',
                    style: const TextStyle(
                        fontFamily: 'monospace', fontSize: 12),
                  ),
                ),
                FilledButton.tonal(
                  onPressed: fingerprint == null ? null : onEnroll,
                  child: const Text('Enrol'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _Section extends StatelessWidget {
  const _Section(this.title);
  final String title;
  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.only(bottom: 8),
        child: Text(title,
            style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
      );
}

class _Console extends StatelessWidget {
  const _Console({required this.lines, required this.cs});
  final List<String> lines;
  final ColorScheme cs;

  @override
  Widget build(BuildContext context) => Container(
        height: 200,
        padding: const EdgeInsets.all(8),
        decoration: BoxDecoration(
          color: cs.surfaceContainerHighest,
          borderRadius: BorderRadius.circular(8),
        ),
        child: ListView.builder(
          reverse: true,
          itemCount: lines.length,
          itemBuilder: (BuildContext ctx, int i) {
            final String line = lines[lines.length - 1 - i];
            Color? c;
            if (line.startsWith('-')) c = Colors.redAccent;
            if (line.startsWith('!')) c = Colors.orangeAccent;
            if (line.startsWith('>')) c = cs.primary;
            return Text(line,
                style: TextStyle(
                    fontFamily: 'monospace', fontSize: 11, color: c));
          },
        ),
      );
}

class _ErrorView extends StatelessWidget {
  const _ErrorView({required this.error, required this.onRetry});
  final String error;
  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) => Center(
        child: Padding(
          padding: const EdgeInsets.all(28),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              const Icon(Icons.bluetooth_disabled, size: 40),
              const SizedBox(height: 12),
              Text(error, textAlign: TextAlign.center),
              const SizedBox(height: 16),
              FilledButton(onPressed: onRetry, child: const Text('Retry')),
            ],
          ),
        ),
      );
}
