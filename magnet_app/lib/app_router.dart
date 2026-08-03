// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:go_router/go_router.dart';

import 'screens/about_screen.dart';
import 'screens/diagnostics_screen.dart';
import 'screens/magnet_node_screen.dart';
import 'screens/mesh_screen.dart';
import 'screens/provision_log_screen.dart';
import 'shell/home_shell.dart';

/// App routes. `/` hosts the swipe shell; sub-pages are pushed on top.
final GoRouter appRouter = GoRouter(
  initialLocation: '/',
  routes: <RouteBase>[
    GoRoute(path: '/', builder: (_, __) => const HomeShell()),
    GoRoute(
      path: '/diagnostics',
      builder: (_, __) => const DiagnosticsScreen(),
    ),
    GoRoute(path: '/about', builder: (_, __) => const AboutScreen()),
    // M3: live mesh view through the resident-BLE companion node.
    GoRoute(path: '/mesh', builder: (_, __) => const MeshScreen()),
    // M1: local ledger of provisioned nodes.
    GoRoute(
        path: '/provisioned',
        builder: (_, __) => const ProvisionLogScreen()),
    // MagNET node configuration. `id` is the platform device id; the name is
    // passed along so the app bar reads well before the node answers.
    GoRoute(
      path: '/node/:id',
      builder: (_, GoRouterState s) => MagnetNodeScreen(
        deviceId: s.pathParameters['id']!,
        deviceName: s.uri.queryParameters['name'],
      ),
    ),
  ],
);
