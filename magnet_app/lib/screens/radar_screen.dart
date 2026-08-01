// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:provider/provider.dart';

import '../gen/app_localizations.dart';
import '../scanner/scanned_access_point.dart';
import '../scanner/scanned_device.dart';
import '../scanner/scanner_controller.dart';
import '../scanner/signal_distance.dart';
import '../theme/theme_controller.dart';
import 'device_detail_sheet.dart';

/// Radar scope: a signal-strength radial view of nearby BLE devices
/// and WiFi APs. Distance from centre = weaker signal (a rough RSSI
/// indicator, not a true range). Angle is a stable per-id hash so a
/// blip keeps its bearing across scans. A sweep line animates unless
/// reduce-motion is on. Tapping a BLE blip opens its detail sheet.
class RadarScreen extends StatefulWidget {
  const RadarScreen({super.key});

  @override
  State<RadarScreen> createState() => _RadarScreenState();
}

/// A positioned radar blip (resolved each layout for paint + hit-test).
class _Blip {
  const _Blip(this.offset, {required this.isWifi, this.deviceId});
  final Offset offset;
  final bool isWifi;
  final String? deviceId;
}

class _RadarScreenState extends State<RadarScreen>
    with SingleTickerProviderStateMixin {
  late final Ticker _ticker;
  double _sweep = 0; // 0..1

  @override
  void initState() {
    super.initState();
    _ticker = Ticker((Duration elapsed) {
      final double next = (elapsed.inMilliseconds % 4000) / 4000.0;
      if ((next - _sweep).abs() > 0.004) setState(() => _sweep = next);
    })..start();
  }

  @override
  void dispose() {
    _ticker.dispose();
    super.dispose();
  }

  /// Stable angle in radians from an identifier string.
  static double _angleFor(String key) {
    int h = 0;
    for (final int c in key.codeUnits) {
      h = (h * 31 + c) & 0x7fffffff;
    }
    return (h % 36000) / 36000.0 * 2 * math.pi;
  }

  @override
  Widget build(BuildContext context) {
    final ScannerController sc = context.watch<ScannerController>();
    final bool reduceMotion = context.watch<ThemeController>().reduceMotion;
    final ColorScheme cs = Theme.of(context).colorScheme;
    final AppLocalizations l = AppLocalizations.of(context);
    final List<ScannedDevice> ble = sc.devices;
    final List<ScannedAccessPoint> wifi = sc.accessPoints;

    if (ble.isEmpty && wifi.isEmpty) {
      return Center(
        child: Text(l.radarEmpty,
            style: TextStyle(color: cs.onSurfaceVariant)),
      );
    }

    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints c) {
        final Size size = Size(c.maxWidth, c.maxHeight);
        final Offset center = size.center(Offset.zero);
        final double maxR = math.min(size.width, size.height) / 2 - 28;

        Offset place(int rssi, String key) {
          final double r = SignalDistance.radialFraction(rssi) * maxR;
          final double a = _angleFor(key);
          return center + Offset(r * math.sin(a), -r * math.cos(a));
        }

        final List<_Blip> blips = <_Blip>[
          for (final ScannedDevice d in ble)
            _Blip(place(d.rssi, d.id), isWifi: false, deviceId: d.id),
          for (final ScannedAccessPoint ap in wifi)
            _Blip(place(ap.rssi, ap.bssid), isWifi: true),
        ];

        return Stack(
          children: <Widget>[
            Positioned.fill(
              child: GestureDetector(
                onTapUp: (TapUpDetails details) {
                  final Offset p = details.localPosition;
                  _Blip? hit;
                  double best = 28 * 28;
                  for (final _Blip b in blips) {
                    if (b.deviceId == null) continue;
                    final double d2 = (b.offset - p).distanceSquared;
                    if (d2 < best) {
                      best = d2;
                      hit = b;
                    }
                  }
                  if (hit != null) {
                    showModalBottomSheet<void>(
                      context: context,
                      isScrollControlled: true,
                      showDragHandle: true,
                      builder: (_) =>
                          DeviceDetailSheet(deviceId: hit!.deviceId!),
                    );
                  }
                },
                child: CustomPaint(
                  size: size,
                  painter: _RadarPainter(
                    blips: blips,
                    center: center,
                    maxR: maxR,
                    sweep: reduceMotion ? null : _sweep,
                    accent: cs.primary,
                    wifiColor: cs.secondary,
                    ring: cs.outline.withValues(alpha: .4),
                    subtle: cs.outline.withValues(alpha: .18),
                    label: cs.onSurfaceVariant,
                  ),
                ),
              ),
            ),
            Positioned(
              left: 12,
              bottom: 12,
              child: _Legend(cs: cs, l: l),
            ),
          ],
        );
      },
    );
  }
}

class _Legend extends StatelessWidget {
  const _Legend({required this.cs, required this.l});
  final ColorScheme cs;
  final AppLocalizations l;
  @override
  Widget build(BuildContext context) {
    Widget row(Color c, bool diamond, String text) => Padding(
          padding: const EdgeInsets.symmetric(vertical: 2),
          child: Row(mainAxisSize: MainAxisSize.min, children: <Widget>[
            Icon(diamond ? Icons.diamond : Icons.circle, size: 9, color: c),
            const SizedBox(width: 6),
            Text(text,
                style: TextStyle(color: cs.onSurfaceVariant, fontSize: 11)),
          ]),
        );
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
      decoration: BoxDecoration(
        color: cs.surface.withValues(alpha: .82),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: cs.outline.withValues(alpha: .4)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          row(cs.primary, false, l.radarLegendBle),
          row(cs.secondary, true, l.radarLegendWifi),
          const SizedBox(height: 2),
          Text(l.radarLegendRings,
              style: TextStyle(color: cs.onSurfaceVariant, fontSize: 10)),
        ],
      ),
    );
  }
}

class _RadarPainter extends CustomPainter {
  _RadarPainter({
    required this.blips,
    required this.center,
    required this.maxR,
    required this.sweep,
    required this.accent,
    required this.wifiColor,
    required this.ring,
    required this.subtle,
    required this.label,
  });

  final List<_Blip> blips;
  final Offset center;
  final double maxR;
  final double? sweep;
  final Color accent;
  final Color wifiColor;
  final Color ring;
  final Color subtle;
  final Color label;

  static const List<double> _ringFracs = <double>[1 / 3, 2 / 3, 1.0];

  @override
  void paint(Canvas canvas, Size size) {
    if (maxR <= 0) return;

    // Range rings + approximate dBm labels (inner = stronger).
    final Paint ringPaint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5
      ..color = ring;
    for (final double f in _ringFracs) {
      canvas.drawCircle(center, maxR * f, ringPaint);
      final int dbm = (-40 - f * 60).round(); // -40 strong … -100 weak
      final TextPainter tp = TextPainter(
        text: TextSpan(
          text: '$dbm',
          style: TextStyle(color: label, fontSize: 10, fontFamily: 'monospace'),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      tp.paint(canvas,
          Offset(center.dx + maxR * f - tp.width - 4, center.dy - tp.height - 2));
    }

    // Cross-hair guides.
    final Paint cross = Paint()
      ..strokeWidth = 1
      ..color = subtle;
    canvas.drawLine(Offset(center.dx - maxR, center.dy),
        Offset(center.dx + maxR, center.dy), cross);
    canvas.drawLine(Offset(center.dx, center.dy - maxR),
        Offset(center.dx, center.dy + maxR), cross);

    // Sweep line + trailing wedge.
    final double? s = sweep;
    if (s != null) {
      final double a = s * 2 * math.pi - math.pi / 2;
      final Rect rect = Rect.fromCircle(center: center, radius: maxR);
      canvas.drawArc(
        rect,
        a - 0.5,
        0.5,
        true,
        Paint()..color = accent.withValues(alpha: .10),
      );
      canvas.drawLine(
        center,
        center + Offset(maxR * math.cos(a), maxR * math.sin(a)),
        Paint()
          ..strokeWidth = 1.5
          ..color = accent.withValues(alpha: .5),
      );
    }

    // Self marker.
    canvas.drawCircle(center, 5, Paint()..color = accent);
    canvas.drawCircle(
        center,
        9,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1.5
          ..color = accent.withValues(alpha: .6));

    // Blips.
    for (final _Blip b in blips) {
      final Color col = b.isWifi ? wifiColor : accent;
      if (b.isWifi) {
        // Small diamond for WiFi.
        final Path p = Path()
          ..moveTo(b.offset.dx, b.offset.dy - 5)
          ..lineTo(b.offset.dx + 5, b.offset.dy)
          ..lineTo(b.offset.dx, b.offset.dy + 5)
          ..lineTo(b.offset.dx - 5, b.offset.dy)
          ..close();
        canvas.drawPath(p, Paint()..color = col);
      } else {
        canvas.drawCircle(b.offset, 4.5, Paint()..color = col);
      }
    }
  }

  @override
  bool shouldRepaint(covariant _RadarPainter old) =>
      old.sweep != sweep || !identical(old.blips, blips);
}
