// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'package:flutter/material.dart';

/// Bundled dark theme presets. Screens reference only the semantic
/// [AppTokens] roles (never hard-coded colours), so every preset
/// re-skins the whole UI. [AppThemePreset.midnight] is the default and
/// the high-contrast benchmark.
enum AppThemePreset { midnight, indigo, cyan, aqua, magenta, amber }

extension AppThemePresetX on AppThemePreset {
  String get label => switch (this) {
        AppThemePreset.midnight => 'Midnight',
        AppThemePreset.indigo => 'Indigo',
        AppThemePreset.cyan => 'Cyan',
        AppThemePreset.aqua => 'Aqua',
        AppThemePreset.magenta => 'Magenta',
        AppThemePreset.amber => 'Amber',
      };
}

/// Preset-independent semantic colour contract.
@immutable
class AppTokens {
  const AppTokens({
    required this.base,
    required this.surface,
    required this.surfaceAlt,
    required this.line,
    required this.fg,
    required this.fgMuted,
    required this.accent,
    required this.alert,
    required this.ok,
  });

  final Color base;
  final Color surface;
  final Color surfaceAlt;
  final Color line;
  final Color fg;
  final Color fgMuted;
  final Color accent;
  final Color alert;
  final Color ok;
}

const Map<AppThemePreset, AppTokens> kAppThemes = <AppThemePreset, AppTokens>{
  // Default — near-black with a warm off-white, high contrast.
  AppThemePreset.midnight: AppTokens(
    base: Color(0xFF000000),
    surface: Color(0xFF0E0E0C),
    surfaceAlt: Color(0xFF15140F),
    line: Color(0xFF2A2A26),
    fg: Color(0xFFEDE6D6),
    fgMuted: Color(0xFF9A958A),
    accent: Color(0xFFEDE6D6),
    alert: Color(0xFFC8102E),
    ok: Color(0xFF9A958A),
  ),
  AppThemePreset.indigo: AppTokens(
    base: Color(0xFF0A0E1A),
    surface: Color(0xFF121826),
    surfaceAlt: Color(0xFF1B2436),
    line: Color(0xFF243049),
    fg: Color(0xFFE6ECF5),
    fgMuted: Color(0xFF7C8AA3),
    accent: Color(0xFFFF7A00),
    alert: Color(0xFFE6005C),
    ok: Color(0xFF9CFF00),
  ),
  AppThemePreset.cyan: AppTokens(
    base: Color(0xFF05060B),
    surface: Color(0xFF10131F),
    surfaceAlt: Color(0xFF161B2A),
    line: Color(0xFF20283B),
    fg: Color(0xFFDDF6FF),
    fgMuted: Color(0xFF6F8196),
    accent: Color(0xFF22D3EE),
    alert: Color(0xFFFF2D78),
    ok: Color(0xFF22D3EE),
  ),
  AppThemePreset.aqua: AppTokens(
    base: Color(0xFF0B0F17),
    surface: Color(0xFF161B26),
    surfaceAlt: Color(0xFF1E2533),
    line: Color(0xFF263041),
    fg: Color(0xFFDDE7EF),
    fgMuted: Color(0xFF7C8AA3),
    accent: Color(0xFF35E0F0),
    alert: Color(0xFFFF3B6B),
    ok: Color(0xFF7CFF6B),
  ),
  AppThemePreset.magenta: AppTokens(
    base: Color(0xFF101014),
    surface: Color(0xFF1C1C24),
    surfaceAlt: Color(0xFF24242E),
    line: Color(0xFF33333F),
    fg: Color(0xFFF2F0E6),
    fgMuted: Color(0xFF8A8A9A),
    accent: Color(0xFFFF2E88),
    alert: Color(0xFFFF2E88),
    ok: Color(0xFFD7FF00),
  ),
  AppThemePreset.amber: AppTokens(
    base: Color(0xFF000000),
    surface: Color(0xFF0A0A07),
    surfaceAlt: Color(0xFF141414),
    line: Color(0xFF6E4E00),
    fg: Color(0xFFFFB000),
    fgMuted: Color(0xFF6E4E00),
    accent: Color(0xFFFFB000),
    alert: Color(0xFFB3231F),
    ok: Color(0xFFFFB000),
  ),
};

AppThemePreset presetFromId(String? id) => AppThemePreset.values.firstWhere(
      (AppThemePreset p) => p.name == id,
      orElse: () => AppThemePreset.midnight,
    );

/// Build a Material 3 dark [ThemeData] from a token set. Binds every
/// commonly-used colour role (including the M3 surface-container roles)
/// so switching presets re-skins the whole UI.
ThemeData buildAppTheme(AppTokens t) {
  final ColorScheme scheme = ColorScheme(
    brightness: Brightness.dark,
    surface: t.base,
    onSurface: t.fg,
    surfaceContainerLowest: t.base,
    surfaceContainerLow: t.surface,
    surfaceContainer: t.surface,
    surfaceContainerHigh: t.surfaceAlt,
    surfaceContainerHighest: t.surfaceAlt,
    surfaceDim: t.base,
    surfaceBright: t.surfaceAlt,
    onSurfaceVariant: t.fgMuted,
    primary: t.accent,
    onPrimary: t.base,
    primaryContainer: t.surfaceAlt,
    onPrimaryContainer: t.accent,
    secondary: t.accent,
    onSecondary: t.base,
    secondaryContainer: t.surfaceAlt,
    onSecondaryContainer: t.fg,
    tertiary: t.ok,
    onTertiary: t.base,
    error: t.alert,
    onError: t.fg,
    errorContainer: t.alert,
    onErrorContainer: t.fg,
    outline: t.line,
    outlineVariant: t.line,
    shadow: const Color(0xFF000000),
    scrim: const Color(0xCC000000),
    inverseSurface: t.fg,
    onInverseSurface: t.base,
    inversePrimary: t.base,
    surfaceTint: t.accent,
  );
  final TextTheme text = ThemeData(brightness: Brightness.dark)
      .textTheme
      .apply(bodyColor: t.fg, displayColor: t.fg, decorationColor: t.fg);
  return ThemeData(
    useMaterial3: true,
    brightness: Brightness.dark,
    colorScheme: scheme,
    scaffoldBackgroundColor: t.base,
    canvasColor: t.base,
    dividerColor: t.line,
    cardColor: t.surface,
    textTheme: text,
    primaryTextTheme: text,
    iconTheme: IconThemeData(color: t.fg),
    primaryIconTheme: IconThemeData(color: t.fg),
    cardTheme: CardThemeData(
      color: t.surface,
      surfaceTintColor: Colors.transparent,
    ),
    appBarTheme: AppBarTheme(
      backgroundColor: t.base,
      foregroundColor: t.fg,
      surfaceTintColor: Colors.transparent,
      iconTheme: IconThemeData(color: t.fg),
      titleTextStyle: text.titleLarge,
      elevation: 0,
    ),
    listTileTheme: ListTileThemeData(iconColor: t.fgMuted, textColor: t.fg),
    dividerTheme: DividerThemeData(color: t.line, space: 1),
    switchTheme: SwitchThemeData(
      thumbColor: WidgetStateProperty.resolveWith((Set<WidgetState> s) =>
          s.contains(WidgetState.selected) ? t.accent : t.fgMuted),
      trackColor: WidgetStateProperty.resolveWith((Set<WidgetState> s) =>
          s.contains(WidgetState.selected)
              ? t.accent.withValues(alpha: .4)
              : t.surfaceAlt),
    ),
    sliderTheme: SliderThemeData(
      activeTrackColor: t.accent,
      thumbColor: t.accent,
      inactiveTrackColor: t.line,
    ),
  );
}
