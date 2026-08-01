// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:flutter_native_splash/flutter_native_splash.dart';
import 'package:provider/provider.dart';

import 'package:magnet_app/app_router.dart';
import 'package:magnet_app/app_state_model.dart';
import 'package:magnet_app/gen/app_localizations.dart';
import 'package:magnet_app/l10n/locale_controller.dart';
import 'package:magnet_app/perms/first_run_controller.dart';
import 'package:magnet_app/perms/permissions_service.dart';
import 'package:magnet_app/scanner/ble_scanner.dart';
import 'package:magnet_app/scanner/scanner_controller.dart';
import 'package:magnet_app/scanner/wifi_sources.dart';
import 'package:magnet_app/screens/first_run_intro_screen.dart';
import 'package:magnet_app/splash/branded_splash_screen.dart';
import 'package:magnet_app/theme/theme_controller.dart';

void main() {
  final WidgetsBinding widgetsBinding =
      WidgetsFlutterBinding.ensureInitialized();
  FlutterNativeSplash.preserve(widgetsBinding: widgetsBinding);

  SystemChrome.setPreferredOrientations(<DeviceOrientation>[
    DeviceOrientation.portraitUp,
    DeviceOrientation.portraitDown,
  ]).then((_) {
    runApp(
      MultiProvider(
        providers: [
          ChangeNotifierProvider<AppState>(create: (_) => AppState()),
          ChangeNotifierProvider<ThemeController>(
            create: (_) => ThemeController()..load(),
          ),
          ChangeNotifierProvider<LocaleController>(
            create: (_) => LocaleController()..load(),
          ),
          ChangeNotifierProvider<FirstRunController>(
            create: (_) => FirstRunController()..load(),
          ),
          Provider<PermissionsService>(
            create: (_) => const PlatformPermissionsService(),
          ),
          ChangeNotifierProvider<ScannerController>(
            create: (_) => ScannerController(
              ble: FlutterBlueScanner(),
              wifi: createWifiSource(),
            ),
          ),
        ],
        child: const MyApp(),
      ),
    );
  });
}

/// Root widget: one MaterialApp + the go_router config. Theme,
/// font-scale, and locale come from their controllers.
class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  bool _minSplashElapsed = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      FlutterNativeSplash.remove();
    });
    Future<void>.delayed(const Duration(milliseconds: 1800), () {
      if (mounted) setState(() => _minSplashElapsed = true);
    });
  }

  static const List<LocalizationsDelegate<Object>> _localeDelegates =
      <LocalizationsDelegate<Object>>[
    AppLocalizations.delegate,
    GlobalMaterialLocalizations.delegate,
    GlobalWidgetsLocalizations.delegate,
    GlobalCupertinoLocalizations.delegate,
  ];

  @override
  Widget build(BuildContext context) {
    final ThemeController tc = context.watch<ThemeController>();
    final FirstRunController fr = context.watch<FirstRunController>();
    final LocaleController lc = context.watch<LocaleController>();

    if (!fr.loaded || !_minSplashElapsed) {
      return MaterialApp(
        theme: tc.theme,
        locale: lc.locale,
        supportedLocales: LocaleController.supported,
        localizationsDelegates: _localeDelegates,
        home: const BrandedSplashScreen(),
      );
    }
    if (!fr.done) {
      return MaterialApp(
        onGenerateTitle: (BuildContext c) => AppLocalizations.of(c).appTitle,
        theme: tc.theme,
        locale: lc.locale,
        supportedLocales: LocaleController.supported,
        localizationsDelegates: _localeDelegates,
        home: const FirstRunIntroScreen(),
      );
    }
    return MaterialApp.router(
      onGenerateTitle: (BuildContext c) => AppLocalizations.of(c).appTitle,
      theme: tc.theme,
      locale: lc.locale,
      supportedLocales: LocaleController.supported,
      localizationsDelegates: _localeDelegates,
      routerConfig: appRouter,
      builder: (BuildContext context, Widget? child) {
        final MediaQueryData mq = MediaQuery.of(context);
        final double osScale = mq.textScaler.scale(1.0);
        return MediaQuery(
          data: mq.copyWith(
            textScaler: TextScaler.linear(osScale * tc.fontScale),
          ),
          child: child ?? const SizedBox.shrink(),
        );
      },
    );
  }
}
