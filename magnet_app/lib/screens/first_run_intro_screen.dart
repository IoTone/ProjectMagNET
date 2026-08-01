// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:io' show Platform;

import 'package:flutter/foundation.dart' show kIsWeb;
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../gen/app_localizations.dart';
import '../perms/first_run_controller.dart';
import '../perms/permissions_service.dart';

/// First-run onboarding: explains why the app needs Bluetooth (+
/// location on Android) and offers Grant or Skip. Either choice marks
/// first-run done; the app gate then boots the shell.
class FirstRunIntroScreen extends StatelessWidget {
  const FirstRunIntroScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;

    Future<void> grant() async {
      final PermissionsService p = context.read<PermissionsService>();
      final FirstRunController fr = context.read<FirstRunController>();
      await p.requestBle();
      if (!kIsWeb && Platform.isAndroid) {
        await p.requestLocation();
      }
      await fr.markDone();
    }

    Future<void> skip() => context.read<FirstRunController>().markDone();

    return Scaffold(
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(28),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              const Spacer(),
              Icon(Icons.radar, size: 56, color: cs.primary),
              const SizedBox(height: 20),
              Text(l.firstRunTitle,
                  style: TextStyle(
                      color: cs.onSurface,
                      fontSize: 26,
                      fontWeight: FontWeight.w700)),
              const SizedBox(height: 12),
              Text(l.firstRunBody,
                  style: TextStyle(color: cs.onSurfaceVariant, height: 1.5)),
              const SizedBox(height: 20),
              _Bullet(icon: Icons.bluetooth, text: l.firstRunPermBle, cs: cs),
              const SizedBox(height: 10),
              _Bullet(
                  icon: Icons.location_on_outlined,
                  text: l.firstRunPermLocation,
                  cs: cs),
              const Spacer(),
              SizedBox(
                width: double.infinity,
                child: FilledButton(
                  onPressed: grant,
                  child: Text(l.firstRunGrant),
                ),
              ),
              const SizedBox(height: 8),
              SizedBox(
                width: double.infinity,
                child: TextButton(
                    onPressed: skip, child: Text(l.firstRunSkip)),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _Bullet extends StatelessWidget {
  const _Bullet({required this.icon, required this.text, required this.cs});
  final IconData icon;
  final String text;
  final ColorScheme cs;
  @override
  Widget build(BuildContext context) => Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Icon(icon, size: 20, color: cs.primary),
          const SizedBox(width: 12),
          Expanded(
            child: Text(text,
                style: TextStyle(color: cs.onSurface, height: 1.4)),
          ),
        ],
      );
}
