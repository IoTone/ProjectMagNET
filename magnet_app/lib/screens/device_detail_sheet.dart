// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../gen/app_localizations.dart';
import '../scanner/ble_scanner.dart';
import '../scanner/scanned_device.dart';
import '../scanner/scanner_controller.dart';
import '../scanner/vendor_lookup.dart';

/// Detail sheet for a scanned BLE device: identifiers, vendor (from
/// the advertised company id), live signal, advertised services, and
/// a Connect action that performs a GATT connect + service discovery.
class DeviceDetailSheet extends StatelessWidget {
  const DeviceDetailSheet({super.key, required this.deviceId});
  final String deviceId;

  @override
  Widget build(BuildContext context) {
    final ScannerController sc = context.watch<ScannerController>();
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final ScannedDevice? d = sc.deviceById(deviceId);

    if (d == null) {
      return Padding(
        padding: const EdgeInsets.all(24),
        child: Text(l.devicesEmpty,
            style: TextStyle(color: cs.onSurfaceVariant)),
      );
    }

    final String vendor = VendorLookup.forCompanyId(d.companyId) ??
        (d.companyId != null
            ? '0x${d.companyId!.toRadixString(16).padLeft(4, '0')} · ${l.deviceDetailVendorUnknown}'
            : l.deviceDetailVendorUnknown);
    final DeviceConnection conn = sc.connectionFor(deviceId);

    return SafeArea(
      child: SingleChildScrollView(
        padding: const EdgeInsets.fromLTRB(20, 4, 20, 24),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            Text(d.isNamed ? d.name : l.deviceUnnamed,
                style: TextStyle(
                    color: cs.onSurface,
                    fontSize: 20,
                    fontWeight: FontWeight.w600)),
            const SizedBox(height: 12),
            _kv(l.deviceDetailId, d.id, cs),
            _kv(l.deviceDetailSignal, l.rssiDbm(d.rssi), cs),
            _kv(l.deviceDetailVendor, vendor, cs),
            if (d.serviceUuids.isNotEmpty)
              _kv(l.deviceDetailServices, d.serviceUuids.join(', '), cs),
            _kv(l.deviceDetailFirstSeen, _hms(d.firstSeen), cs),
            _kv(l.deviceDetailLastSeen, _hms(d.lastSeen), cs),
            const SizedBox(height: 16),
            _ConnectButton(deviceId: deviceId, conn: conn, l: l),
            if (sc.connectFailed && conn == DeviceConnection.disconnected)
              Padding(
                padding: const EdgeInsets.only(top: 8),
                child: Text(l.deviceConnectFailed,
                    style: TextStyle(color: cs.error)),
              ),
            if (conn == DeviceConnection.connected &&
                sc.services.isNotEmpty) ...<Widget>[
              const SizedBox(height: 18),
              Text(l.deviceGattServices,
                  style: TextStyle(
                      color: cs.primary, fontSize: 12, letterSpacing: 2)),
              const SizedBox(height: 6),
              for (final GattService s in sc.services)
                _ServiceTile(service: s, cs: cs),
            ],
          ],
        ),
      ),
    );
  }

  Widget _kv(String k, String v, ColorScheme cs) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 4),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: <Widget>[
            SizedBox(
                width: 96,
                child:
                    Text(k, style: TextStyle(color: cs.onSurfaceVariant))),
            Expanded(
              child: Text(v,
                  style: TextStyle(
                      color: cs.onSurface, fontFamily: 'monospace')),
            ),
          ],
        ),
      );

  static String _hms(DateTime t) =>
      '${t.hour.toString().padLeft(2, '0')}:'
      '${t.minute.toString().padLeft(2, '0')}:'
      '${t.second.toString().padLeft(2, '0')}';
}

class _ConnectButton extends StatelessWidget {
  const _ConnectButton(
      {required this.deviceId, required this.conn, required this.l});
  final String deviceId;
  final DeviceConnection conn;
  final AppLocalizations l;

  @override
  Widget build(BuildContext context) {
    final ScannerController sc = context.read<ScannerController>();
    switch (conn) {
      case DeviceConnection.connecting:
        return const FilledButton(
          onPressed: null,
          child: SizedBox(
              width: 18,
              height: 18,
              child: CircularProgressIndicator(strokeWidth: 2)),
        );
      case DeviceConnection.connected:
        return OutlinedButton.icon(
          onPressed: () => sc.disconnect(),
          icon: const Icon(Icons.link_off),
          label: Text(l.deviceDisconnect),
        );
      case DeviceConnection.disconnected:
        return FilledButton.icon(
          onPressed: () => sc.connect(deviceId),
          icon: const Icon(Icons.link),
          label: Text(l.deviceConnect),
        );
    }
  }
}

class _ServiceTile extends StatelessWidget {
  const _ServiceTile({required this.service, required this.cs});
  final GattService service;
  final ColorScheme cs;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Text(service.uuid,
              style: TextStyle(
                  color: cs.onSurface,
                  fontFamily: 'monospace',
                  fontSize: 12)),
          for (final String c in service.characteristics)
            Padding(
              padding: const EdgeInsets.only(left: 12, top: 2),
              child: Text('• $c',
                  style: TextStyle(
                      color: cs.onSurfaceVariant,
                      fontFamily: 'monospace',
                      fontSize: 11)),
            ),
        ],
      ),
    );
  }
}
