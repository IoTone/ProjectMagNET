// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// A single line in the diagnostics scan log.
enum ScanEventKind { started, stopped, bleFound, wifiFound, adapter, cleared }

class ScanEvent {
  const ScanEvent({required this.at, required this.kind, required this.detail});
  final DateTime at;
  final ScanEventKind kind;
  final String detail;

  String get kindLabel => switch (kind) {
        ScanEventKind.started => 'SCAN',
        ScanEventKind.stopped => 'STOP',
        ScanEventKind.bleFound => 'BLE',
        ScanEventKind.wifiFound => 'WIFI',
        ScanEventKind.adapter => 'RADIO',
        ScanEventKind.cleared => 'CLEAR',
      };
}
