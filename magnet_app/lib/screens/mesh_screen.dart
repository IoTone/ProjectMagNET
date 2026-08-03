// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// M3 — live mesh view (SCOPE.md §M3): companion picker, event feed with a
/// chat composer, peer table, and mesh topology, all through the one node
/// running the resident-BLE companion build.
library;
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:provider/provider.dart';

import '../gen/app_localizations.dart';
import '../magnet/magnet_ble.dart';
import '../magnet/mesh_session.dart';
import 'mesh_test_tab.dart';

class MeshScreen extends StatelessWidget {
  const MeshScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final MeshSession session = context.watch<MeshSession>();
    final AppLocalizations l = AppLocalizations.of(context);
    if (!session.hasCompanion) {
      return Scaffold(
        appBar: AppBar(title: Text(l.meshPickTitle)),
        body: const _CompanionPicker(),
      );
    }
    return const _LiveMeshView();
  }
}

// ---------------------------------------------------------------------------
// Companion picker
// ---------------------------------------------------------------------------

/// Scans for MagNET nodes and adopts the tapped one as THE companion.
///
/// Selection is stored by platform device id. The advertised `MagNET-XXXX`
/// name is display-only here: it snapshots the node's device id at BLE start
/// and platforms cache it, so it must never be used as the identity key.
class _CompanionPicker extends StatefulWidget {
  const _CompanionPicker();

  @override
  State<_CompanionPicker> createState() => _CompanionPickerState();
}

class _CompanionPickerState extends State<_CompanionPicker> {
  final Map<String, ScanResult> _found = <String, ScanResult>{};
  StreamSubscription<List<ScanResult>>? _sub;
  bool _scanning = false;
  bool _adopting = false;

  @override
  void dispose() {
    _sub?.cancel();
    unawaited(FlutterBluePlus.stopScan());
    super.dispose();
  }

  Future<void> _scan() async {
    setState(() {
      _found.clear();
      _scanning = true;
    });
    _sub ??= FlutterBluePlus.scanResults.listen((List<ScanResult> rs) {
      bool changed = false;
      for (final ScanResult r in rs) {
        final String name = r.advertisementData.advName;
        final List<String> svcs = r.advertisementData.serviceUuids
            .map((Guid g) => g.str.toLowerCase())
            .toList();
        if (!MagnetUuids.looksLikeNode(name: name, serviceUuids: svcs)) {
          continue;
        }
        _found[r.device.remoteId.str] = r;
        changed = true;
      }
      if (changed && mounted) setState(() {});
    });
    try {
      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 12));
      await FlutterBluePlus.isScanning
          .firstWhere((bool s) => !s)
          .timeout(const Duration(seconds: 15));
    } catch (_) {/* adapter off / permission — the empty state explains */}
    if (mounted) setState(() => _scanning = false);
  }

  Future<void> _adopt(ScanResult r) async {
    final MeshSession session = context.read<MeshSession>();
    setState(() => _adopting = true);
    await FlutterBluePlus.stopScan();
    final String name = r.advertisementData.advName;
    await session.adopt(
      r.device.remoteId.str,
      name: name.isEmpty ? null : name,
    );
    if (mounted) setState(() => _adopting = false);
  }

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final List<ScanResult> results = _found.values.toList()
      ..sort((ScanResult a, ScanResult b) => b.rssi.compareTo(a.rssi));

    return ListView(
      padding: const EdgeInsets.all(20),
      children: <Widget>[
        Text(l.meshPickHint, style: TextStyle(color: cs.onSurfaceVariant)),
        const SizedBox(height: 16),
        FilledButton.icon(
          onPressed: _scanning || _adopting ? null : _scan,
          icon: _scanning
              ? const SizedBox(
                  width: 16,
                  height: 16,
                  child: CircularProgressIndicator(strokeWidth: 2))
              : const Icon(Icons.bluetooth_searching),
          label: Text(_scanning ? l.meshPickScanning : l.meshPickScan),
        ),
        const SizedBox(height: 16),
        if (results.isEmpty && !_scanning)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 24),
            child: Text(l.meshPickEmpty,
                textAlign: TextAlign.center,
                style: TextStyle(color: cs.onSurfaceVariant)),
          ),
        for (final ScanResult r in results)
          Card(
            child: ListTile(
              leading: const Icon(Icons.hub_outlined),
              title: Text(r.advertisementData.advName.isEmpty
                  ? r.device.remoteId.str
                  : r.advertisementData.advName),
              subtitle: Text('${r.device.remoteId.str} · ${r.rssi} dBm'),
              trailing: _adopting ? null : const Icon(Icons.chevron_right),
              enabled: !_adopting,
              onTap: () => _adopt(r),
            ),
          ),
      ],
    );
  }
}

// ---------------------------------------------------------------------------
// Live view
// ---------------------------------------------------------------------------

class _LiveMeshView extends StatefulWidget {
  const _LiveMeshView();

  @override
  State<_LiveMeshView> createState() => _LiveMeshViewState();
}

class _LiveMeshViewState extends State<_LiveMeshView> {
  @override
  void initState() {
    super.initState();
    // Arriving with a remembered companion but no link → connect now.
    final MeshSession s = context.read<MeshSession>();
    if (s.state == MeshLinkState.idle) {
      unawaited(s.connect());
    }
  }

  @override
  Widget build(BuildContext context) {
    final MeshSession session = context.watch<MeshSession>();
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;

    final (String stateLabel, Color stateColor) = switch (session.state) {
      MeshLinkState.connecting => (l.meshStateConnecting, cs.tertiary),
      MeshLinkState.connected => (l.meshStateConnected, cs.primary),
      MeshLinkState.idle => (l.meshStateDisconnected, cs.onSurfaceVariant),
      MeshLinkState.error => (l.meshStateError, cs.error),
    };

    return DefaultTabController(
      length: 4,
      child: Scaffold(
        appBar: AppBar(
          title: Row(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              Flexible(
                child: Text(session.companionName ?? l.meshTitle,
                    overflow: TextOverflow.ellipsis),
              ),
              const SizedBox(width: 10),
              Container(
                padding:
                    const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                decoration: BoxDecoration(
                  color: stateColor.withValues(alpha: .15),
                  borderRadius: BorderRadius.circular(10),
                ),
                child: Text(stateLabel,
                    style: TextStyle(fontSize: 12, color: stateColor)),
              ),
            ],
          ),
          actions: <Widget>[
            if (session.state != MeshLinkState.connected &&
                session.state != MeshLinkState.connecting)
              IconButton(
                tooltip: l.meshReconnect,
                icon: const Icon(Icons.refresh),
                onPressed: () => session.connect(),
              ),
            PopupMenuButton<String>(
              onSelected: (String v) {
                if (v == 'forget') unawaited(session.forget());
              },
              itemBuilder: (BuildContext ctx) => <PopupMenuEntry<String>>[
                PopupMenuItem<String>(
                  value: 'forget',
                  child: Text(l.meshChangeCompanion),
                ),
              ],
            ),
          ],
          bottom: TabBar(tabs: <Tab>[
            Tab(text: l.meshTabFeed),
            Tab(text: l.meshTabPeers),
            Tab(text: l.meshTabTopology),
            Tab(text: l.meshTabTest),
          ]),
        ),
        body: const TabBarView(
          children: <Widget>[
            _FeedTab(),
            _PeersTab(),
            _TopologyTab(),
            MeshTestTab(),
          ],
        ),
      ),
    );
  }
}

// ---- Feed --------------------------------------------------------------

class _FeedTab extends StatefulWidget {
  const _FeedTab();

  @override
  State<_FeedTab> createState() => _FeedTabState();
}

class _FeedTabState extends State<_FeedTab> {
  final TextEditingController _composer = TextEditingController();
  bool _sending = false;

  @override
  void dispose() {
    _composer.dispose();
    super.dispose();
  }

  Future<void> _send() async {
    final MeshSession s = context.read<MeshSession>();
    final String text = _composer.text.trim();
    if (text.isEmpty || _sending) return;
    setState(() => _sending = true);
    try {
      await s.sendChat(text);
      _composer.clear();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context)
            .showSnackBar(SnackBar(content: Text('$e')));
      }
    } finally {
      if (mounted) setState(() => _sending = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final MeshSession session = context.watch<MeshSession>();
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final List<MeshFeedItem> feed = session.feed.reversed.toList();

    return Column(
      children: <Widget>[
        Expanded(
          child: feed.isEmpty
              ? Center(
                  child: Padding(
                    padding: const EdgeInsets.all(24),
                    child: Text(l.meshFeedEmpty,
                        textAlign: TextAlign.center,
                        style: TextStyle(color: cs.onSurfaceVariant)),
                  ),
                )
              : ListView.builder(
                  reverse: true,
                  padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
                  itemCount: feed.length,
                  itemBuilder: (BuildContext ctx, int i) =>
                      _FeedLine(item: feed[i]),
                ),
        ),
        SafeArea(
          top: false,
          child: Padding(
            padding: const EdgeInsets.fromLTRB(12, 4, 12, 8),
            child: Row(
              children: <Widget>[
                Expanded(
                  child: TextField(
                    controller: _composer,
                    enabled: session.state == MeshLinkState.connected,
                    decoration: InputDecoration(
                      hintText: l.meshComposerHint,
                      isDense: true,
                      border: OutlineInputBorder(
                          borderRadius: BorderRadius.circular(24)),
                      contentPadding: const EdgeInsets.symmetric(
                          horizontal: 16, vertical: 10),
                    ),
                    textInputAction: TextInputAction.send,
                    onSubmitted: (_) => _send(),
                  ),
                ),
                const SizedBox(width: 8),
                IconButton.filled(
                  tooltip: l.meshSend,
                  onPressed: session.state == MeshLinkState.connected &&
                          !_sending
                      ? _send
                      : null,
                  icon: const Icon(Icons.send),
                ),
              ],
            ),
          ),
        ),
      ],
    );
  }
}

class _FeedLine extends StatelessWidget {
  const _FeedLine({required this.item});
  final MeshFeedItem item;

  @override
  Widget build(BuildContext context) {
    final ColorScheme cs = Theme.of(context).colorScheme;
    final String hh = item.at.hour.toString().padLeft(2, '0');
    final String mm = item.at.minute.toString().padLeft(2, '0');
    final String ss = item.at.second.toString().padLeft(2, '0');
    final String when = '$hh:$mm:$ss';

    final (IconData icon, Color color) = switch (item.kind) {
      'chat' when item.backfill => (Icons.history, cs.onSurfaceVariant),
      'chat' => (Icons.chat_bubble_outline, cs.primary),
      'dm' => (Icons.lock_outline, cs.primary),
      'sent' => (Icons.north_east, cs.tertiary),
      'peer' => (Icons.person_add_alt, cs.secondary),
      'role' || 'state' => (Icons.swap_horiz, cs.onSurfaceVariant),
      'warn' => (Icons.warning_amber, cs.error),
      _ => (Icons.info_outline, cs.onSurfaceVariant),
    };

    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 3),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Icon(icon, size: 16, color: color),
          const SizedBox(width: 8),
          Expanded(
            child: Text.rich(
              TextSpan(children: <InlineSpan>[
                if (item.from != null)
                  TextSpan(
                      text: '${item.from}  ',
                      style: TextStyle(
                          fontWeight: FontWeight.w600, color: color)),
                TextSpan(text: item.text),
              ]),
            ),
          ),
          const SizedBox(width: 8),
          Text(
              item.backfill
                  ? AppLocalizations.of(context).meshBackfillTag
                  : when,
              style: TextStyle(
                  fontSize: 11,
                  color: cs.onSurfaceVariant,
                  fontStyle:
                      item.backfill ? FontStyle.italic : FontStyle.normal)),
        ],
      ),
    );
  }
}

// ---- Peers ---------------------------------------------------------------

class _PeersTab extends StatelessWidget {
  const _PeersTab();

  @override
  Widget build(BuildContext context) {
    final MeshSession session = context.watch<MeshSession>();
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;

    return RefreshIndicator(
      onRefresh: () => session.refreshPeers(),
      child: session.peers.isEmpty
          ? ListView(
              children: <Widget>[
                Padding(
                  padding: const EdgeInsets.all(32),
                  child: Text(l.meshNoPeers,
                      textAlign: TextAlign.center,
                      style: TextStyle(color: cs.onSurfaceVariant)),
                ),
              ],
            )
          : ListView.builder(
              itemCount: session.peers.length,
              itemBuilder: (BuildContext ctx, int i) {
                final MeshPeer p = session.peers[i];
                return ListTile(
                  leading: const Icon(Icons.device_hub),
                  title: Text(p.displayName),
                  subtitle: Text('${p.id} · ${p.ipv6}',
                      style: const TextStyle(fontSize: 12)),
                  trailing: p.lastSeen == null
                      ? null
                      : Text(
                          l.meshPeerLastSeen(
                              p.lastSeen!.inSeconds.toString()),
                          style: TextStyle(
                              fontSize: 11, color: cs.onSurfaceVariant)),
                );
              },
            ),
    );
  }
}

// ---- Topology --------------------------------------------------------------

class _TopologyTab extends StatelessWidget {
  const _TopologyTab();

  @override
  Widget build(BuildContext context) {
    final MeshSession session = context.watch<MeshSession>();
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final MeshTopology t = session.topology;

    final List<(String, String)> facts = <(String, String)>[
      ('role', t.role),
      ('partition', t.partition),
      ('rloc16', t.rloc16),
      ('channel', t.channel),
      ('pan', t.pan),
      ('ml-eid', t.mlEid),
      ('mcast', t.mcast),
      ('leader-rloc', t.leaderRloc),
    ].where(((String, String) f) => f.$2.isNotEmpty).toList();

    return RefreshIndicator(
      onRefresh: () => session.refreshTopology(),
      child: ListView(
        padding: const EdgeInsets.all(16),
        children: <Widget>[
          Align(
            alignment: Alignment.centerRight,
            child: TextButton.icon(
              onPressed: () => session.refreshTopology(),
              icon: const Icon(Icons.refresh, size: 18),
              label: Text(l.meshRefresh),
            ),
          ),
          Card(
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: Column(
                children: <Widget>[
                  for (final (String k, String v) in facts)
                    Padding(
                      padding: const EdgeInsets.symmetric(vertical: 3),
                      child: Row(
                        children: <Widget>[
                          SizedBox(
                            width: 110,
                            child: Text(k,
                                style:
                                    TextStyle(color: cs.onSurfaceVariant)),
                          ),
                          Expanded(
                            child: Text(v,
                                style: const TextStyle(
                                    fontFamily: 'monospace', fontSize: 13)),
                          ),
                        ],
                      ),
                    ),
                ],
              ),
            ),
          ),
          const SizedBox(height: 16),
          Text(l.meshNeighbors,
              style: Theme.of(context).textTheme.titleMedium),
          const SizedBox(height: 8),
          if (t.neighbors.isEmpty)
            Text(l.meshNoNeighbors,
                style: TextStyle(color: cs.onSurfaceVariant))
          else
            for (final MeshNeighbor n in t.neighbors)
              ListTile(
                dense: true,
                leading: Icon(
                    n.isChild ? Icons.child_care : Icons.router_outlined),
                title: Text(n.rloc16,
                    style: const TextStyle(fontFamily: 'monospace')),
                subtitle: Text(
                    'rssi ${n.rssi} dBm · lqi ${n.lqi} · age ${n.age.inSeconds}s'),
              ),
        ],
      ),
    );
  }
}
