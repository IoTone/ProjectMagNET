// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../gen/app_localizations.dart';
import '../magnet/sentinel_feed.dart';
import 'sentinel_charts.dart';

/// Room presence from Sentinel IR nodes: one card per Sentinel with its
/// state, the hour/day/week/month counters it keeps, and recent activity.
class SentinelScreen extends StatefulWidget {
  const SentinelScreen({super.key});

  @override
  State<SentinelScreen> createState() => _SentinelScreenState();
}

class _SentinelScreenState extends State<SentinelScreen> {
  Timer? _tick;

  @override
  void initState() {
    super.initState();
    context.read<SentinelFeed>().start();
    // Countdown, "last motion" and chart scrolling. A second is cheap; the
    // screen only lives while it is open.
    _tick = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _tick?.cancel();
    super.dispose();
  }

  Future<void> _editBroker(SentinelFeed feed) async {
    final AppLocalizations l = AppLocalizations.of(context);
    final TextEditingController tc = TextEditingController(text: feed.broker);
    final String? v = await showDialog<String>(
      context: context,
      builder: (BuildContext ctx) => AlertDialog(
        title: Text(l.sentinelBroker),
        content: TextField(
          controller: tc,
          autofocus: true,
          keyboardType: TextInputType.url,
          autocorrect: false,
          decoration: InputDecoration(helperText: l.sentinelBrokerHint, helperMaxLines: 3),
          onSubmitted: (String s) => Navigator.of(ctx).pop(s),
        ),
        actions: <Widget>[
          TextButton(onPressed: () => Navigator.of(ctx).pop(), child: Text(l.commonCancel)),
          FilledButton(
              onPressed: () => Navigator.of(ctx).pop(tc.text),
              child: Text(l.sentinelBrokerSave)),
        ],
      ),
    );
    tc.dispose();
    if (v != null && v.trim() != feed.broker) await feed.setBroker(v);
  }

  @override
  Widget build(BuildContext context) {
    final SentinelFeed feed = context.watch<SentinelFeed>();
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final List<SentinelDevice> devices = feed.devices;

    return Scaffold(
      appBar: AppBar(
        title: Text(l.sentinelTitle),
        actions: <Widget>[
          _LinkChip(feed: feed),
          IconButton(
            tooltip: l.sentinelBroker,
            icon: const Icon(Icons.dns_outlined),
            onPressed: () => _editBroker(feed),
          ),
        ],
      ),
      body: RefreshIndicator(
        onRefresh: feed.reconnect,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 32),
          children: <Widget>[
            if (feed.link == SentinelLinkState.error && feed.lastError != null)
              Padding(
                padding: const EdgeInsets.only(bottom: 12),
                child: Text(feed.lastError!, style: TextStyle(color: cs.error, fontSize: 12)),
              ),
            if (devices.isEmpty)
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 32),
                child: Text(l.sentinelEmpty(feed.broker),
                    style: TextStyle(color: cs.onSurfaceVariant, height: 1.4)),
              ),
            for (final SentinelDevice d in devices) ...<Widget>[
              _SentinelCard(device: d),
              const SizedBox(height: 16),
            ],
            if (feed.broker.contains('broker.hivemq.com'))
              Text(l.sentinelPrivacy,
                  style: TextStyle(color: cs.onSurfaceVariant, fontSize: 12, height: 1.4)),
          ],
        ),
      ),
    );
  }
}

class _LinkChip extends StatelessWidget {
  const _LinkChip({required this.feed});
  final SentinelFeed feed;

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final (String label, Color color) = switch (feed.link) {
      SentinelLinkState.idle => (l.sentinelLinkIdle, cs.onSurfaceVariant),
      SentinelLinkState.connecting => (l.sentinelLinkConnecting, cs.onSurfaceVariant),
      SentinelLinkState.connected => (l.sentinelLinkConnected, cs.tertiary),
      SentinelLinkState.reconnecting => (l.sentinelLinkReconnecting, cs.onSurfaceVariant),
      SentinelLinkState.error => (l.sentinelLinkError, cs.error),
    };
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 4),
      child: Row(mainAxisSize: MainAxisSize.min, children: <Widget>[
        Container(
            width: 8, height: 8, decoration: BoxDecoration(color: color, shape: BoxShape.circle)),
        const SizedBox(width: 6),
        Text(label, style: TextStyle(color: color, fontSize: 12)),
      ]),
    );
  }
}

/// Occupied time: "25 s" under a minute (so a short visit doesn't read as
/// nothing), then h:mm.
String _hm(int secs) => secs > 0 && secs < 60
    ? '$secs s'
    : '${secs ~/ 3600}:${(secs ~/ 60 % 60).toString().padLeft(2, '0')}';

String _clock(DateTime t) =>
    '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}:'
    '${t.second.toString().padLeft(2, '0')}';

String _ago(Duration d) {
  final int s = d.inSeconds < 0 ? 0 : d.inSeconds;
  if (s < 3600) return '${s ~/ 60}:${(s % 60).toString().padLeft(2, '0')}';
  return '${s ~/ 3600}h${(s ~/ 60 % 60).toString().padLeft(2, '0')}m';
}

/// Elapsed or remaining time for prose: "42 s", "4:18", "1h05m".
String _dur(int s) {
  if (s < 60) return '$s s';
  if (s < 3600) return '${s ~/ 60}:${(s % 60).toString().padLeft(2, '0')}';
  return '${s ~/ 3600}h${(s ~/ 60 % 60).toString().padLeft(2, '0')}m';
}

String _hhmm(DateTime t) =>
    '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';

String _periodName(AppLocalizations l, String p) => switch (p) {
      'hour' => l.sentinelPeriodHour,
      'day' => l.sentinelPeriodDay,
      'week' => l.sentinelPeriodWeek,
      'month' => l.sentinelPeriodMonth,
      _ => p,
    };

class _SentinelCard extends StatelessWidget {
  const _SentinelCard({required this.device});
  final SentinelDevice device;

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final SentinelDevice d = device;
    final DateTime now = DateTime.now();

    final (String stateLabel, Color stateColor) = !d.online
        ? (l.sentinelStateOffline, cs.onSurfaceVariant)
        : switch (d.state) {
            'present' when d.active => (l.sentinelStateActive, cs.error),
            'present' => (l.sentinelStatePresent, cs.error),
            'clear' => (l.sentinelStateClear, cs.tertiary),
            'warmup' => (l.sentinelStateWarmup, cs.secondary),
            _ => (l.sentinelStateUnknown, cs.onSurfaceVariant),
          };

    final int? clearIn = d.clearInAt(now);
    final int? motionAgo = d.lastMotionAgoAt(now);
    // Left and right halves of one fixed-height row (see below).
    final String motionLeft = d.active
        ? l.sentinelMotionNow
        : motionAgo != null
            ? l.sentinelLastMotion(_dur(motionAgo))
            : '';
    final String motionRight = clearIn != null && !d.active ? l.sentinelClearIn(_dur(clearIn)) : '';
    final List<String> meta = <String>[
      if (d.stateSince != null && d.online) l.sentinelFor(_ago(now.difference(d.stateSince!))),
      if (d.lastHeard != null) l.sentinelLastHeard(_clock(d.lastHeard!)),
      if (d.online && !d.clockKnown) l.sentinelClockUnset,
    ];

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: cs.surfaceContainerHighest.withValues(alpha: .4),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(
            color: d.present ? cs.error.withValues(alpha: .7) : cs.outline.withValues(alpha: .3)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          Row(children: <Widget>[
            Icon(Icons.sensors, color: stateColor),
            const SizedBox(width: 10),
            Expanded(
              child: Text('Sentinel ${d.id}',
                  style: TextStyle(
                      color: cs.onSurface, fontSize: 17, fontWeight: FontWeight.w600)),
            ),
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
              decoration: BoxDecoration(
                color: stateColor.withValues(alpha: .15),
                borderRadius: BorderRadius.circular(20),
                border: Border.all(color: stateColor),
              ),
              child: Text(stateLabel,
                  style: TextStyle(
                      color: stateColor,
                      fontSize: 12,
                      fontWeight: FontWeight.w700,
                      letterSpacing: 1.2)),
            ),
          ]),
          if (meta.isNotEmpty) ...<Widget>[
            const SizedBox(height: 6),
            Text(meta.join('  ·  '),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: TextStyle(color: cs.onSurfaceVariant, fontSize: 12)),
          ],
          // Motion changes every second or two. Keep it to one row of fixed
          // height that is always there while online, so nothing below it
          // moves when "motion now" swaps with the countdown.
          if (d.online) ...<Widget>[
            const SizedBox(height: 4),
            SizedBox(
              height: 20,
              child: Row(children: <Widget>[
                Expanded(
                  child: Text(motionLeft,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(
                          color: d.active ? cs.error : cs.onSurface,
                          fontSize: 13,
                          fontFeatures: const <FontFeature>[FontFeature.tabularFigures()])),
                ),
                Text(motionRight,
                    maxLines: 1,
                    style: TextStyle(
                        color: clearIn != null && clearIn < 60 ? cs.error : cs.onSurfaceVariant,
                        fontSize: 13,
                        fontFeatures: const <FontFeature>[FontFeature.tabularFigures()])),
              ]),
            ),
          ],
          if (d.checkSensorAt(now)) ...<Widget>[
            const SizedBox(height: 12),
            Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: cs.secondary.withValues(alpha: .12),
                borderRadius: BorderRadius.circular(10),
                border: Border.all(color: cs.secondary),
              ),
              child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: <Widget>[
                Icon(Icons.warning_amber_rounded, color: cs.secondary, size: 20),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: <Widget>[
                    Text(l.sentinelCheckSensor,
                        style: TextStyle(color: cs.onSurface, fontWeight: FontWeight.w600)),
                    const SizedBox(height: 2),
                    Text(l.sentinelCheckSensorBody(_dur(d.quietAt(now) ?? 0)),
                        style: TextStyle(color: cs.onSurfaceVariant, fontSize: 13, height: 1.35)),
                  ]),
                ),
              ]),
            ),
          ],
          const SizedBox(height: 16),
          Row(children: <Widget>[
            for (final String p in kSentinelPeriods)
              Expanded(child: _StatCell(label: _periodName(l, p), stat: d.stats[p])),
          ]),
          const SizedBox(height: 18),
          _SeriesCharts(device: d, now: now),
          const SizedBox(height: 16),
          Text(l.sentinelRecent,
              style: TextStyle(
                  color: cs.onSurfaceVariant, fontSize: 11, letterSpacing: 1.5)),
          const SizedBox(height: 6),
          if (d.events.isEmpty)
            Text(l.sentinelNoEvents, style: TextStyle(color: cs.onSurfaceVariant, fontSize: 13))
          else
            for (final SentinelMessage e in d.events.take(8)) _EventRow(event: e),
        ],
      ),
    );
  }
}

class _StatCell extends StatelessWidget {
  const _StatCell({required this.label, required this.stat});
  final String label;
  final SentinelStat? stat;

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: <Widget>[
      Text(label, style: TextStyle(color: cs.onSurfaceVariant, fontSize: 11)),
      const SizedBox(height: 2),
      Text(stat == null ? '–' : '${stat!.entries}',
          style: TextStyle(
              color: cs.onSurface,
              fontSize: 26,
              fontWeight: FontWeight.w700,
              height: 1.1,
              fontFeatures: const <FontFeature>[FontFeature.tabularFigures()])),
      Text(l.sentinelEntries, style: TextStyle(color: cs.onSurfaceVariant, fontSize: 11)),
      const SizedBox(height: 2),
      Text(stat == null ? '' : l.sentinelOccupied(_hm(stat!.occupiedS)),
          style: TextStyle(color: cs.onSurfaceVariant, fontSize: 11)),
    ]);
  }
}

class _EventRow extends StatelessWidget {
  const _EventRow({required this.event});
  final SentinelMessage event;

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final SentinelMessage e = event;
    final (IconData icon, Color color, String text) = switch (e.ev) {
      'enter' => (Icons.login, cs.error, l.sentinelEvEnter),
      'present' => (Icons.person_outline, cs.onSurface, l.sentinelEvPresent(_hm(e.visitS ?? 0))),
      'clear' => (Icons.logout, cs.tertiary, l.sentinelEvClear(_hm(e.visitS ?? 0))),
      'rollover' => (
          Icons.event_repeat,
          cs.onSurfaceVariant,
          l.sentinelEvRollover(_periodName(l, e.period ?? ''), e.label ?? '',
              '${e.entries ?? 0}', _hm(e.occupiedS ?? 0))
        ),
      'reset' => (
          Icons.restart_alt,
          cs.onSurfaceVariant,
          l.sentinelEvReset(e.periods.map((String p) => _periodName(l, p)).join(', '))
        ),
      _ => (Icons.circle_outlined, cs.onSurfaceVariant, e.ev),
    };
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: <Widget>[
        SizedBox(
          width: 64,
          child: Text(_clock(e.at),
              style: TextStyle(
                  color: cs.onSurfaceVariant,
                  fontSize: 12,
                  fontFeatures: const <FontFeature>[FontFeature.tabularFigures()])),
        ),
        Icon(icon, size: 16, color: color),
        const SizedBox(width: 8),
        Expanded(child: Text(text, style: TextStyle(color: cs.onSurface, fontSize: 13))),
      ]),
    );
  }
}

class _SeriesCharts extends StatelessWidget {
  const _SeriesCharts({required this.device, required this.now});
  final SentinelDevice device;
  final DateTime now;

  Widget _heading(ColorScheme cs, String t) => Padding(
        padding: const EdgeInsets.only(bottom: 4),
        child: Text(t,
            style: TextStyle(color: cs.onSurfaceVariant, fontSize: 11, letterSpacing: 1.5)),
      );

  @override
  Widget build(BuildContext context) {
    final AppLocalizations l = AppLocalizations.of(context);
    final ColorScheme cs = Theme.of(context).colorScheme;
    final List<int> recent = device.recentAt(now);
    final List<int> hist = device.historyAt(now);
    if (recent.isEmpty && hist.isEmpty) {
      return Text(l.sentinelNoSeries, style: TextStyle(color: cs.onSurfaceVariant, fontSize: 13));
    }
    final List<Widget> out = <Widget>[];

    if (recent.isNotEmpty) {
      final int n = recent.length;
      final double motionS = recent.fold<int>(0, (int a, int b) => a + b) / 10;
      out.addAll(<Widget>[
        _heading(cs, l.sentinelLast5),
        BarSeriesChart(
          values: recent.map((int v) => v.toDouble()).toList(),
          maxY: 100,
          yTicks: const <double>[50, 100],
          yLabel: (double v) => '${v.round()}%',
          xTicks: <BarTick>[
            const BarTick(0, '-5m'),
            BarTick(n ~/ 2, '-2.5m'),
            BarTick(n - 1, l.sentinelNow),
          ],
          height: 70,
          highlightLast: true,
          summary: l.sentinelLast5Summary(motionS.toStringAsFixed(motionS < 10 ? 1 : 0)),
          describe: (int i) =>
              l.sentinelLast5Bar(_dur((n - 1 - i) * 10), '${recent[i]}'),
        ),
        const SizedBox(height: 18),
      ]);
    }

    if (hist.isNotEmpty) {
      final int n = hist.length;
      // Newest bin starts at historyEnd (shifted to now); bin i starts
      // (n-1-i) quarter-hours before it.
      final DateTime newest = device.historyEnd == null
          ? now
          : device.historyEnd!.add(Duration(
              minutes: 15 * (now.difference(device.historyEnd!).inMinutes ~/ 15)));
      DateTime start(int i) => newest.subtract(Duration(minutes: 15 * (n - 1 - i)));
      final List<BarTick> ticks = <BarTick>[
        for (int i = 0; i < n; i++)
          if (start(i).minute == 0 && start(i).hour % 6 == 0) BarTick(i, _hhmm(start(i))),
      ];
      final int totalS = hist.fold<int>(0, (int a, int b) => a + b);
      out.addAll(<Widget>[
        _heading(cs, l.sentinelLast24),
        BarSeriesChart(
          values: hist.map((int v) => v / 60).toList(),
          maxY: 15,
          yTicks: const <double>[5, 10, 15],
          yLabel: (double v) => '${v.round()}m',
          xTicks: ticks,
          height: 110,
          highlightLast: true,
          summary: l.sentinelLast24Summary(_hm(totalS)),
          describe: (int i) => l.sentinelLast24Bar(
              _hhmm(start(i)), _hhmm(start(i).add(const Duration(minutes: 15))),
              (hist[i] / 60).toStringAsFixed(hist[i] > 0 && hist[i] < 60 ? 1 : 0)),
        ),
      ]);
    }
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: out);
  }
}
