// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../app_state_model.dart';
import '../gen/app_localizations.dart';

/// About screen: MagNET branding, version, protocol level, copyright, and
/// the full open-source license inventory (Flutter's LicensePage, which
/// self-collects every bundled package's LICENSE).
class AboutScreen extends StatelessWidget {
  const AboutScreen({super.key});

  /// HCP protocol level this app speaks — the `proto=` the firmware
  /// announces in `!READY`/`CAPS` (design proposal §11.3).
  static const String hcpProto = '2.1';

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final AppState app = context.watch<AppState>();
    final ColorScheme cs = Theme.of(context).colorScheme;

    return Scaffold(
      appBar: AppBar(title: Text(l.settingsAbout)),
      body: Center(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(28),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              ClipRRect(
                borderRadius: BorderRadius.circular(18),
                child: Image.asset(
                  'assets/images/icon-192.png',
                  width: 84,
                  height: 84,
                  // Asset trouble must never take down the About screen.
                  errorBuilder: (_, __, ___) =>
                      Icon(Icons.hub_outlined, size: 64, color: cs.primary),
                ),
              ),
              const SizedBox(height: 20),
              Text(
                l.appTitle,
                textAlign: TextAlign.center,
                style: TextStyle(
                    color: cs.onSurface,
                    fontSize: 22,
                    fontWeight: FontWeight.w600,
                    letterSpacing: 1),
              ),
              const SizedBox(height: 4),
              Text(l.aboutSubtitle,
                  textAlign: TextAlign.center,
                  style: TextStyle(color: cs.onSurfaceVariant)),
              const SizedBox(height: 14),
              Text(l.aboutVersion(app.getAppVersion()),
                  style: TextStyle(
                      color: cs.onSurfaceVariant, fontFamily: 'monospace')),
              const SizedBox(height: 4),
              Text(l.aboutProtocol(hcpProto),
                  style: TextStyle(
                      color: cs.onSurfaceVariant,
                      fontFamily: 'monospace',
                      fontSize: 12)),
              const SizedBox(height: 20),
              Text(l.aboutDescription,
                  textAlign: TextAlign.center,
                  style: TextStyle(color: cs.onSurfaceVariant, height: 1.4)),
              const SizedBox(height: 20),
              Text(l.aboutCopyright,
                  textAlign: TextAlign.center,
                  style: TextStyle(color: cs.onSurface)),
              const SizedBox(height: 4),
              Text(l.aboutLicense,
                  style: TextStyle(color: cs.onSurfaceVariant)),
              const SizedBox(height: 16),
              OutlinedButton.icon(
                icon: const Icon(Icons.description_outlined, size: 18),
                label: Text(l.aboutLicensesButton),
                onPressed: () => showLicensePage(
                  context: context,
                  applicationName: l.appTitle,
                  applicationVersion: app.getAppVersion(),
                  applicationLegalese: l.aboutCopyright,
                ),
              ),
              const SizedBox(height: 16),
              Text(l.aboutMadeWith,
                  style: TextStyle(
                      color: cs.onSurfaceVariant.withValues(alpha: .8),
                      fontStyle: FontStyle.italic)),
            ],
          ),
        ),
      ),
    );
  }
}
