// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:go_router/go_router.dart';
import 'package:provider/provider.dart';

import '../gen/app_localizations.dart';
import '../scanner/ble_scanner.dart';
import '../scanner/scanner_controller.dart';
import 'scan_controls.dart';

/// Home dashboard: scan summary (BLE + WiFi counts, radio state) and
/// the primary Start/Stop control.
class DashboardScreen extends StatelessWidget {
  const DashboardScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final ScannerController sc = context.watch<ScannerController>();
    final ColorScheme cs = Theme.of(context).colorScheme;
    final AppLocalizations l = AppLocalizations.of(context);

    final String radio = switch (sc.adapterState) {
      BleAdapterState.on => l.bluetoothOn,
      BleAdapterState.off => l.bluetoothOff,
      BleAdapterState.unauthorized => l.bluetoothUnauthorized,
      BleAdapterState.unknown => l.bluetoothUnknown,
    };

    return ListView(
      padding: const EdgeInsets.fromLTRB(20, 24, 20, 32),
      children: <Widget>[
        Text(
          sc.isScanning ? l.scanScanning : l.scanIdle,
          style: TextStyle(
              color: sc.isScanning ? cs.tertiary : cs.onSurfaceVariant,
              fontSize: 13,
              letterSpacing: 4),
        ),
        const SizedBox(height: 16),
        Row(
          children: <Widget>[
            Expanded(
              child: _CountCard(
                label: l.dashBleDevices,
                value: sc.bleCount,
                icon: Icons.bluetooth,
                color: cs.primary,
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: _CountCard(
                label: l.dashWifiNetworks,
                value: sc.wifiCount,
                icon: Icons.wifi,
                color: cs.secondary,
              ),
            ),
          ],
        ),
        const SizedBox(height: 20),
        _KvRow(label: l.dashRadio, value: radio, cs: cs),
        const SizedBox(height: 24),
        FilledButton.icon(
          onPressed: () => ScanControls.toggle(context),
          icon: Icon(sc.isScanning ? Icons.stop : Icons.play_arrow),
          label: Text(sc.isScanning ? l.scanStop : l.scanStart),
        ),
        if (!sc.isScanning && sc.bleCount == 0 && sc.wifiCount == 0) ...<Widget>[
          const SizedBox(height: 20),
          Text(l.dashScanHint,
              style: TextStyle(color: cs.onSurfaceVariant, height: 1.4)),
        ],
        const SizedBox(height: 20),
        Card(
          child: ListTile(
            leading: Icon(Icons.hub_outlined, color: cs.primary),
            title: Text(l.meshDashboardCard),
            subtitle: Text(l.meshDashboardHint),
            trailing: const Icon(Icons.chevron_right),
            onTap: () => context.push('/mesh'),
          ),
        ),
      ],
    );
  }
}

class _CountCard extends StatelessWidget {
  const _CountCard({
    required this.label,
    required this.value,
    required this.icon,
    required this.color,
  });
  final String label;
  final int value;
  final IconData icon;
  final Color color;

  @override
  Widget build(BuildContext context) {
    final ColorScheme cs = Theme.of(context).colorScheme;
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: cs.surfaceContainerHighest.withValues(alpha: .4),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: cs.outline.withValues(alpha: .3)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Icon(icon, color: color, size: 20),
          const SizedBox(height: 10),
          Text('$value',
              style: TextStyle(
                  color: cs.onSurface,
                  fontSize: 34,
                  fontWeight: FontWeight.w700,
                  height: 1)),
          const SizedBox(height: 2),
          Text(label,
              style: TextStyle(color: cs.onSurfaceVariant, fontSize: 12)),
        ],
      ),
    );
  }
}

class _KvRow extends StatelessWidget {
  const _KvRow({required this.label, required this.value, required this.cs});
  final String label;
  final String value;
  final ColorScheme cs;

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisAlignment: MainAxisAlignment.spaceBetween,
      children: <Widget>[
        Text(label, style: TextStyle(color: cs.onSurfaceVariant)),
        Text(value,
            style: TextStyle(color: cs.onSurface, fontFamily: 'monospace')),
      ],
    );
  }
}
