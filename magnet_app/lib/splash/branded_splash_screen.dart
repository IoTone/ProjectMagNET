// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:package_info_plus/package_info_plus.dart';

/// Branded Flutter-side splash. The native splash (`flutter_native_splash`)
/// shows for the few hundred ms between OS app-start and the first
/// Flutter frame; this widget then takes over with the animated brand
/// mark until `MyApp` is ready to mount the real home tree.
///
/// The mark is the same magnetic dipole as the app icon, painted rather
/// than loaded from an asset so it stays crisp at any size — with the
/// field pulsing outward, which is the one motion a magnet suggests.
///
/// Pure presentation — owns its animation + a version-read future. The
/// min-display gate lives in `main.dart` so this stays reusable.
class BrandedSplashScreen extends StatefulWidget {
  const BrandedSplashScreen({super.key});

  @override
  State<BrandedSplashScreen> createState() => _BrandedSplashScreenState();
}

class _BrandedSplashScreenState extends State<BrandedSplashScreen>
    with SingleTickerProviderStateMixin {
  late final AnimationController _pulse = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 2600),
  )..repeat();

  final Future<PackageInfo> _info = PackageInfo.fromPlatform();

  @override
  void dispose() {
    _pulse.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final ThemeData t = Theme.of(context);
    final ColorScheme cs = t.colorScheme;
    return Scaffold(
      backgroundColor: cs.surface,
      body: SafeArea(
        child: Center(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              // Flutter-rendered brand mark (no asset dep so it
              // always reflects the active theme + doesn't fall
              // back to a stale launcher template).
              AnimatedBuilder(
                animation: _pulse,
                builder: (BuildContext _, Widget? __) => CustomPaint(
                  size: const Size(120, 120),
                  painter: _MagnetMarkPainter(phase: _pulse.value),
                ),
              ),
              const SizedBox(height: 24),
              Text(
                'MagNET',
                style: t.textTheme.headlineSmall?.copyWith(
                  color: cs.onSurface,
                  letterSpacing: 2,
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(height: 6),
              Text(
                'mesh nodes, provisioned',
                style: t.textTheme.labelMedium?.copyWith(
                  color: cs.onSurfaceVariant,
                  letterSpacing: 1,
                ),
              ),
              const SizedBox(height: 18),
              FutureBuilder<PackageInfo>(
                future: _info,
                builder: (BuildContext _, AsyncSnapshot<PackageInfo> snap) {
                  final String label = snap.hasData
                      ? 'v${snap.data!.version}+${snap.data!.buildNumber}'
                      : '';
                  return Text(
                    label,
                    style: t.textTheme.labelSmall?.copyWith(
                      color: cs.onSurfaceVariant.withValues(alpha: .7),
                      fontFamily: 'monospace',
                      letterSpacing: 2,
                    ),
                  );
                },
              ),
            ],
          ),
        ),
      ),
    );
  }
}

/// The MagNET mark: a magnetic dipole field, painted.
///
/// The loops are the real dipole solution r = L·sin²θ (stretched on the
/// axis so the mark sits well in a square, exactly as the icon art does),
/// and [phase] drives a highlight travelling outward through them — the
/// field "radiating" rather than the logo spinning.
class _MagnetMarkPainter extends CustomPainter {
  _MagnetMarkPainter({required this.phase});

  /// 0..1, wraps continuously.
  final double phase;

  static const List<Color> _ramp = <Color>[
    Color(0xFF22D3EE), // cyan at the poles
    Color(0xFF6366F1),
    Color(0xFFA78BFA),
    Color(0xFFF472B6), // magenta at the equator
  ];
  static const double _yStretch = 1.55;

  Color _rampAt(double t) {
    final double x = (t.clamp(0.0, 1.0)) * (_ramp.length - 1);
    final int i = x.floor().clamp(0, _ramp.length - 2);
    return Color.lerp(_ramp[i], _ramp[i + 1], x - i)!;
  }

  Path _fieldLine(Offset c, double l, bool mirror) {
    final Path p = Path();
    const int n = 96;
    for (int i = 0; i <= n; i++) {
      final double th = math.pi * i / n;
      final double r = l * math.pow(math.sin(th), 2);
      final double dx = r * math.sin(th) * (mirror ? -1 : 1);
      final double dy = r * math.cos(th) * _yStretch;
      final Offset pt = Offset(c.dx + dx, c.dy - dy);
      i == 0 ? p.moveTo(pt.dx, pt.dy) : p.lineTo(pt.dx, pt.dy);
    }
    return p;
  }

  @override
  void paint(Canvas canvas, Size size) {
    final Offset c = size.center(Offset.zero);
    final double r = math.min(size.width, size.height) / 2;

    for (final (int idx, double scale) in <double>[0.60, 1.00].indexed) {
      final double l = r * 0.80 * scale;
      // each loop lights up in turn as the pulse travels outward
      final double lead = (phase - idx * 0.28) % 1.0;
      final double glow = math.max(0, 1 - (lead * 3).clamp(0.0, 3.0));
      final Color col = _rampAt(0.35 + scale * 0.5);

      for (final bool mirror in <bool>[false, true]) {
        final Path path = _fieldLine(c, l, mirror);
        canvas.drawPath(
          path,
          Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = 5 + 3 * glow
            ..maskFilter = MaskFilter.blur(BlurStyle.normal, 4 + 8 * glow)
            ..color = col.withValues(alpha: .30 + .45 * glow),
        );
        canvas.drawPath(
          path,
          Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = 2.4
            ..color = col.withValues(alpha: .75 + .25 * glow),
        );
      }
    }

    // the magnet itself
    final double h = r * 0.62, w = r * 0.14;
    final RRect body = RRect.fromRectAndRadius(
      Rect.fromCenter(center: c, width: w * 2, height: h * 2),
      Radius.circular(w),
    );
    canvas.drawRRect(
      body,
      Paint()
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 10)
        ..color = _ramp.first.withValues(alpha: .55),
    );
    canvas.drawRRect(body, Paint()..color = const Color(0xFFE2E8F0));
    canvas.save();
    canvas.clipRRect(body);
    canvas.drawRect(
      Rect.fromLTRB(c.dx - w, c.dy - h, c.dx + w, c.dy - h * 0.34),
      Paint()..color = _ramp.first,
    );
    canvas.drawRect(
      Rect.fromLTRB(c.dx - w, c.dy + h * 0.34, c.dx + w, c.dy + h),
      Paint()..color = _ramp.last,
    );
    canvas.restore();
  }

  @override
  bool shouldRepaint(covariant _MagnetMarkPainter old) => old.phase != phase;
}
