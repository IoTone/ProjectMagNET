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
import 'package:flutter/services.dart' show Clipboard, ClipboardData;
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:qr_flutter/qr_flutter.dart';

import '../magnet/credentials.dart';
import '../magnet/hcp.dart';
import '../magnet/magnet_ble.dart';
import '../magnet/multisense.dart';
import '../magnet/operator_identity.dart';

class MagnetNodeScreen extends StatefulWidget {
  const MagnetNodeScreen({super.key, required this.deviceId, this.deviceName});

  final String deviceId;
  final String? deviceName;

  @override
  State<MagnetNodeScreen> createState() => _MagnetNodeScreenState();
}

/// Where the BLE link stands. The screen must show this: a node that reboots
/// — because it was reflashed, or because the battery took over from USB —
/// drops the link, and until this was rendered the screen simply kept its
/// stale values and every button failed silently.
enum _LinkState { connecting, live, reconnecting, lost }

class _MagnetNodeScreenState extends State<MagnetNodeScreen> {
  /// Retry the same node for this long after the link drops. A reboot is back
  /// within a couple of seconds; a power-source changeover plus sensor
  /// bring-up takes longer, and a node carried out of range may well come
  /// back. Matches `MeshSession.reconnectWindow`.
  static const Duration _reconnectWindow = Duration(minutes: 3);
  static const List<Duration> _reconnectDelays = <Duration>[
    Duration(seconds: 1),
    Duration(seconds: 2),
    Duration(seconds: 3),
    Duration(seconds: 5),
  ];

  /// How often to re-read `SENSE` while the link is live.
  static const Duration _sensePeriod = Duration(seconds: 5);

  MagnetBleTransport? _transport;
  HcpClient? _hcp;
  StreamSubscription<String>? _trafficSub;
  StreamSubscription<BluetoothConnectionState>? _connSub;

  _LinkState _link = _LinkState.connecting;
  int _reconnectGen = 0; // bumped to cancel a running reconnect loop
  int _reconnects = 0;
  Timer? _senseTimer;

  /// Latest `SENSE` reply, or null when this node has no sensors to report.
  MultisenseReadings? _sense;

  /// What the node found at boot, from `CAPS`. Read once per connect — a
  /// sensor cannot appear on the bus without a reboot, so polling it is waste.
  MsSensorSet _detected = MsSensorSet.unknown;

  /// False once the node answers `E_UNKNOWN` to `SENSE` — firmware without
  /// the verb, so stop asking and hide the card.
  bool _senseSupported = true;

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
    // Live path/entropy hint under the credential field.
    _credCtl.addListener(() {
      if (mounted) setState(() {});
    });
    _connect();
  }

  @override
  void dispose() {
    _reconnectGen++; // stop any retry loop before the state object goes away
    _senseTimer?.cancel();
    _connSub?.cancel();
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

  /// Connect by hand: the first attempt, and the Retry/Reconnect buttons.
  /// Cancels any automatic retry in flight so the two never race.
  Future<void> _connect() async {
    _reconnectGen++;
    await _teardown();
    setState(() {
      _connecting = true;
      _link = _LinkState.connecting;
      _error = null;
    });
    try {
      await _attach();
    } catch (e) {
      if (mounted) {
        setState(() {
          _error = '$e';
          _link = _LinkState.lost;
        });
      }
    } finally {
      if (mounted) setState(() => _connecting = false);
    }
  }

  /// Open the link and wire everything that depends on it. Throws if the node
  /// does not answer; the caller decides whether to retry or show the error.
  Future<void> _attach() async {
    final BluetoothDevice dev = BluetoothDevice.fromId(widget.deviceId);
    final MagnetBleTransport t = MagnetBleTransport(dev);
    await t.connect();
    final HcpClient c = HcpClient(t);
    _trafficSub = c.traffic.listen(_log);
    _transport = t;
    _hcp = c;

    // Watch the platform's own view of the link. This is the only thing that
    // notices a node that went away on its own — HCP commands would just time
    // out, one at a time, with nothing on screen to say why.
    _connSub = dev.connectionState.listen((BluetoothConnectionState s) {
      if (s == BluetoothConnectionState.disconnected &&
          _link == _LinkState.live) {
        _onLinkLost();
      }
    });

    // The operator key is this phone's claim to administer the fleet; it is
    // created on first use and lives in the keystore from then on.
    _operator ??= await OperatorIdentity.loadOrCreate();
    if (mounted) setState(() => _link = _LinkState.live);
    await _refresh();
    _startSensePoll();
  }

  /// Drop the current link and everything hanging off it, leaving the screen
  /// ready for a fresh [_attach].
  Future<void> _teardown() async {
    _senseTimer?.cancel();
    _senseTimer = null;
    await _connSub?.cancel();
    _connSub = null;
    await _trafficSub?.cancel();
    _trafficSub = null;
    final HcpClient? c = _hcp;
    final MagnetBleTransport? t = _transport;
    _hcp = null;
    _transport = null;
    await c?.dispose();
    await t?.dispose();
  }

  void _onLinkLost() {
    _log('! link lost — reconnecting');
    if (mounted) setState(() => _link = _LinkState.reconnecting);
    unawaited(_reconnectLoop());
  }

  /// Retry the same node until [_reconnectWindow] runs out.
  ///
  /// The node keeps its BLE address across a reboot (it advertises the public
  /// MAC), so the device id this screen was opened with stays valid and there
  /// is nothing to re-discover.
  Future<void> _reconnectLoop() async {
    final int gen = ++_reconnectGen;
    final DateTime deadline = DateTime.now().add(_reconnectWindow);
    await _teardown();
    for (int attempt = 0; DateTime.now().isBefore(deadline); attempt++) {
      final Duration wait = _reconnectDelays[
          attempt < _reconnectDelays.length ? attempt : _reconnectDelays.length - 1];
      await Future<void>.delayed(wait);
      if (gen != _reconnectGen || !mounted) return;
      try {
        await _attach();
        _reconnects++;
        _log('# reconnected (${_reconnects}x this session)');
        if (mounted) setState(() => _error = null);
        return;
      } catch (_) {
        await _teardown();
        if (gen != _reconnectGen || !mounted) return;
      }
    }
    if (gen != _reconnectGen || !mounted) return;
    _log('! gave up reconnecting');
    setState(() {
      _link = _LinkState.lost;
      _error = 'link lost — the node did not come back';
    });
  }

  /// Poll `SENSE` while the link is live. Readings are the point of a
  /// multisensor node, so they refresh on their own rather than waiting for
  /// someone to press Refresh.
  void _startSensePoll() {
    _senseTimer?.cancel();
    if (!_senseSupported) return;
    _senseTimer = Timer.periodic(_sensePeriod, (_) {
      if (_link == _LinkState.live) unawaited(_readSense());
    });
  }

  /// Read the node's sensors. An `E_UNKNOWN` here is not an error: it means
  /// this firmware has no sensors to report, so stop asking.
  Future<void> _readSense() async {
    final HcpClient? c = _hcp;
    if (c == null || !_senseSupported) return;
    try {
      final MultisenseReadings r = MultisenseReadings.fromSense(await c.sense());
      if (mounted) setState(() => _sense = r);
    } on HcpError catch (e) {
      if (e.code == 'E_UNKNOWN') {
        _senseTimer?.cancel();
        _senseTimer = null;
        if (mounted) {
          setState(() {
            _senseSupported = false;
            _sense = null;
          });
        }
      }
    } catch (_) {
      // A timeout here is the link going; the connection watcher handles it.
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
      await _readSense();
      // Which sensors the node actually detected. This is what tells a blank
      // Light row apart from a missing OPT4048.
      try {
        _detected = MsSensorSet.fromCaps(await c.caps());
        if (mounted) setState(() {});
      } on HcpError catch (_) {
        // Firmware without CAPS: leave it unknown rather than claim nothing.
      }
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

  /// Run a privileged action, repairing a stale bond if the node rejects it.
  ///
  /// `E_NOT_BONDED` on a link the *platform* calls bonded means the node forgot
  /// us — factory-reset or reflashed while this phone kept its half. The BLE
  /// address is unchanged, so Android sees no new device and never re-prompts;
  /// left alone this fails forever. Drop our half, pair again, retry once.
  /// See `docs/BLE-PAIRING.md`.
  Future<void> _guard(Future<void> Function() action, String label,
      {bool recoverStaleBond = true}) async {
    try {
      await action();
    } on HcpError catch (e) {
      final MagnetBleTransport? t = _transport;
      if (e.code == 'E_NOT_BONDED' && recoverStaleBond && t != null) {
        _snack('This node no longer recognises this phone — re-pairing');
        final BondOutcome o = await t.recoverStaleBondAndRebond();
        if (o == BondOutcome.bonded) {
          return _guard(action, label, recoverStaleBond: false);
        }
        _snack('$label failed: could not re-pair (${o.name})');
        return;
      }
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
    final CredInfo credInfo = MagnetCredentials.describe(cred);
    final bool? go = await showDialog<bool>(
      context: context,
      builder: (BuildContext ctx) => AlertDialog(
        title: const Text('Provision this node?'),
        content: Text(
          'The node will derive its channel key, then shut its Bluetooth '
          'radio down and hand the antenna to the mesh.\n\n'
          'It will disappear from this app. To provision it again you must '
          'clear its stored settings.'
          '${credInfo.path == CredPath.passphrase ? '\n\nThis is a TYPED '
              'passphrase (path B, ~${credInfo.bits} bits). Generated '
              'phrases and qr: secrets are much stronger — Cancel and tap '
              'Generate unless you need this exact phrase.' : ''}',
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

      // SCOPE M1: remember what we provisioned, so the fleet is knowable
      // after the node goes dark. The label is derived, NOT the credential —
      // keys don't belong in an app-readable log.
      final CredInfo info = MagnetCredentials.describe(cred);
      final String label = info.path == CredPath.qrSecret
          ? 'qr-channel'
          : cred.split(RegExp(r'\s+')).first;
      await ProvisionLog.add(ProvisionRecord(
        deviceId: widget.deviceId,
        nodeId: _who['id'] ?? '?',
        name: _who['name'] ?? _nameCtl.text.trim(),
        channelLabel: label,
        selector: r['selector'] ?? '?',
        path: r['path'] ?? info.pathLetter,
        at: DateTime.now(),
      ));

      if (!mounted) return;
      await showDialog<void>(
        context: context,
        builder: (BuildContext ctx) => AlertDialog(
          title: const Text('Provisioned'),
          content: SingleChildScrollView(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                Text(
                  'Channel selector ${r['selector'] ?? '?'} '
                  '(derivation path ${r['path'] ?? '?'}).\n\n'
                  'To join the next device to the same channel, scan this '
                  'QR from it — no typing, no wire.',
                ),
                const SizedBox(height: 14),
                _credQrBox(cred),
              ],
            ),
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

  /// White-backed QR of a credential (scanners need the light background in
  /// dark theme too).
  Widget _credQrBox(String cred) => Container(
        color: Colors.white,
        padding: const EdgeInsets.all(8),
        child: QrImageView(data: cred, size: 208),
      );

  Future<void> _showCredentialQr(String cred) => showDialog<void>(
        context: context,
        builder: (BuildContext ctx) => AlertDialog(
          title: const Text('Channel credential'),
          content: SingleChildScrollView(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                _credQrBox(cred),
                const SizedBox(height: 10),
                SelectableText(cred,
                    style: const TextStyle(
                        fontFamily: 'monospace', fontSize: 12)),
                const SizedBox(height: 6),
                Text(
                  'This IS the channel key — share it only with devices '
                  'that belong on the channel.',
                  style: TextStyle(
                      fontSize: 11,
                      color: Theme.of(ctx).colorScheme.error),
                ),
              ],
            ),
          ),
          actions: <Widget>[
            TextButton.icon(
              icon: const Icon(Icons.copy, size: 16),
              label: const Text('Copy'),
              onPressed: () =>
                  Clipboard.setData(ClipboardData(text: cred)),
            ),
            FilledButton(
                onPressed: () => Navigator.pop(ctx),
                child: const Text('Close')),
          ],
        ),
      );

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
            onPressed:
                _hcp == null || _link != _LinkState.live ? null : _refresh,
            icon: const Icon(Icons.refresh),
          ),
        ],
      ),
      body: Column(
        children: <Widget>[
          if (_link != _LinkState.live && _who.isNotEmpty)
            _LinkBanner(
              link: _link,
              reconnects: _reconnects,
              onReconnect: _connect,
              cs: cs,
            ),
          Expanded(child: _body(cs)),
        ],
      ),
    );
  }

  Widget _body(ColorScheme cs) {
    // Only a failure with nothing to show yet replaces the screen. Once the
    // node has answered once, a drop keeps the page — including the console —
    // and the banner above carries the state.
    if (_connecting && _who.isEmpty) {
      return const Center(child: CircularProgressIndicator());
    }
    if (_error != null && _who.isEmpty) {
      return _ErrorView(error: _error!, onRetry: _connect);
    }
    return ListView(
                  padding: const EdgeInsets.all(16),
                  children: <Widget>[
                    _StatusCard(status: _status, who: _who, channel: _channel, cs: cs),
                    if (_sense != null) ...<Widget>[
                      const SizedBox(height: 16),
                      _Section('Sensors'),
                      _SensorsCard(
                          readings: _sense!,
                          detected: _detected,
                          stale: _link != _LinkState.live,
                          cs: cs),
                    ],
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
                        hintText: 'tap Generate — typing one is the fallback',
                        border: OutlineInputBorder(),
                      ),
                    ),
                    const SizedBox(height: 8),
                    Wrap(
                      spacing: 8,
                      runSpacing: 4,
                      children: <Widget>[
                        OutlinedButton.icon(
                          onPressed: () => setState(() => _credCtl.text =
                              MagnetCredentials.generateSeedPhrase()),
                          icon: const Icon(Icons.casino_outlined, size: 18),
                          label: const Text('Generate phrase'),
                        ),
                        OutlinedButton.icon(
                          onPressed: () => setState(() => _credCtl.text =
                              MagnetCredentials.generateQrSecret()),
                          icon: const Icon(Icons.vpn_key_outlined, size: 18),
                          label: const Text('Generate secret'),
                        ),
                        OutlinedButton.icon(
                          onPressed: _credCtl.text.trim().isEmpty
                              ? null
                              : () => _showCredentialQr(_credCtl.text.trim()),
                          icon: const Icon(Icons.qr_code_2, size: 18),
                          label: const Text('Show QR'),
                        ),
                      ],
                    ),
                    if (_credCtl.text.trim().isNotEmpty) ...<Widget>[
                      const SizedBox(height: 6),
                      _CredHint(
                          info:
                              MagnetCredentials.describe(_credCtl.text),
                          cs: cs),
                    ],
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
                );
  }
}

/// The link's state, across the top of the screen.
///
/// The node reboots for ordinary reasons — a reflash, or the battery taking
/// over when USB is unplugged — and each one drops the BLE link. Without this
/// the screen kept showing the last values it read and every command failed
/// into a snackbar, which reads as the app being broken rather than the node
/// having restarted.
class _LinkBanner extends StatelessWidget {
  const _LinkBanner({
    required this.link,
    required this.reconnects,
    required this.onReconnect,
    required this.cs,
  });

  final _LinkState link;
  final int reconnects;
  final VoidCallback onReconnect;
  final ColorScheme cs;

  @override
  Widget build(BuildContext context) {
    final (String text, Color bg, Color fg, bool busy) = switch (link) {
      _LinkState.connecting => ('Connecting…', cs.surfaceContainerHighest, cs.onSurface, true),
      _LinkState.reconnecting => (
          'Node went away — reconnecting…'
              '${reconnects > 0 ? '  (came back ${reconnects}x already)' : ''}',
          cs.tertiaryContainer,
          cs.onTertiaryContainer,
          true
        ),
      _LinkState.lost => (
          'Disconnected. The node may be rebooting or out of range.',
          cs.errorContainer,
          cs.onErrorContainer,
          false
        ),
      _LinkState.live => ('', cs.surface, cs.onSurface, false),
    };
    return Material(
      color: bg,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 10, 8, 10),
        child: Row(
          children: <Widget>[
            if (busy)
              SizedBox(
                  width: 14,
                  height: 14,
                  child: CircularProgressIndicator(strokeWidth: 2, color: fg))
            else
              Icon(Icons.bluetooth_disabled, size: 16, color: fg),
            const SizedBox(width: 10),
            Expanded(
              child: Text(text, style: TextStyle(fontSize: 12, color: fg)),
            ),
            if (link == _LinkState.lost)
              TextButton(
                onPressed: onReconnect,
                child: const Text('Reconnect'),
              ),
          ],
        ),
      ),
    );
  }
}

/// Live readings from a multisensor node (role 15), from HCP `SENSE`.
///
/// Only the sensors the node actually found are listed: the firmware omits a
/// key for anything that did not answer the boot-time bus probe, so a missing
/// row means "not fitted", never "zero".
class _SensorsCard extends StatelessWidget {
  const _SensorsCard({
    required this.readings,
    required this.detected,
    required this.stale,
    required this.cs,
  });

  final MultisenseReadings readings;

  /// What the node reported finding at boot.
  final MsSensorSet detected;

  /// True while the link is down: the numbers are the last ones read, not now.
  final bool stale;
  final ColorScheme cs;

  @override
  Widget build(BuildContext context) {
    final MultisenseReadings r = readings;
    final List<Widget> rows = <Widget>[
      // Light and colour both come from the OPT4048, so a claimed-but-silent
      // part shows as such instead of leaving a hole where a row should be.
      if (r.lux != null)
        _row('Light', '${r.lux!.toStringAsFixed(1)} lx',
            Icons.light_mode_outlined)
      else if (detected.has('lux'))
        _missing('Light', detected.parts['lux']!, Icons.light_mode_outlined),
      if (r.cct != null)
        _row(
          'Colour',
          '${r.cct!.round()} K'
              '${r.cieX != null && r.cieY != null ? '   xy ${r.cieX!.toStringAsFixed(3)}/${r.cieY!.toStringAsFixed(3)}' : ''}',
          Icons.palette_outlined,
        )
      else if (detected.has('cct'))
        _missing('Colour', detected.parts['cct']!, Icons.palette_outlined),
      if (r.yawDeg != null)
        _row(
          'Orientation',
          'yaw ${r.yawDeg!.round()}°  pitch ${r.pitchDeg?.round() ?? '?'}°  '
              'roll ${r.rollDeg?.round() ?? '?'}°',
          Icons.explore_outlined,
        )
      else if (detected.has('orientation'))
        _missing('Orientation', detected.parts['orientation']!,
            Icons.explore_outlined),
      if (r.stability != null)
        _row(
          'Motion',
          '${stabilityLabel(r.stability!)}'
              '${r.sigMotion != null ? '   ${r.sigMotion} events' : ''}',
          Icons.vibration,
        ),
      if (r.socPct == null && detected.has('battery'))
        _missing('Battery', detected.parts['battery']!,
            Icons.battery_std_outlined),
      if (r.socPct != null)
        _row(
          'Battery',
          '${r.socPct}%'
              '${r.vbatVolts != null ? '   ${r.vbatVolts!.toStringAsFixed(3)} V' : ''}'
              '${r.tbatCelsius != null ? '   ${r.tbatCelsius!.toStringAsFixed(1)} °C' : ''}',
          Icons.battery_std_outlined,
        ),
      if (r.timeIso != null)
        _row('Node clock', r.timeIso!, Icons.schedule_outlined),
    ];

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            ...rows,
            const SizedBox(height: 8),
            Text(
              stale
                  ? 'last reading before the link dropped (seq ${r.seq ?? "?"})'
                  : 'updating every 5 s · seq ${r.seq ?? "?"}',
              style: TextStyle(
                  fontSize: 11,
                  color: stale ? cs.error : cs.onSurfaceVariant),
            ),
            if (detected.isKnown) ...<Widget>[
              const SizedBox(height: 4),
              Text(
                'detected at boot: ${detected.parts.keys.join(", ")}',
                style: TextStyle(fontSize: 11, color: cs.onSurfaceVariant),
              ),
              Text(
                'A sensor missing from that list did not answer the node\'s '
                'boot-time bus scan — reseat its Qwiic breakout and reboot.',
                style: TextStyle(fontSize: 11, color: cs.onSurfaceVariant),
              ),
            ],
            if (r.headingAccDeg != null && r.headingAccDeg! > 25) ...<Widget>[
              const SizedBox(height: 6),
              Row(
                children: <Widget>[
                  const Icon(Icons.warning_amber, color: Colors.amber, size: 16),
                  const SizedBox(width: 6),
                  Expanded(
                    child: Text(
                      'Heading accuracy ±${r.headingAccDeg!.round()}° — move '
                      'the node in a figure-8 to calibrate its magnetometer.',
                      style:
                          TextStyle(fontSize: 11, color: cs.onSurfaceVariant),
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

  /// A sensor the node says it found, but that produced no reading. The part
  /// name matters: it is the difference between "reseat the breakout" and
  /// "the driver is returning an error".
  Widget _missing(String label, String part, IconData icon) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 3),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Icon(icon, size: 16, color: cs.error),
            const SizedBox(width: 8),
            SizedBox(
                width: 88,
                child: Text(label,
                    style: TextStyle(fontSize: 12, color: cs.error))),
            Expanded(
              child: Text('no data from $part',
                  style: TextStyle(
                      fontSize: 12,
                      fontFamily: 'monospace',
                      color: cs.error)),
            ),
          ],
        ),
      );

  Widget _row(String label, String value, IconData icon) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 3),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Icon(icon, size: 16, color: cs.onSurfaceVariant),
            const SizedBox(width: 8),
            SizedBox(
                width: 88,
                child: Text(label, style: const TextStyle(fontSize: 12))),
            Expanded(
              child: Text(value,
                  style: const TextStyle(
                      fontSize: 12, fontFamily: 'monospace')),
            ),
          ],
        ),
      );
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
            _buildRow(OtaBuild.fromWhoami(who)),
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

  /// The installed build (WHOAMI `ota=`), flagged while it is on trial or
  /// after a rollback. Older firmware and non-OTA builds show a dash.
  Widget _buildRow(OtaBuild? b) {
    if (b == null) return _kv('Build', '— (no OTA)');
    final String note = b.onTrial
        ? 'on trial — confirms when READY'
        : b.rolledBack
            ? 'update failed — old image restored'
            : '';
    final Color? color = b.onTrial
        ? Colors.amber.shade800
        : b.rolledBack
            ? cs.error
            : null;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          const SizedBox(width: 96, child: Text('Build', style: TextStyle(fontSize: 12))),
          Expanded(
            child: Text(
              '${b.version}  ${b.slot}  ${b.state}${note.isEmpty ? '' : '\n$note'}',
              style: TextStyle(fontSize: 12, fontFamily: 'monospace', color: color),
            ),
          ),
        ],
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

/// Live derivation-path + entropy readout under the credential field.
/// Path B (typed) renders in the error color: it is the fallback, not the path.
class _CredHint extends StatelessWidget {
  const _CredHint({required this.info, required this.cs});
  final CredInfo info;
  final ColorScheme cs;

  @override
  Widget build(BuildContext context) {
    final (String text, Color color) = switch (info.path) {
      CredPath.qrSecret => (
          'path A — 256-bit random secret',
          cs.primary
        ),
      CredPath.seedPhrase => (
          'path C — seed phrase, ~${info.bits} bits if randomly chosen',
          cs.primary
        ),
      CredPath.passphrase => (
          'path B — typed passphrase, ~${info.bits} bits (weak; '
              'prefer Generate)',
          cs.error
        ),
    };
    return Row(children: <Widget>[
      Icon(
          info.path == CredPath.passphrase
              ? Icons.warning_amber
              : Icons.verified_outlined,
          size: 14,
          color: color),
      const SizedBox(width: 6),
      Expanded(
          child:
              Text(text, style: TextStyle(fontSize: 11, color: color))),
    ]);
  }
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
