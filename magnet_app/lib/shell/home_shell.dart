// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:go_router/go_router.dart';
import 'package:provider/provider.dart';

import '../gen/app_localizations.dart';
import '../scanner/ble_scanner.dart';
import '../scanner/scanner_controller.dart';
import '../screens/dashboard_screen.dart';
import '../screens/devices_screen.dart';
import '../screens/radar_screen.dart';
import '../screens/settings_screen.dart';
import '../theme/theme_controller.dart';

/// The primary views.
enum AppView { dashboard, radar, devices, settings }

extension on AppView {
  String title(AppLocalizations l) => switch (this) {
        AppView.dashboard => l.tabDashboard,
        AppView.radar => l.tabRadar,
        AppView.devices => l.tabDevices,
        AppView.settings => l.tabSettings,
      };
  IconData get icon => switch (this) {
        AppView.dashboard => Icons.dashboard_outlined,
        AppView.radar => Icons.radar,
        AppView.devices => Icons.devices_other_outlined,
        AppView.settings => Icons.settings_outlined,
      };
}

/// Primary navigation shell: horizontal swipe between views; the
/// app-bar leading control opens a quick-nav sheet. Back from a
/// non-first tab returns to the Dashboard.
class HomeShell extends StatefulWidget {
  const HomeShell({super.key});

  @override
  State<HomeShell> createState() => _HomeShellState();
}

class _HomeShellState extends State<HomeShell> {
  final PageController _pc = PageController();
  int _index = 0;

  @override
  void dispose() {
    _pc.dispose();
    super.dispose();
  }

  void _goTo(int i, {required bool reduceMotion}) {
    if (reduceMotion) {
      _pc.jumpToPage(i);
    } else {
      _pc.animateToPage(i,
          duration: const Duration(milliseconds: 250), curve: Curves.easeOut);
    }
  }

  Future<void> _openQuickNav() async {
    final bool reduceMotion = context.read<ThemeController>().reduceMotion;
    final AppLocalizations l = AppLocalizations.of(context);
    await showModalBottomSheet<void>(
      context: context,
      showDragHandle: true,
      builder: (BuildContext ctx) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            for (final AppView v in AppView.values)
              ListTile(
                leading: Icon(v.icon),
                title: Text(v.title(l)),
                selected: v.index == _index,
                onTap: () {
                  Navigator.of(ctx).pop();
                  _goTo(v.index, reduceMotion: reduceMotion);
                },
              ),
          ],
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final ColorScheme cs = Theme.of(context).colorScheme;
    final AppLocalizations l = AppLocalizations.of(context);
    final AppView current = AppView.values[_index];
    return PopScope(
      canPop: _index == 0,
      onPopInvokedWithResult: (bool didPop, Object? result) {
        if (!didPop) {
          _goTo(0, reduceMotion: context.read<ThemeController>().reduceMotion);
        }
      },
      child: Scaffold(
        appBar: AppBar(
          leading: Semantics(
            button: true,
            label: l.quickNav,
            child: InkWell(
              onTap: _openQuickNav,
              onLongPress: _openQuickNav,
              customBorder: const CircleBorder(),
              child: const Icon(Icons.menu_open),
            ),
          ),
          title: Text(current.title(l)),
          actions: const <Widget>[ScanStatusIndicator()],
          bottom: PreferredSize(
            preferredSize: const Size.fromHeight(6),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: <Widget>[
                for (final AppView v in AppView.values)
                  Padding(
                    padding:
                        const EdgeInsets.symmetric(horizontal: 3, vertical: 4),
                    child: GestureDetector(
                      onTap: () => _goTo(v.index,
                          reduceMotion:
                              context.read<ThemeController>().reduceMotion),
                      child: Container(
                        width: v.index == _index ? 18 : 6,
                        height: 4,
                        decoration: BoxDecoration(
                          color: v.index == _index
                              ? cs.primary
                              : cs.onSurface.withValues(alpha: .3),
                          borderRadius: BorderRadius.circular(2),
                        ),
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ),
        body: PageView(
          controller: _pc,
          onPageChanged: (int i) => setState(() => _index = i),
          children: const <Widget>[
            DashboardScreen(),
            RadarScreen(),
            DevicesScreen(),
            SettingsScreen(),
          ],
        ),
      ),
    );
  }
}

/// Always-visible scan state in the app bar; tap → Diagnostics.
class ScanStatusIndicator extends StatelessWidget {
  const ScanStatusIndicator({super.key});

  @override
  Widget build(BuildContext context) {
    final ScannerController sc = context.watch<ScannerController>();
    final ColorScheme cs = Theme.of(context).colorScheme;
    final AppLocalizations l = AppLocalizations.of(context);

    late final String label;
    late final Color color;
    late final IconData icon;
    if (sc.adapterState == BleAdapterState.off) {
      label = l.bluetoothOff;
      color = cs.error;
      icon = Icons.bluetooth_disabled;
    } else if (sc.adapterState == BleAdapterState.unauthorized) {
      label = l.bluetoothUnauthorized;
      color = cs.error;
      icon = Icons.bluetooth_disabled;
    } else if (sc.isScanning) {
      label = l.scanScanning;
      color = cs.tertiary;
      icon = Icons.bluetooth_searching;
    } else {
      label = l.scanIdle;
      color = cs.onSurfaceVariant;
      icon = Icons.bluetooth;
    }

    return Semantics(
      button: true,
      label: label,
      child: InkWell(
        onTap: () => context.push('/diagnostics'),
        borderRadius: BorderRadius.circular(20),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 12),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              Container(
                width: 9,
                height: 9,
                decoration:
                    BoxDecoration(color: color, shape: BoxShape.circle),
              ),
              const SizedBox(width: 6),
              Icon(icon, size: 16, color: color),
            ],
          ),
        ),
      ),
    );
  }
}
