// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// The fleet ledger (SCOPE M1): every node this phone has provisioned —
/// id, name, channel label, selector, when. Nodes go dark on BLE the moment
/// they take a channel, so this log is how the fleet stays knowable.
library;
import 'package:flutter/material.dart';

import '../gen/app_localizations.dart';
import '../magnet/credentials.dart';

class ProvisionLogScreen extends StatefulWidget {
  const ProvisionLogScreen({super.key});

  @override
  State<ProvisionLogScreen> createState() => _ProvisionLogScreenState();
}

class _ProvisionLogScreenState extends State<ProvisionLogScreen> {
  List<ProvisionRecord>? _records;

  @override
  void initState() {
    super.initState();
    ProvisionLog.load().then((List<ProvisionRecord> r) {
      if (mounted) setState(() => _records = r);
    });
  }

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final List<ProvisionRecord>? records = _records;

    return Scaffold(
      appBar: AppBar(title: Text(l.provisionLogTitle)),
      body: records == null
          ? const Center(child: CircularProgressIndicator())
          : records.isEmpty
              ? Center(
                  child: Padding(
                    padding: const EdgeInsets.all(32),
                    child: Text(l.provisionLogEmpty,
                        textAlign: TextAlign.center,
                        style: TextStyle(color: cs.onSurfaceVariant)),
                  ),
                )
              : ListView.separated(
                  itemCount: records.length,
                  separatorBuilder: (_, __) => const Divider(height: 1),
                  itemBuilder: (BuildContext ctx, int i) {
                    final ProvisionRecord r = records[i];
                    final String when =
                        r.at.toLocal().toString().substring(0, 16);
                    return ListTile(
                      leading: const Icon(Icons.hub_outlined),
                      title: Text(r.name.isEmpty || r.name == '-'
                          ? r.nodeId
                          : r.name),
                      subtitle: Text(
                        'id ${r.nodeId} · ${r.channelLabel} '
                        '(selector ${r.selector}, path ${r.path})\n$when',
                        style: const TextStyle(fontSize: 12),
                      ),
                      isThreeLine: true,
                    );
                  },
                ),
    );
  }
}
