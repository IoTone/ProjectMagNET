// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../gen/app_localizations.dart';
import '../scanner/scan_event.dart';
import '../scanner/scanner_controller.dart';

/// Diagnostics: a rolling log of scan activity (devices found, radio
/// state, start/stop) with copy-to-clipboard. A useful pattern for a
/// forensic tool and for debugging the scan pipeline.
class DiagnosticsScreen extends StatelessWidget {
  const DiagnosticsScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final ScannerController sc = context.watch<ScannerController>();
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final List<ScanEvent> log = sc.log;

    return Scaffold(
      appBar: AppBar(
        title: Text(l.settingsDiagnostics),
        actions: <Widget>[
          IconButton(
            tooltip: l.diagClear,
            icon: const Icon(Icons.delete_outline),
            onPressed: sc.clear,
          ),
          IconButton(
            tooltip: l.diagCopy,
            icon: const Icon(Icons.copy_all),
            onPressed: log.isEmpty
                ? null
                : () {
                    final String text = log
                        .map((ScanEvent e) =>
                            '${_hms(e.at)} ${e.kindLabel} ${e.detail}')
                        .join('\n');
                    Clipboard.setData(ClipboardData(text: text));
                    ScaffoldMessenger.of(context).showSnackBar(
                      SnackBar(content: Text(l.commonCopied)),
                    );
                  },
          ),
        ],
      ),
      body: log.isEmpty
          ? Center(
              child: Text(l.diagEmpty,
                  style: TextStyle(color: cs.onSurfaceVariant)))
          : ListView.builder(
              itemCount: log.length,
              itemBuilder: (BuildContext _, int i) {
                final ScanEvent e = log[i];
                return Padding(
                  padding:
                      const EdgeInsets.symmetric(horizontal: 14, vertical: 3),
                  child: Text(
                    '${_hms(e.at)}  ${e.kindLabel.padRight(5)} ${e.detail}',
                    style: TextStyle(
                        color: cs.onSurface,
                        fontFamily: 'monospace',
                        fontSize: 12),
                  ),
                );
              },
            ),
    );
  }

  static String _hms(DateTime t) =>
      '${t.hour.toString().padLeft(2, '0')}:'
      '${t.minute.toString().padLeft(2, '0')}:'
      '${t.second.toString().padLeft(2, '0')}';
}
