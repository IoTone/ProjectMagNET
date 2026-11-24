// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:math';

import 'package:flutter/material.dart';

/// One labelled x position on a [BarSeriesChart].
class BarTick {
  const BarTick(this.index, this.label);
  final int index;
  final String label;
}

/// A single-series bar chart for the Sentinel's time series: thin bars with
/// rounded tops sitting on the baseline, a faint grid, and tap / drag to
/// read one bar. Nothing is selected at rest; [summary] says what the
/// chart adds up to, and [describe] what one bar means.
class BarSeriesChart extends StatefulWidget {
  const BarSeriesChart({
    super.key,
    required this.values,
    required this.maxY,
    required this.yTicks,
    required this.yLabel,
    required this.xTicks,
    required this.summary,
    required this.describe,
    this.height = 120,
    this.highlightLast = false,
  });

  final List<double> values;
  final double maxY;

  /// Grid lines (in data units), labelled at the right edge.
  final List<double> yTicks;
  final String Function(double) yLabel;
  final List<BarTick> xTicks;
  final String summary;
  final String Function(int index) describe;
  final double height;

  /// Mark the newest bar as still filling (drawn lighter).
  final bool highlightLast;

  @override
  State<BarSeriesChart> createState() => _BarSeriesChartState();
}

class _BarSeriesChartState extends State<BarSeriesChart> {
  int? _sel;

  static const double _yAxisW = 30;

  int? _index(Offset p, double width) {
    final int n = widget.values.length;
    if (n == 0) return null;
    final double plotW = width - _yAxisW;
    return (p.dx / plotW * n).floor().clamp(0, n - 1);
  }

  @override
  Widget build(BuildContext context) {
    final ColorScheme cs = Theme.of(context).colorScheme;
    final String caption = _sel == null ? widget.summary : widget.describe(_sel!);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: <Widget>[
        Text(caption,
            style: TextStyle(
                color: _sel == null ? cs.onSurfaceVariant : cs.onSurface,
                fontSize: 12,
                fontFeatures: const <FontFeature>[FontFeature.tabularFigures()])),
        const SizedBox(height: 6),
        LayoutBuilder(builder: (BuildContext context, BoxConstraints c) {
          return GestureDetector(
            behavior: HitTestBehavior.opaque,
            // Tap a bar to read it, tap it again to go back to the summary;
            // drag to scrub.
            onTapDown: (TapDownDetails d) {
              final int? i = _index(d.localPosition, c.maxWidth);
              setState(() => _sel = i == _sel ? null : i);
            },
            onHorizontalDragUpdate: (DragUpdateDetails d) {
              final int? i = _index(d.localPosition, c.maxWidth);
              if (i != _sel) setState(() => _sel = i);
            },
            child: Semantics(
              label: widget.summary,
              child: CustomPaint(
                size: Size(c.maxWidth, widget.height),
                painter: _BarPainter(
                  values: widget.values,
                  maxY: widget.maxY,
                  yTicks: widget.yTicks,
                  yLabel: widget.yLabel,
                  xTicks: widget.xTicks,
                  selected: _sel,
                  highlightLast: widget.highlightLast,
                  bar: cs.primary,
                  barSelected: cs.onSurface,
                  grid: cs.outline.withValues(alpha: .25),
                  ink: cs.onSurfaceVariant,
                  textDirection: Directionality.of(context),
                  yAxisW: _yAxisW,
                ),
              ),
            ),
          );
        }),
      ],
    );
  }
}

class _BarPainter extends CustomPainter {
  _BarPainter({
    required this.values,
    required this.maxY,
    required this.yTicks,
    required this.yLabel,
    required this.xTicks,
    required this.selected,
    required this.highlightLast,
    required this.bar,
    required this.barSelected,
    required this.grid,
    required this.ink,
    required this.textDirection,
    required this.yAxisW,
  });

  final List<double> values;
  final double maxY;
  final List<double> yTicks;
  final String Function(double) yLabel;
  final List<BarTick> xTicks;
  final int? selected;
  final bool highlightLast;
  final Color bar, barSelected, grid, ink;
  final TextDirection textDirection;
  final double yAxisW;

  static const double _xAxisH = 16;

  void _text(Canvas c, String s, Offset at, {TextAlign align = TextAlign.left}) {
    final TextPainter tp = TextPainter(
      text: TextSpan(text: s, style: TextStyle(color: ink, fontSize: 10)),
      textDirection: textDirection,
    )..layout();
    final double dx = switch (align) {
      TextAlign.center => at.dx - tp.width / 2,
      TextAlign.right => at.dx - tp.width,
      _ => at.dx,
    };
    tp.paint(c, Offset(dx, at.dy - tp.height / 2));
  }

  @override
  void paint(Canvas canvas, Size size) {
    final int n = values.length;
    final double plotW = size.width - yAxisW;
    final double plotH = size.height - _xAxisH;
    double y(double v) => plotH - (v / maxY).clamp(0, 1) * plotH;

    final Paint gp = Paint()
      ..color = grid
      ..strokeWidth = 1;
    for (final double t in yTicks) {
      final double yy = y(t).roundToDouble() + .5;
      canvas.drawLine(Offset(0, yy), Offset(plotW, yy), gp);
      _text(canvas, yLabel(t), Offset(size.width, yy), align: TextAlign.right);
    }
    canvas.drawLine(Offset(0, plotH + .5), Offset(plotW, plotH + .5), gp..color = grid.withValues(alpha: .6));
    if (n == 0) return;

    final double slot = plotW / n;
    // 2 px gap between bars; drop to 1 px when bars get very thin.
    final double gap = slot >= 6 ? 2 : 1;
    final double w = max(1, slot - gap);
    final double r = min(4, w / 2);
    for (int i = 0; i < n; i++) {
      final double v = values[i];
      if (v <= 0) continue;
      final double top = min(y(v), plotH - 1.5);             // keep tiny values visible
      final Paint p = Paint()
        ..color = i == selected
            ? barSelected
            : (highlightLast && i == n - 1 ? bar.withValues(alpha: .55) : bar);
      canvas.drawRRect(
        RRect.fromRectAndCorners(Rect.fromLTRB(i * slot + gap / 2, top, i * slot + gap / 2 + w, plotH),
            topLeft: Radius.circular(r), topRight: Radius.circular(r)),
        p,
      );
    }
    if (selected != null) {
      final double cx = (selected! + .5) * slot;
      canvas.drawLine(Offset(cx, 0), Offset(cx, plotH), Paint()
        ..color = ink.withValues(alpha: .35)
        ..strokeWidth = 1);
    }
    for (final BarTick t in xTicks) {
      final double x = (t.index + .5) * slot;
      _text(canvas, t.label, Offset(x.clamp(10, plotW - 10), plotH + _xAxisH / 2 + 1),
          align: TextAlign.center);
    }
  }

  @override
  bool shouldRepaint(_BarPainter o) =>
      o.values != values || o.selected != selected || o.maxY != maxY || o.bar != bar;
}
