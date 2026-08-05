#!/usr/bin/env python3
"""hcp — talk to a MagNET node over its USB serial link.

The one place pyserial lives. Everything else (bench orchestration, parsing,
reporting) is driven from Pop-11 via `tools/bench.p`, which shells out to this.

    hcp.py list                          # ports that look like a C6
    hcp.py <port> STATUS                 # send one HCP line, print the reply
    hcp.py <port> --reboot               # pulse RTS, wait for READY
    hcp.py <port> --watch 20             # dump the console for 20s

Exit code is 1 if the node answered `-ERR`, so callers can branch on it.
"""
import argparse
import glob
import os
import sys
import time

try:
    import serial
except ImportError:                                    # pragma: no cover
    sys.exit('pyserial missing: pip install pyserial')

BAUD = 115200


# Ports on this bench that are NOT Hanasu nodes. Other ESP32 boards enumerate
# with the same Espressif USB-JTAG VID/PID (303a:1001) and the same
# description, so nothing but the port path tells them apart — and `all` /
# `flash` must never touch them. Override with
# MAGNET_HCP_SKIP=<substr>[,<substr>…] (empty string = skip nothing).
# Both entries are the same physical board: the hub re-enumerates it as
# usbmodem1101 or usbmodem11101 depending on plug order/topology.
SKIP_DEFAULT = 'usbmodem1101,usbmodem11101'


def ports():
    """C6 dev boards enumerate as usbmodem* on macOS, ttyACM* on Linux."""
    skip = [s for s in os.environ.get('MAGNET_HCP_SKIP', SKIP_DEFAULT).split(',') if s]
    found = sorted(glob.glob('/dev/cu.usbmodem*') + glob.glob('/dev/ttyACM*'))
    return [p for p in found if not any(s in p for s in skip)]


def _open(port, retries=40, reset=False):
    """Open the port, tolerating the device briefly disappearing.

    `reset` is accepted for symmetry with reboot(); the reset pulse itself is
    done with explicit RTS toggling there.
    """
    del reset
    for _ in range(retries):
        try:
            # NB: opening with DTR de-asserted stops the C6's USB-CDC
            # endpoint transmitting — the node then answers nothing. A plain
            # open is what works here; the cost is that the board may reset,
            # which is why peer tables read empty right after connecting.
            return serial.Serial(port, BAUD, timeout=0.05)
        except (OSError, serial.SerialException):
            time.sleep(0.25)
    raise SystemExit(f'cannot open {port}')


def drain(ser, secs):
    """Read for `secs`, tolerating the C6's USB-serial-JTAG vanishing.

    The bridge is on-die: any reset re-enumerates the device mid-read, which
    surfaces as OSError. That is normal here, not a failure.
    """
    end, buf = time.time() + secs, b''
    while time.time() < end:
        try:
            buf += ser.read(8192)
        except (OSError, serial.SerialException):
            break
    return buf.decode('utf-8', 'replace')


def reboot(port, settle=16.0):
    """Pulse the board into reset and return its boot console."""
    ser = _open(port, reset=True)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    try:
        ser.close()
    except Exception:
        pass
    time.sleep(0.6)
    ser = _open(port)
    out = drain(ser, settle)
    ser.close()
    return out


def send(port, line, wait=1.5):
    ser = _open(port)
    ser.read(8192)                                     # discard backlog
    ser.write((line + '\n').encode())
    out = drain(ser, wait)
    ser.close()
    return out


def timed(port, line, wait=15.0):
    """Send, and report seconds until the first +/-/@ response line.

    Verbs whose cost is the point — CHANNEL SET's PBKDF2 stretch, SELFTEST's
    CoAP loopback — need a number, not a guess. `wait` is an upper bound; this
    returns as soon as the response lands.
    """
    ser = _open(port)
    ser.read(8192)
    t0 = time.time()
    ser.write((line + '\n').encode())
    end, buf, elapsed = t0 + wait, b'', None
    while time.time() < end and elapsed is None:
        try:
            buf += ser.read(4096)
        except (OSError, serial.SerialException):
            break
        for ln in buf.split(b'\n'):
            if ln[:1] in (b'+', b'-', b'@'):
                elapsed = time.time() - t0
                break
    ser.close()
    return elapsed, buf.decode('utf-8', 'replace')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port', nargs='?')
    ap.add_argument('command', nargs='*')
    ap.add_argument('--reboot', action='store_true')
    ap.add_argument('--watch', type=float, metavar='SECS')
    ap.add_argument('--wait', type=float, default=1.5)
    ap.add_argument('--time', action='store_true',
                    help='report seconds until the response (for slow verbs)')
    a = ap.parse_args()

    if a.port in (None, 'list'):
        print('\n'.join(ports()))
        return 0

    if a.port == 'synctime':
        # Seed the mesh clock from THIS host. A Thread-only mesh has no border
        # router and therefore no NTP, so one node has to be told; it becomes
        # stratum 0 and multicasts to the channel (see docs/MESH-TIME.md).
        # Seeding one node is the point — seeding several creates competing
        # anchors — so this picks the first port unless one is named.
        now = time.time()
        tzmin = -int(time.timezone if not time.localtime().tm_isdst
                     else time.altzone) // 60
        target = a.command[0] if a.command else (ports() or [None])[0]
        if not target:
            print('no nodes found', file=sys.stderr)
            return 1
        line = f'TIME SET {int(now)} {tzmin}'
        print(f'{target}\t{line}')
        sys.stdout.write(send(target, line, max(a.wait, 3.0)))
        return 0

    if a.port == 'all':
        # One process for the whole bench — spawning python per node made a
        # four-node status sweep cost seconds.
        line = ' '.join(a.command) or 'STATUS'
        for p in ports():
            try:
                out = send(p, line, a.wait)
            except (OSError, serial.SerialException, SystemExit) as e:
                print(f'{p}\t(unreachable: {e})')
                continue
            reply = next((l for l in out.splitlines()
                          if l[:1] in '+-@'), '(no answer)')
            print(f'{p}\t{reply}')
        return 0

    if a.reboot:
        sys.stdout.write(reboot(a.port))
        return 0
    if a.watch:
        # Stream, don't buffer: a watcher whose output only appears when it
        # exits is useless for "start capture, then trigger the event".
        ser = _open(a.port)
        end, buf = time.time() + a.watch, b''
        while time.time() < end:
            try:
                buf += ser.read(8192)
            except (OSError, serial.SerialException):
                break
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                sys.stdout.write(line.decode('utf-8', 'replace').rstrip('\r') + '\n')
                sys.stdout.flush()
        ser.close()
        return 0

    if a.time:
        secs, out = timed(a.port, ' '.join(a.command),
                          a.wait if a.wait != 1.5 else 15.0)
        sys.stdout.write(out)
        print(f'# elapsed {secs:.2f}s' if secs is not None else '# no response')
        return 1 if '-ERR' in out else 0

    out = send(a.port, ' '.join(a.command), a.wait)
    sys.stdout.write(out)
    return 1 if '-ERR' in out else 0


if __name__ == '__main__':
    sys.exit(main())
