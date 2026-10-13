// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:go_router/go_router.dart';
import 'package:provider/provider.dart';

import '../app_state_model.dart';
import '../gen/app_localizations.dart';
import '../l10n/locale_controller.dart';
import '../perms/permissions_service.dart';
import '../scanner/scanner_controller.dart';
import '../scanner/wifi_proxy_source.dart';
import '../scanner/wifi_sources.dart';
import '../theme/theme_controller.dart';
import '../theme/tokens.dart';

/// Settings hub: appearance/theme, language, permissions, a link to
/// Diagnostics, and About. Demonstrates the theming + l10n pipelines.
class SettingsScreen extends StatelessWidget {
  const SettingsScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final ThemeController tc = context.watch<ThemeController>();
    final LocaleController lc = context.watch<LocaleController>();
    final ScannerController sc = context.watch<ScannerController>();
    final AppState app = context.watch<AppState>();
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;

    return ListView(
      children: <Widget>[
        _Header(l.settingsScanner, cs: cs),
        ListTile(
          title: Text(l.settingsRssiFloor),
          subtitle: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              Text(sc.rssiFloor <= -100
                  ? l.settingsRssiFloorOff
                  : l.settingsRssiFloorValue(sc.rssiFloor)),
              Slider(
                value: sc.rssiFloor.toDouble(),
                min: -100,
                max: -40,
                divisions: 12,
                label: '${sc.rssiFloor} dBm',
                onChanged: (double v) => sc.setRssiFloor(v.round()),
              ),
            ],
          ),
        ),
        const _ProxyToggle(),
        _Header(l.settingsAppearance, cs: cs),
        ListTile(
          title: Text(l.settingsTheme),
          trailing: DropdownButton<AppThemePreset>(
            value: tc.preset,
            underline: const SizedBox.shrink(),
            onChanged: (AppThemePreset? v) {
              if (v != null) tc.setPreset(v);
            },
            items: <DropdownMenuItem<AppThemePreset>>[
              for (final AppThemePreset p in AppThemePreset.values)
                DropdownMenuItem<AppThemePreset>(
                    value: p, child: Text(p.label)),
            ],
          ),
        ),
        ListTile(
          title: Text(l.settingsFontScale),
          subtitle: Slider(
            value: tc.fontScale,
            min: 0.8,
            max: 1.6,
            divisions: 8,
            label: '${(tc.fontScale * 100).round()}%',
            onChanged: tc.setFontScale,
          ),
        ),
        SwitchListTile(
          title: Text(l.settingsReduceMotion),
          value: tc.reduceMotion,
          onChanged: tc.setReduceMotion,
        ),
        SwitchListTile(
          title: Text(l.settingsHighContrast),
          value: tc.highContrast,
          onChanged: tc.setHighContrast,
        ),
        _Header(l.settingsLanguage, cs: cs),
        _LangTile(
            label: l.settingsLanguageSystem,
            selected: lc.locale == null,
            onTap: () => lc.set(null)),
        _LangTile(
            label: l.langEnglish,
            selected: lc.locale?.languageCode == 'en',
            onTap: () => lc.set(const Locale('en'))),
        _LangTile(
            label: l.langJapanese,
            selected: lc.locale?.languageCode == 'ja',
            onTap: () => lc.set(const Locale('ja'))),
        _Header(l.settingsPermissions, cs: cs),
        const _PermissionTile(),
        _Header(l.settingsDiagnostics, cs: cs),
        ListTile(
          leading: const Icon(Icons.list_alt),
          title: Text(l.settingsDiagnostics),
          subtitle: Text(l.settingsDiagnosticsSub),
          trailing: const Icon(Icons.chevron_right),
          onTap: () => context.push('/diagnostics'),
        ),
        ListTile(
          leading: const Icon(Icons.hub_outlined),
          title: Text(l.provisionLogTitle),
          subtitle: Text(l.provisionLogSub),
          trailing: const Icon(Icons.chevron_right),
          onTap: () => context.push('/provisioned'),
        ),
        _Header(l.settingsAbout, cs: cs),
        ListTile(
          leading: const Icon(Icons.info_outline),
          title: Text(l.settingsAbout),
          subtitle: Text('${l.appTitle} · ${l.aboutVersion(app.getAppVersion())}'),
          trailing: const Icon(Icons.chevron_right),
          onTap: () => context.push('/about'),
        ),
      ],
    );
  }
}

class _Header extends StatelessWidget {
  const _Header(this.text, {required this.cs});
  final String text;
  final ColorScheme cs;
  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.fromLTRB(16, 18, 16, 4),
        child: Text(text,
            style: TextStyle(
                color: cs.primary, fontSize: 12, letterSpacing: 3)),
      );
}

/// Toggles the WiFi source between the on-device source and the mDNS
/// relay proxy. Local UI state — resets on relaunch (a starter-grade
/// affordance; persist it if you ship this).
class _ProxyToggle extends StatefulWidget {
  const _ProxyToggle();
  @override
  State<_ProxyToggle> createState() => _ProxyToggleState();
}

class _ProxyToggleState extends State<_ProxyToggle> {
  bool _on = false;

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final ScannerController sc = context.read<ScannerController>();
    return SwitchListTile(
      title: Text(l.settingsWifiProxy),
      subtitle: Text(l.settingsWifiProxySub),
      value: _on,
      onChanged: (bool v) {
        setState(() => _on = v);
        sc.setWifiSource(v ? ProxyWifiSource() : createWifiSource());
      },
    );
  }
}

class _LangTile extends StatelessWidget {
  const _LangTile(
      {required this.label, required this.selected, required this.onTap});
  final String label;
  final bool selected;
  final VoidCallback onTap;
  @override
  Widget build(BuildContext context) {
    final ColorScheme cs = Theme.of(context).colorScheme;
    return ListTile(
      title: Text(label),
      trailing: selected ? Icon(Icons.check, color: cs.primary) : null,
      onTap: onTap,
    );
  }
}

class _PermissionTile extends StatefulWidget {
  const _PermissionTile();
  @override
  State<_PermissionTile> createState() => _PermissionTileState();
}

class _PermissionTileState extends State<_PermissionTile> {
  bool? _ble;
  bool? _loc;

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  Future<void> _refresh() async {
    final PermissionsService p = context.read<PermissionsService>();
    final bool ble = await p.bleGranted();
    final bool loc = await p.locationGranted();
    if (mounted) {
      setState(() {
        _ble = ble;
        _loc = loc;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final PermissionsService p = context.read<PermissionsService>();
    return Column(
      children: <Widget>[
        _row(l.permBle, _ble, () async {
          await p.requestBle();
          await _refresh();
        }, l),
        _row(l.permLocation, _loc, () async {
          await p.requestLocation();
          await _refresh();
        }, l),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
          child: Text(l.permLocationWhy,
              style: TextStyle(
                  color: Theme.of(context).colorScheme.onSurfaceVariant,
                  fontSize: 12)),
        ),
      ],
    );
  }

  Widget _row(String label, bool? granted, VoidCallback onRequest,
      AppLocalizations l) {
    return ListTile(
      title: Text(label),
      subtitle: Text(granted == null
          ? '…'
          : granted
              ? l.permGranted
              : l.permDenied),
      trailing: granted == true
          ? const Icon(Icons.check_circle, color: Colors.green)
          : TextButton(onPressed: onRequest, child: Text(l.permRequest)),
    );
  }
}

