// Copyright (c) 2026 IoTone, Inc.
// SPDX-License-Identifier: MIT

/// MagNET Host Control Protocol — framing and typed client.
///
/// This is the Dart sibling of `host-sdk/python/magnet_hcp.py`; the framing
/// rules come from the design proposal §11.3 and are identical on every
/// transport (§11.2), so the same parser serves BLE here and USB-CDC there.
///
/// Line classes are identified by the first character:
///   `+` / `-`  the single terminal response to a command (optionally
///              prefixed with `@tag` so replies can be matched even when
///              events interleave)
///   `!`        unsolicited event — a message arrived, a peer joined
///   `#`        human-readable commentary a parser may ignore
///
/// That separation is the whole point: a caller awaiting a response can never
/// be handed someone else's chat message.
library;
import 'dart:async';

/// A `-ERR <CODE> <text>` response. [code] is the stable `E_*` token (§11.3.4).
class HcpError implements Exception {
  HcpError(this.code, [this.message = '']);
  final String code;
  final String message;
  @override
  String toString() => 'HcpError($code${message.isEmpty ? '' : ': $message'})';
}

class HcpTimeout implements Exception {
  HcpTimeout(this.command);
  final String command;
  @override
  String toString() => 'HcpTimeout(no response to "$command")';
}

/// An unsolicited `!` line, split into its name and fields.
class HcpEvent {
  HcpEvent(this.name, this.fields, this.raw);

  /// Lowercased event name without the sigil — `chat`, `peer_join`, `state`.
  final String name;
  final List<String> fields;
  final String raw;

  /// Trailing free-text payload, per event shape (§11.3.3).
  String get text {
    const Map<String, int> from = <String, int>{
      'chat': 3, 'rchat': 3, 'dm': 2, 'warn': 1, 'state': 1,
    };
    final int? i = from[name];
    if (i == null || fields.length <= i) return '';
    return fields.sublist(i).join(' ');
  }

  @override
  String toString() => raw;
}

/// Byte pipe a [HcpClient] drives. BLE, serial, or a fake in tests.
abstract class HcpTransport {
  /// Complete lines arriving from the node (terminator already stripped).
  Stream<String> get lines;

  /// Send one command line. The implementation appends the terminator if its
  /// framing needs one.
  Future<void> send(String line);
}

/// Speaks HCP over any [HcpTransport].
class HcpClient {
  HcpClient(this._transport) {
    _sub = _transport.lines.listen(_onLine);
  }

  final HcpTransport _transport;
  late final StreamSubscription<String> _sub;

  final StreamController<HcpEvent> _events =
      StreamController<HcpEvent>.broadcast();
  final StreamController<String> _comments =
      StreamController<String>.broadcast();
  final StreamController<String> _traffic =
      StreamController<String>.broadcast();

  final Map<String, Completer<String>> _pending = <String, Completer<String>>{};
  final List<String> _recentComments = <String>[];
  int _tagSeq = 0;
  Future<void> _queue = Future<void>.value();

  /// Unsolicited `!` events.
  Stream<HcpEvent> get events => _events.stream;

  /// `#` commentary lines, for a console view.
  Stream<String> get comments => _comments.stream;

  /// Every line in and out, for the raw console.
  Stream<String> get traffic => _traffic.stream;

  /// `#` lines emitted during the most recent command.
  List<String> get lastComments => List<String>.unmodifiable(_recentComments);

  void _onLine(String raw) {
    final String line = raw.trim();
    if (line.isEmpty) return;
    _traffic.add(line);

    String body = line;
    String? tag;
    if (line.startsWith('@')) {
      final int sp = line.indexOf(' ');
      if (sp > 0) {
        tag = line.substring(1, sp);
        body = line.substring(sp + 1);
      }
    }
    if (body.isEmpty) return;

    switch (body[0]) {
      case '+':
      case '-':
        final Completer<String>? c =
            tag != null ? _pending.remove(tag) : _firstPending();
        if (c != null && !c.isCompleted) {
          if (body[0] == '-') {
            final List<String> p = body.substring(1).split(' ');
            c.completeError(HcpError(
              p.length > 1 ? p[1] : 'E_UNKNOWN',
              p.length > 2 ? p.sublist(2).join(' ') : '',
            ));
          } else {
            c.complete(body.substring(1).trimLeft());
          }
        }
        break;
      case '!':
        final List<String> p = body.substring(1).split(' ');
        _events.add(HcpEvent(p.first.toLowerCase(),
            p.sublist(1), body));
        break;
      case '#':
        final String c = body.substring(1).trim();
        _recentComments.add(c);
        _comments.add(c);
        break;
      default:
        break; // not HCP (stray log line) — ignore per §11.3
    }
  }

  Completer<String>? _firstPending() =>
      _pending.isEmpty ? null : _pending.values.first;

  /// Send a command and await its terminal response.
  ///
  /// Commands are serialized so a tag can never be ambiguous. Returns the
  /// `+OK …` body without the sigil; throws [HcpError] on `-ERR`.
  Future<String> command(String line,
          {Duration timeout = const Duration(seconds: 8)}) async =>
      (await commandCaptured(line, timeout: timeout)).$1;

  /// Like [command], but also returns the `#` comment lines that arrived
  /// during THIS command, captured before the next queued command can clear
  /// them. Callers that parse comments (PEERS, MESH, STATS, ADMIN LIST) must
  /// use this rather than [lastComments] whenever another command could be
  /// queued concurrently — [lastComments] is only safe when the caller owns
  /// the client exclusively and serialises its own calls.
  Future<(String, List<String>)> commandCaptured(String line,
      {Duration timeout = const Duration(seconds: 8)}) {
    final Completer<(String, List<String>)> done =
        Completer<(String, List<String>)>();
    _queue = _queue.then((_) async {
      _recentComments.clear();
      _tagSeq = (_tagSeq + 1) % 10000;
      final String tag = 'a${_tagSeq.toString().padLeft(4, '0')}';
      final Completer<String> c = Completer<String>();
      _pending[tag] = c;
      _traffic.add('> $line');
      try {
        await _transport.send('@$tag $line');
        final String r = await c.future.timeout(timeout, onTimeout: () {
          _pending.remove(tag);
          throw HcpTimeout(line);
        });
        if (!done.isCompleted) {
          done.complete((r, List<String>.unmodifiable(_recentComments)));
        }
      } catch (e) {
        _pending.remove(tag);
        if (!done.isCompleted) done.completeError(e);
      }
    });
    return done.future;
  }

  Future<void> dispose() async {
    await _sub.cancel();
    await _events.close();
    await _comments.close();
    await _traffic.close();
  }

  // ---- typed helpers over the verb set ----

  static Map<String, String> parseKv(String body) {
    final Map<String, String> out = <String, String>{};
    for (final String tok in body.split(RegExp(r'\s+'))) {
      final int i = tok.indexOf('=');
      if (i > 0) out[tok.substring(0, i)] = tok.substring(i + 1);
    }
    return out;
  }

  Future<bool> ping() async => (await command('PING')).startsWith('PONG');

  Future<Map<String, String>> status() async => parseKv(await command('STATUS'));

  Future<Map<String, String>> whoami() async => parseKv(await command('WHOAMI'));

  Future<Map<String, String>> caps() async =>
      parseKv(await command('CAPS', timeout: const Duration(seconds: 12)));

  Future<Map<String, String>> channel() async =>
      parseKv(await command('CHANNEL SHOW'));

  /// Join/derive a channel. A typed passphrase runs PBKDF2 on-device, which
  /// takes seconds — hence the long default timeout.
  Future<Map<String, String>> setChannel(String credential,
          {Duration timeout = const Duration(seconds: 45)}) async =>
      parseKv(await command('CHANNEL SET $credential', timeout: timeout));

  Future<void> setName(String name) => command('NAME $name');

  /// Enrol an operator key. `publicKeyHex` is SEC1 uncompressed (`04 ‖ X ‖ Y`,
  /// 130 chars) — exactly what [OperatorIdentity.publicKeyHex] produces.
  /// Idempotent on the node: re-adding a key already present is `+OK`.
  Future<void> adminAdd(String publicKeyHex) =>
      command('ADMIN ADD $publicKeyHex');

  /// Enrolled admin keys as the node reports them — the leading 8 bytes of
  /// each 65-byte pubkey, lowercase hex. Empty list means an empty allow-list,
  /// which is the state where *anyone* can still claim the node.
  ///
  /// The node answers with `#` comment lines, so this reads [lastComments]
  /// rather than the `+OK` body (same shape as [stats]).
  Future<List<String>> adminList() async {
    await command('ADMIN LIST');
    final List<String> out = <String>[];
    for (final String c in lastComments) {
      final Match? m = RegExp(r'^admin key \d+:\s*([0-9a-f]+)').firstMatch(c);
      if (m != null) out.add(m.group(1)!);
    }
    return out;
  }

  Future<void> chat(String text) => command('CHAT $text');

  Future<void> subscribe(List<String> classes) =>
      command('SUB ${classes.join(',')}');

  /// tx/rx counters, flattened as `tx_ok`, `rx_msgs`, …
  Future<Map<String, String>> stats() async {
    await command('STATS');
    final Map<String, String> out = <String, String>{};
    for (final String c in lastComments) {
      if (!c.startsWith('stats ')) continue;
      final List<String> parts = c.split(RegExp(r'\s+'));
      if (parts.length < 2) continue;
      final String kind = parts[1]; // tx | rx
      parseKv(c).forEach((String k, String v) => out['${kind}_$k'] = v);
    }
    return out;
  }

  Future<List<Map<String, String>>> peers() async {
    await command('PEERS');
    final List<Map<String, String>> out = <Map<String, String>>[];
    for (final String c in lastComments) {
      if (!c.startsWith('peer ')) continue;
      final List<String> p = c.split(RegExp(r'\s+'));
      if (p.length >= 4) {
        out.add(<String, String>{'id': p[1], 'name': p[2], 'ipv6': p[3]});
      }
    }
    return out;
  }

  Future<bool> selftest() async {
    try {
      final String r =
          await command('SELFTEST', timeout: const Duration(seconds: 20));
      return r.contains('pass');
    } on HcpError {
      return false;
    }
  }
}
