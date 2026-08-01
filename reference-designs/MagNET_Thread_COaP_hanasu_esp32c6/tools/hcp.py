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
import sys
import time

try:
    import serial
except ImportError:                                    # pragma: no cover
    sys.exit('pyserial missing: pip install pyserial')

BAUD = 115200


def ports():
    """C6 dev boards enumerate as usbmodem* on macOS, ttyACM* on Linux."""
    return sorted(glob.glob('/dev/cu.usbmodem*') + glob.glob('/dev/ttyACM*'))


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port', nargs='?')
    ap.add_argument('command', nargs='*')
    ap.add_argument('--reboot', action='store_true')
    ap.add_argument('--watch', type=float, metavar='SECS')
    ap.add_argument('--wait', type=float, default=1.5)
    a = ap.parse_args()

    if a.port in (None, 'list'):
        print('\n'.join(ports()))
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
        ser = _open(a.port)
        sys.stdout.write(drain(ser, a.watch))
        ser.close()
        return 0

    out = send(a.port, ' '.join(a.command), a.wait)
    sys.stdout.write(out)
    return 1 if '-ERR' in out else 0


if __name__ == '__main__':
    sys.exit(main())
