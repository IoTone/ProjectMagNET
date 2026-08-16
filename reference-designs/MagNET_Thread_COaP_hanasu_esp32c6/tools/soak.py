#!/usr/bin/env python3
"""soak — long-running whole-bench soak for MagNET Hanasu.

Opens every bench node's serial port ONCE and holds it for hours (reopening
per command resets the C6 — see hcp.py), then:

  * drips chat-rate traffic: one node sends `CHAT soak#N …` round-robin
    every --chat-secs (default 60s ⇒ each of 4 nodes sends every 4 min)
  * samples STATS + SYSINFO on every node every --sample-mins (default 10)
  * counts delivery: each soak chat should surface as !CHAT on the other
    three nodes; misses are logged when they age out
  * survives port vanish/re-enumerate (on-die USB bridge) and counts it

    soak.py                       # 6-hour soak
    soak.py --hours 2 --chat-secs 30 --sample-mins 5

Everything goes to stdout and to tools/logs/soak-<stamp>.log; a summary
(delivery %, per-node heap drift, error counts) is printed at the end and
on SIGINT/SIGTERM.
"""
import argparse
import os
import re
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hcp  # noqa: E402  (ports(), BAUD, skip-list logic)

try:
    import serial
except ImportError:                                    # pragma: no cover
    sys.exit('pyserial missing: pip install pyserial')

SOAK_RE = re.compile(r'soak#(\d+)\b')
HEAP_RE = re.compile(r'# sys heap free=(\d+) largest=(\d+) min-ever=(\d+)')
STATS_TX_RE = re.compile(r'# stats tx try=(\d+) ok=(\d+) err=(\d+)')
STATS_RX_RE = re.compile(r'# stats rx msgs=(\d+) dup=(\d+) err=(\d+)')


class Node:
    def __init__(self, port):
        self.port = port
        self.tag = port.rsplit('usbmodem', 1)[-1].rsplit('ttyACM', 1)[-1]
        self.ser = None
        self.buf = b''
        self.name = '?'
        self.node_id = '?'
        self.reconnects = 0
        self.err_lines = 0
        self.heap_samples = []          # (t, free, min_ever)
        self.last_stats = {}            # rx_err etc. from last sample
        self.down_since = None

    def open(self):
        try:
            self.ser = serial.Serial(self.port, hcp.BAUD, timeout=0)
            self.down_since = None
            return True
        except (OSError, serial.SerialException):
            self.ser = None
            return False

    def write_line(self, line, log):
        if not self.ser:
            return False
        try:
            self.ser.write((line + '\n').encode())
            return True
        except (OSError, serial.SerialException):
            self._drop(log)
            return False

    def poll_lines(self, log):
        """Non-blocking read; yield complete decoded lines."""
        if not self.ser:
            down = self.down_since          # open() clears it on success
            if down is not None and self.open():
                self.reconnects += 1
                log(f'[{self.tag}] port back after '
                    f'{time.time() - down:.0f}s '
                    f'(reconnect #{self.reconnects})')
            if not self.ser:
                return
        try:
            data = self.ser.read(8192)
        except (OSError, serial.SerialException):
            self._drop(log)
            return
        if not data:
            return
        self.buf += data
        while b'\n' in self.buf:
            raw, self.buf = self.buf.split(b'\n', 1)
            line = raw.decode('utf-8', 'replace').rstrip('\r')
            if line:
                yield line

    def _drop(self, log):
        try:
            self.ser.close()
        except Exception:
            pass
        self.ser = None
        if self.down_since is None:
            self.down_since = time.time()
            log(f'[{self.tag}] port vanished — will re-open')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hours', type=float, default=6.0)
    ap.add_argument('--chat-secs', type=float, default=60.0)
    ap.add_argument('--sample-mins', type=float, default=10.0)
    ap.add_argument('--skip', default='',
                    help='comma-separated port substrings to leave alone '
                         '(e.g. a node another harness is driving)')
    ap.add_argument('--expect-nodes', type=int, default=0,
                    help='fail fast unless exactly this many nodes are found')
    ap.add_argument('--gate', action='store_true',
                    help='CI mode: exit 2 unless the gates below pass')
    ap.add_argument('--gate-delivery', type=float, default=99.5,
                    help='min delivery %% (chat rate is lossless; default 99.5)')
    ap.add_argument('--gate-heap-drift', type=int, default=8192,
                    help='max per-node heap loss first->last sample, bytes')
    a = ap.parse_args()

    logdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'logs')
    os.makedirs(logdir, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M')
    logpath = os.path.join(logdir, f'soak-{stamp}.log')
    logf = open(logpath, 'a', buffering=1)

    def log(msg):
        line = f'{time.strftime("%H:%M:%S")} {msg}'
        print(line, flush=True)
        logf.write(line + '\n')

    skips = [s for s in a.skip.split(',') if s]
    nodes = [Node(p) for p in hcp.ports()
             if not any(s in p for s in skips)]
    if len(nodes) < 2:
        sys.exit(f'need >=2 nodes, found {len(nodes)}')
    if a.expect_nodes and len(nodes) != a.expect_nodes:
        sys.exit(f'expected {a.expect_nodes} nodes, found {len(nodes)}: '
                 + ' '.join(n.port for n in nodes))
    log(f'soak start: {len(nodes)} nodes, {a.hours}h, '
        f'chat every {a.chat_secs:.0f}s round-robin, '
        f'sample every {a.sample_mins:.0f}min  log={logpath}')

    for n in nodes:
        if not n.open():
            sys.exit(f'cannot open {n.port}')
        log(f'[{n.tag}] open {n.port}')

    # Opening may have reset the boards; give them one settle window, then
    # identify each (STATUS carries name= and id=).
    log('settling 20s (open can reset the C6)…')
    end = time.time() + 20
    while time.time() < end:
        for n in nodes:
            for _ in n.poll_lines(log):
                pass
        time.sleep(0.05)
    for n in nodes:
        n.write_line('STATUS', log)
    end = time.time() + 3
    while time.time() < end:
        for n in nodes:
            for line in n.poll_lines(log):
                m = re.search(r'name=(\S+)', line)
                if m and 'state=' in line:
                    n.name = m.group(1)
                    mid = re.search(r'id=(\S+)', line)
                    if mid:
                        n.node_id = mid.group(1)
        time.sleep(0.05)
    ids = {n.node_id: n for n in nodes}
    log('bench: ' + '  '.join(f'{n.tag}={n.name}/{n.node_id}' for n in nodes))

    # seq -> {'t': sent_time, 'sender': Node, 'seen': set(node_id)}
    inflight = {}
    misses = []                          # (seq, sender, missing_tags)
    delivered = expected = 0
    seq = 0
    sender_idx = 0
    stop = {'now': False}

    def on_signal(_s, _f):
        stop['now'] = True
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    t0 = time.time()
    deadline = t0 + a.hours * 3600
    next_chat = t0 + 5
    next_sample = t0 + 30                # early first sample = baseline
    age_out = max(30.0, a.chat_secs)     # misses judged after this long

    while time.time() < deadline and not stop['now']:
        now = time.time()

        # 1. pump every port, harvest events
        for n in nodes:
            for line in n.poll_lines(log):
                if line.startswith('-ERR'):
                    n.err_lines += 1
                    log(f'[{n.tag}] {line}')
                    continue
                m = HEAP_RE.search(line)
                if m:
                    n.heap_samples.append(
                        (now - t0, int(m.group(1)), int(m.group(3))))
                    log(f'[{n.tag}] {line}')
                    continue
                if STATS_TX_RE.search(line) or STATS_RX_RE.search(line):
                    mrx = STATS_RX_RE.search(line)
                    if mrx:
                        n.last_stats = {'rx_msgs': int(mrx.group(1)),
                                        'rx_dup': int(mrx.group(2)),
                                        'rx_err': int(mrx.group(3))}
                    log(f'[{n.tag}] {line}')
                    continue
                if line.startswith('!CHAT'):
                    ms = SOAK_RE.search(line)
                    if ms:
                        rec = inflight.get(int(ms.group(1)))
                        if rec is not None and n is not rec['sender']:
                            rec['seen'].add(n.tag)
                    continue                      # counted, don't spam log
                if line.startswith(('!HEARTBEAT', '# peer', '@')) \
                        or line.startswith('+OK'):
                    continue                      # routine noise
                log(f'[{n.tag}] {line}')          # roles, warns, surprises

        # 2. age out inflight chats → delivery accounting
        for s in sorted(inflight):
            rec = inflight[s]
            if now - rec['t'] < age_out:
                break
            want = {n.tag for n in nodes if n is not rec['sender']}
            got = rec['seen'] & want
            delivered += len(got)
            expected += len(want)
            if got != want:
                missing = ','.join(sorted(want - got))
                misses.append((s, rec['sender'].name, missing))
                log(f'MISS soak#{s} from {rec["sender"].name}: '
                    f'not seen on {missing}')
            del inflight[s]

        # 3. round-robin chat
        if now >= next_chat:
            sender = nodes[sender_idx % len(nodes)]
            sender_idx += 1
            if sender.ser:
                seq += 1
                sender.write_line(f'CHAT soak#{seq} from {sender.name}', log)
                inflight[seq] = {'t': now, 'sender': sender, 'seen': set()}
            next_chat = now + a.chat_secs

        # 4. periodic STATS + SYSINFO sweep
        if now >= next_sample:
            log(f'--- sample sweep @ {(now - t0)/60:.1f} min ---')
            for n in nodes:
                n.write_line('STATS', log)
                n.write_line('SYSINFO', log)
            next_sample = now + a.sample_mins * 60

        time.sleep(0.05)

    # ---- summary ----
    dur = (time.time() - t0) / 60
    log('=' * 60)
    log(f'SOAK SUMMARY  ({dur:.1f} min, {seq} chats sent)')
    if expected:
        log(f'delivery: {delivered}/{expected} '
            f'({100.0 * delivered / expected:.2f}%)  misses={len(misses)}')
    for n in nodes:
        h = n.heap_samples
        if h:
            drift = h[-1][1] - h[0][1]
            log(f'[{n.tag}] {n.name}: heap first={h[0][1]} last={h[-1][1]} '
                f'({drift:+d} B) min-ever={h[-1][2]}  '
                f'rx_err={n.last_stats.get("rx_err", "?")} '
                f'rx_dup={n.last_stats.get("rx_dup", "?")} '
                f'-ERR={n.err_lines} reconnects={n.reconnects}')
        else:
            log(f'[{n.tag}] {n.name}: no heap samples '
                f'(-ERR={n.err_lines} reconnects={n.reconnects})')
    for s, sender, missing in misses[:20]:
        log(f'  miss soak#{s} from {sender}: {missing}')
    log(f'log: {logpath}')

    rc = 0
    if a.gate:
        fails = []
        pct = 100.0 * delivered / expected if expected else 0.0
        if pct < a.gate_delivery:
            fails.append(f'delivery {pct:.2f}% < {a.gate_delivery}%')
        for n in nodes:
            h = n.heap_samples
            if not h:
                fails.append(f'{n.name}: no heap samples')
            elif h[0][1] - h[-1][1] > a.gate_heap_drift:
                fails.append(f'{n.name}: heap lost {h[0][1] - h[-1][1]} B '
                             f'> {a.gate_heap_drift} B')
            if n.last_stats.get('rx_err', 0):
                fails.append(f'{n.name}: rx_err={n.last_stats["rx_err"]}')
        if fails:
            for f in fails:
                log(f'GATE FAIL: {f}')
            rc = 2
        else:
            log('GATE PASS')
    logf.close()
    return rc


if __name__ == '__main__':
    sys.exit(main())
