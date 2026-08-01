// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../app_state_model.dart';
import '../gen/app_localizations.dart';

/// About screen: app name, version, attribution, and license.
class AboutScreen extends StatelessWidget {
  const AboutScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final AppState app = context.watch<AppState>();
    final ColorScheme cs = Theme.of(context).colorScheme;

    return Scaffold(
      appBar: AppBar(title: Text(l.settingsAbout)),
      body: Center(
        child: Padding(
          padding: const EdgeInsets.all(28),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              Icon(Icons.radar, size: 64, color: cs.primary),
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
              const SizedBox(height: 20),
              Text(l.aboutCopyright,
                  textAlign: TextAlign.center,
                  style: TextStyle(color: cs.onSurface)),
              const SizedBox(height: 4),
              Text(l.aboutLicense,
                  style: TextStyle(color: cs.onSurfaceVariant)),
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
