// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// M4 — field test console (SCOPE.md §M4): everything the bench Python
/// scripts do, in your hand, against the companion node. Selftest with
/// per-stage results, stats with deltas over time, one-tap bench, guarded
/// stress with live progress, and an exportable node report.
///
/// Console text is deliberately unlocalized, matching the node screen: this
/// is the technical surface, and its vocabulary is the protocol's.
library;
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show Clipboard, ClipboardData;
import 'package:provider/provider.dart';

import '../magnet/hcp.dart';
import '../magnet/mesh_session.dart';
import '../scanner/ble_scanner.dart';
import '../scanner/scanner_controller.dart';

class MeshTestTab extends StatefulWidget {
  const MeshTestTab({super.key});

  @override
  State<MeshTestTab> createState() => _MeshTestTabState();
}

class _MeshTestTabState extends State<MeshTestTab>
    with AutomaticKeepAliveClientMixin {
  // Selftest
  Map<String, bool>? _stages;
  bool _selftestPass = false;
  bool _selftestRunning = false;

  // Stats
  Map<String, int>? _stats;
  Map<String, int>? _prevStats;
  DateTime? _statsAt;
  DateTime? _prevStatsAt;

  // Bench
  List<String> _benchLines = <String>[];
  bool _benchRunning = false;

  // Stress
  final TextEditingController _stressSecs =
      TextEditingController(text: '5');
  final TextEditingController _stressLen =
      TextEditingController(text: '62');
  final List<String> _stressLog = <String>[];
  bool _stressRunning = false;
  StreamSubscription<String>? _stressSub;

  // Report
  bool _reportBusy = false;

  @override
  bool get wantKeepAlive => true;

  @override
  void dispose() {
    _stressSub?.cancel();
    _stressSecs.dispose();
    _stressLen.dispose();
    super.dispose();
  }

  void _snack(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg)));
  }

  String _errText(Object e) => switch (e) {
        HcpError(:final String code) when code == 'E_NOT_BONDED' =>
          'E_NOT_BONDED — pair with the companion first (privileged verb)',
        HcpError(:final String code) when code == 'E_BUSY' =>
          'node busy — a previous run is still going',
        HcpError(:final String code) when code == 'E_BAD_STATE' =>
          'node not READY — is the mesh up?',
        HcpTimeout() => 'no response — node busy or out of range',
        StateError() => 'not connected to the companion node',
        _ => '$e',
      };

  // ---- Selftest ----

  Future<void> _runSelftest(MeshSession s) async {
    setState(() {
      _selftestRunning = true;
      _stages = null;
    });
    try {
      final (String r, List<String> comments) =
          await s.run('SELFTEST', timeout: const Duration(seconds: 20));
      final Map<String, bool> stages = <String, bool>{};
      for (final String c in comments) {
        final Match? m =
            RegExp(r'^selftest ([a-z+-]+) (ok|FAIL)').firstMatch(c);
        if (m != null) stages[m.group(1)!] = m.group(2) == 'ok';
      }
      setState(() {
        _stages = stages;
        _selftestPass = r.contains('pass');
      });
    } catch (e) {
      _snack(_errText(e));
    } finally {
      if (mounted) setState(() => _selftestRunning = false);
    }
  }

  // ---- Stats ----

  static Map<String, int> _parseStats(List<String> comments) {
    final Map<String, int> out = <String, int>{};
    for (final String c in comments) {
      if (!c.startsWith('stats ')) continue;
      final String kind = c.split(RegExp(r'\s+'))[1]; // tx | rx
      HcpClient.parseKv(c).forEach((String k, String v) {
        final int? n = int.tryParse(v);
        if (n != null) out['${kind}_$k'] = n;
      });
    }
    return out;
  }

  Future<void> _sampleStats(MeshSession s) async {
    try {
      final (_, List<String> comments) = await s.run('STATS');
      setState(() {
        _prevStats = _stats;
        _prevStatsAt = _statsAt;
        _stats = _parseStats(comments);
        _statsAt = DateTime.now();
      });
    } catch (e) {
      _snack(_errText(e));
    }
  }

  // ---- Bench ----

  Future<void> _runBench(MeshSession s) async {
    setState(() {
      _benchRunning = true;
      _benchLines = <String>[];
    });
    try {
      final (_, List<String> comments) =
          await s.run('BENCH', timeout: const Duration(seconds: 25));
      setState(() => _benchLines = comments
          .where((String c) => c.startsWith('bench'))
          .toList());
    } catch (e) {
      _snack(_errText(e));
    } finally {
      if (mounted) setState(() => _benchRunning = false);
    }
  }

  // ---- Stress ----

  Future<void> _runStress(MeshSession s) async {
    final int secs = int.tryParse(_stressSecs.text) ?? 0;
    final int len = int.tryParse(_stressLen.text) ?? 0;
    if (secs < 1 || secs > 3600 || len < 7 || len > 496) {
      _snack('secs 1..3600, len 7..496');
      return;
    }
    final bool? go = await showDialog<bool>(
      context: context,
      builder: (BuildContext ctx) => AlertDialog(
        title: const Text('Saturate the channel?'),
        content: Text(
          'STRESS $secs $len floods the channel multicast for $secs seconds. '
          'Real traffic will be drowned out while it runs — this is the '
          'point, and why it is gated.',
        ),
        actions: <Widget>[
          TextButton(
              onPressed: () => Navigator.pop(ctx, false),
              child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(ctx, true),
              child: const Text('Run stress')),
        ],
      ),
    );
    if (go != true) return;

    setState(() {
      _stressRunning = true;
      _stressLog.clear();
    });
    // STRESS reports progress as # commentary; mirror it live.
    _stressSub = s.comments?.listen((String c) {
      if (!c.contains('stress')) return;
      if (mounted) {
        setState(() {
          _stressLog.add(c);
          if (_stressLog.length > 40) _stressLog.removeAt(0);
        });
      }
    });
    try {
      final (String r, _) = await s.run('STRESS $secs $len');
      setState(() => _stressLog.add('> $r'));
      // Let the run finish + its completion line arrive, then stop mirroring.
      await Future<void>.delayed(Duration(seconds: secs + 3));
    } catch (e) {
      _snack(_errText(e));
    } finally {
      await _stressSub?.cancel();
      _stressSub = null;
      if (mounted) setState(() => _stressRunning = false);
    }
  }

  // ---- Report ----

  Future<void> _exportReport(MeshSession s, {required bool asJson}) async {
    setState(() => _reportBusy = true);
    try {
      final Map<String, Object> report = <String, Object>{
        'generated': DateTime.now().toIso8601String(),
        'app': 'magnet_app',
      };
      final (String who, _) = await s.run('WHOAMI');
      report['identity'] = HcpClient.parseKv(who);
      final (String st, _) = await s.run('STATUS');
      report['status'] = HcpClient.parseKv(st);
      final (String ch, _) = await s.run('CHANNEL SHOW');
      report['channel'] = HcpClient.parseKv(ch);
      final (_, List<String> sys) = await s.run('SYSINFO');
      report['sysinfo'] = sys.where((String c) => c.startsWith('sys')).toList();
      final (_, List<String> stats) = await s.run('STATS');
      report['counters'] = _parseStats(stats);
      if (_stages != null) {
        report['selftest'] = <String, Object>{
          'pass': _selftestPass,
          'stages': _stages!,
        };
      }

      final String out;
      if (asJson) {
        out = const JsonEncoder.withIndent('  ').convert(report);
      } else {
        final StringBuffer b = StringBuffer('MagNET node report\n');
        report.forEach((String k, Object v) => b.writeln('$k: $v'));
        out = b.toString();
      }
      await Clipboard.setData(ClipboardData(text: out));
      _snack('report copied (${asJson ? 'JSON' : 'text'}, '
          '${out.length} chars)');
    } catch (e) {
      _snack(_errText(e));
    } finally {
      if (mounted) setState(() => _reportBusy = false);
    }
  }

  // ---- UI ----

  @override
  Widget build(BuildContext context) {
    super.build(context);
    final MeshSession session = context.watch<MeshSession>();
    final ColorScheme cs = Theme.of(context).colorScheme;
    final bool up = session.state == MeshLinkState.connected;

    return ListView(
      padding: const EdgeInsets.all(16),
      children: <Widget>[
        if (!up) _NotConnectedCard(session: session, cs: cs),
        _card(
          cs,
          title: 'Selftest',
          subtitle: 'envelope roundtrip · event pump · CoAP loopback',
          trailing: FilledButton(
            onPressed: up && !_selftestRunning
                ? () => _runSelftest(session)
                : null,
            child: _selftestRunning
                ? const SizedBox(
                    width: 16, height: 16,
                    child: CircularProgressIndicator(strokeWidth: 2))
                : const Text('Run'),
          ),
          child: _stages == null
              ? null
              : Column(
                  children: <Widget>[
                    for (final MapEntry<String, bool> e in _stages!.entries)
                      _stageRow(e.key, e.value, cs),
                    if (_stages!.isEmpty)
                      Text('no stage lines — ran at ok> ?',
                          style: TextStyle(color: cs.onSurfaceVariant)),
                    const SizedBox(height: 4),
                    _stageRow('overall', _selftestPass, cs),
                  ],
                ),
        ),
        _card(
          cs,
          title: 'Stats',
          subtitle: _prevStatsAt == null
              ? 'sample twice — loss and rate only exist as differences'
              : 'Δ over ${_statsAt!.difference(_prevStatsAt!).inSeconds}s '
                  'since previous sample',
          trailing: FilledButton(
            onPressed: up ? () => _sampleStats(session) : null,
            child: const Text('Sample'),
          ),
          child: _stats == null ? null : _statsTable(cs),
        ),
        _card(
          cs,
          title: 'Bench',
          subtitle: 'envelope codec µs/op + TX queue latency',
          trailing: FilledButton(
            onPressed: up && !_benchRunning ? () => _runBench(session) : null,
            child: _benchRunning
                ? const SizedBox(
                    width: 16, height: 16,
                    child: CircularProgressIndicator(strokeWidth: 2))
                : const Text('Run'),
          ),
          child: _benchLines.isEmpty
              ? null
              : Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    for (final String l in _benchLines)
                      Text(l,
                          style: const TextStyle(
                              fontFamily: 'monospace', fontSize: 11)),
                  ],
                ),
        ),
        _card(
          cs,
          title: 'Stress',
          subtitle: 'saturation burst — drowns real traffic while running',
          trailing: FilledButton.tonal(
            onPressed:
                up && !_stressRunning ? () => _runStress(session) : null,
            child: _stressRunning
                ? const SizedBox(
                    width: 16, height: 16,
                    child: CircularProgressIndicator(strokeWidth: 2))
                : const Text('Run…'),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Row(children: <Widget>[
                SizedBox(
                  width: 90,
                  child: TextField(
                    controller: _stressSecs,
                    keyboardType: TextInputType.number,
                    decoration: const InputDecoration(
                        labelText: 'secs', isDense: true),
                  ),
                ),
                const SizedBox(width: 12),
                SizedBox(
                  width: 90,
                  child: TextField(
                    controller: _stressLen,
                    keyboardType: TextInputType.number,
                    decoration: const InputDecoration(
                        labelText: 'len (7..496)', isDense: true),
                  ),
                ),
              ]),
              if (_stressLog.isNotEmpty) ...<Widget>[
                const SizedBox(height: 8),
                for (final String l in _stressLog.reversed.take(8))
                  Text(l,
                      style: const TextStyle(
                          fontFamily: 'monospace', fontSize: 11)),
              ],
            ],
          ),
        ),
        _card(
          cs,
          title: 'Node report',
          subtitle: 'identity · firmware · channel · counters · selftest',
          trailing: _reportBusy
              ? const SizedBox(
                  width: 16, height: 16,
                  child: CircularProgressIndicator(strokeWidth: 2))
              : null,
          child: Wrap(spacing: 8, children: <Widget>[
            OutlinedButton.icon(
              onPressed:
                  up && !_reportBusy ? () => _exportReport(session, asJson: true) : null,
              icon: const Icon(Icons.data_object, size: 18),
              label: const Text('Copy JSON'),
            ),
            OutlinedButton.icon(
              onPressed: up && !_reportBusy
                  ? () => _exportReport(session, asJson: false)
                  : null,
              icon: const Icon(Icons.notes, size: 18),
              label: const Text('Copy text'),
            ),
          ]),
        ),
      ],
    );
  }

  Widget _card(ColorScheme cs,
          {required String title,
          required String subtitle,
          Widget? trailing,
          Widget? child}) =>
      Card(
        margin: const EdgeInsets.only(bottom: 12),
        child: Padding(
          padding: const EdgeInsets.all(14),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Row(children: <Widget>[
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text(title,
                          style:
                              const TextStyle(fontWeight: FontWeight.w600)),
                      Text(subtitle,
                          style: TextStyle(
                              fontSize: 11, color: cs.onSurfaceVariant)),
                    ],
                  ),
                ),
                if (trailing != null) trailing,
              ]),
              if (child != null) ...<Widget>[
                const SizedBox(height: 10),
                child,
              ],
            ],
          ),
        ),
      );

  Widget _stageRow(String name, bool ok, ColorScheme cs) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 2),
        child: Row(children: <Widget>[
          Icon(ok ? Icons.check_circle : Icons.cancel,
              size: 16, color: ok ? cs.primary : cs.error),
          const SizedBox(width: 8),
          Text(name, style: const TextStyle(fontFamily: 'monospace')),
        ]),
      );

  Widget _statsTable(ColorScheme cs) {
    final Map<String, int> now = _stats!;
    final Map<String, int>? prev = _prevStats;
    final double secs = _prevStatsAt == null
        ? 0
        : _statsAt!.difference(_prevStatsAt!).inMilliseconds / 1000.0;
    return Column(
      children: <Widget>[
        for (final MapEntry<String, int> e in now.entries)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 1),
            child: Row(children: <Widget>[
              SizedBox(
                  width: 110,
                  child: Text(e.key,
                      style: TextStyle(
                          fontSize: 12, color: cs.onSurfaceVariant))),
              Text('${e.value}',
                  style: const TextStyle(
                      fontFamily: 'monospace', fontSize: 12)),
              const Spacer(),
              if (prev != null && secs > 0 && prev.containsKey(e.key))
                Text(
                  '${((e.value - prev[e.key]!) / secs).toStringAsFixed(1)}/s',
                  style: TextStyle(
                      fontFamily: 'monospace',
                      fontSize: 12,
                      color: e.value != prev[e.key]
                          ? cs.primary
                          : cs.onSurfaceVariant),
                ),
            ]),
          ),
      ],
    );
  }
}

/// The boring failures, diagnosed instead of shrugged at: adapter off,
/// permission missing, or simply out of range / not connected.
class _NotConnectedCard extends StatelessWidget {
  const _NotConnectedCard({required this.session, required this.cs});
  final MeshSession session;
  final ColorScheme cs;

  @override
  Widget build(BuildContext context) {
    final ScannerController sc = context.watch<ScannerController>();
    final String why = switch (sc.adapterState) {
      BleAdapterState.off => 'Bluetooth is OFF — turn it on first.',
      BleAdapterState.unauthorized =>
        'Bluetooth permission denied — grant it in system settings.',
      _ => session.state == MeshLinkState.connecting
          ? 'Connecting to the companion…'
          : 'Companion not connected — powered off, out of range, or '
              'busy. Reconnect from the app bar.',
    };
    return Card(
      color: cs.errorContainer.withValues(alpha: .35),
      margin: const EdgeInsets.only(bottom: 12),
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Row(children: <Widget>[
          Icon(Icons.link_off, color: cs.error),
          const SizedBox(width: 10),
          Expanded(child: Text(why)),
        ]),
      ),
    );
  }
}
