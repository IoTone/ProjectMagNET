// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:go_router/go_router.dart';
import 'package:provider/provider.dart';

import '../gen/app_localizations.dart';
import '../magnet/magnet_ble.dart';
import '../scanner/scanned_access_point.dart';
import '../scanner/scanned_device.dart';
import '../scanner/scanner_controller.dart';
import '../scanner/signal_distance.dart';
import '../scanner/wifi_source.dart';
import 'device_detail_sheet.dart';
import 'scan_controls.dart';

/// Unified list of discovered devices: BLE first, then WiFi APs.
class DevicesScreen extends StatelessWidget {
  const DevicesScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final ScannerController sc = context.watch<ScannerController>();
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final List<ScannedDevice> ble = sc.devices;
    final List<ScannedAccessPoint> wifi = sc.accessPoints;

    if (ble.isEmpty && wifi.isEmpty) {
      return Center(
        child: Padding(
          padding: const EdgeInsets.all(28),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              Text(l.devicesEmpty,
                  textAlign: TextAlign.center,
                  style: TextStyle(color: cs.onSurfaceVariant)),
              const SizedBox(height: 16),
              FilledButton.icon(
                onPressed: () => ScanControls.toggle(context),
                icon: Icon(sc.isScanning ? Icons.stop : Icons.play_arrow),
                label: Text(sc.isScanning ? l.scanStop : l.scanStart),
              ),
            ],
          ),
        ),
      );
    }

    return ListView(
      children: <Widget>[
        if (ble.isNotEmpty) _SectionHeader(l.devicesSectionBle, cs: cs),
        for (final ScannedDevice d in ble)
          _BleRow(device: d, cs: cs, l: l),
        if (wifi.isNotEmpty) ...<Widget>[
          _SectionHeader(l.devicesSectionWifi, cs: cs),
          _WifiNote(capability: sc.wifiCapability, cs: cs, l: l),
          for (final ScannedAccessPoint ap in wifi)
            _WifiRow(ap: ap, cs: cs, l: l),
        ],
      ],
    );
  }
}

class _WifiNote extends StatelessWidget {
  const _WifiNote({required this.capability, required this.cs, required this.l});
  final WifiCapability capability;
  final ColorScheme cs;
  final AppLocalizations l;
  @override
  Widget build(BuildContext context) {
    final String? note = switch (capability) {
      WifiCapability.connectedOnly => l.wifiCurrentOnly,
      WifiCapability.fullScan => l.wifiThrottleNote,
      WifiCapability.unavailable => null,
    };
    if (note == null) return const SizedBox.shrink();
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 0, 16, 6),
      child: Text(note,
          style: TextStyle(color: cs.onSurfaceVariant, fontSize: 11)),
    );
  }
}

Color signalColor(int rssi, ColorScheme cs) => switch (SignalDistance.band(rssi)) {
      SignalBand.strong => cs.tertiary,
      SignalBand.medium => cs.secondary,
      SignalBand.weak => cs.onSurfaceVariant,
    };

class _SectionHeader extends StatelessWidget {
  const _SectionHeader(this.text, {required this.cs});
  final String text;
  final ColorScheme cs;
  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.fromLTRB(16, 16, 16, 6),
        child: Text(text,
            style: TextStyle(
                color: cs.primary, fontSize: 12, letterSpacing: 3)),
      );
}

class _BleRow extends StatelessWidget {
  const _BleRow({required this.device, required this.cs, required this.l});
  final ScannedDevice device;
  final ColorScheme cs;
  final AppLocalizations l;

  @override
  Widget build(BuildContext context) {
    // A node advertising the HCP service is unprovisioned and waiting to be
    // configured — surface that instead of burying it in the generic sheet.
    final bool isNode = MagnetUuids.looksLikeNode(
        name: device.name, serviceUuids: device.serviceUuids);
    return ListTile(
      leading: Icon(isNode ? Icons.hub : Icons.bluetooth,
          color: isNode ? cs.primary : signalColor(device.rssi, cs)),
      title: Row(
        children: <Widget>[
          Flexible(child: Text(device.isNamed ? device.name : l.deviceUnnamed)),
          if (isNode) ...<Widget>[
            const SizedBox(width: 8),
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
              decoration: BoxDecoration(
                color: cs.primaryContainer,
                borderRadius: BorderRadius.circular(4),
              ),
              child: Text('SET UP',
                  style: TextStyle(
                      fontSize: 10,
                      letterSpacing: 1,
                      color: cs.onPrimaryContainer)),
            ),
          ],
        ],
      ),
      subtitle: Text(device.id,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: const TextStyle(fontFamily: 'monospace', fontSize: 11)),
      trailing: Text(l.rssiDbm(device.rssi),
          style: TextStyle(color: signalColor(device.rssi, cs))),
      onTap: () {
        if (isNode) {
          context.push('/node/${Uri.encodeComponent(device.id)}'
              '?name=${Uri.encodeComponent(device.name)}');
        } else {
          showModalBottomSheet<void>(
            context: context,
            isScrollControlled: true,
            showDragHandle: true,
            builder: (_) => DeviceDetailSheet(deviceId: device.id),
          );
        }
      },
    );
  }
}

class _WifiRow extends StatelessWidget {
  const _WifiRow({required this.ap, required this.cs, required this.l});
  final ScannedAccessPoint ap;
  final ColorScheme cs;
  final AppLocalizations l;

  @override
  Widget build(BuildContext context) {
    final int? ch = ap.channel;
    final String meta =
        <String>[ap.band, if (ch != null) l.wifiChannel(ch)]
            .where((String s) => s.isNotEmpty)
            .join(' · ');
    return ListTile(
      leading: Icon(ap.isCurrentConnection ? Icons.wifi : Icons.wifi_find,
          color: signalColor(ap.rssi, cs)),
      title: Text(ap.displayName),
      subtitle: Text(
        <String>[ap.bssid, if (meta.isNotEmpty) meta].join('  ·  '),
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
        style: const TextStyle(fontFamily: 'monospace', fontSize: 11),
      ),
      trailing: Text(l.rssiDbm(ap.rssi),
          style: TextStyle(color: signalColor(ap.rssi, cs))),
    );
  }
}
