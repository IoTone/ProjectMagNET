#!/usr/bin/env python3
"""uartpipe — expose one Hanasu node's serial port as a FIFO pair.

Plain open(2) of the C6's USB-serial-JTAG device gets NOTHING back: the
host must assert DTR (pyserial does; a bare open does not), so the node's
drop-on-stall TX guard discards every byte it would have sent us.  Rather
than teach every host language termios+ioctl, this pump owns the port the
proven way (same pyserial open soak.py holds for hours) and re-presents it
as two FIFOs any process can use like ordinary files:

    uartpipe.py /dev/cu.usbmodemXXXX /path/rx.fifo /path/tx.fifo

  rx.fifo — read node output here (bytes, as they arrive)
  tx.fifo — write commands here (include your own newlines)

The port is opened ONCE and held (reopening resets the C6).  Either FIFO
peer may disconnect and come back; the serial port dying is fatal (exit 1)
so a supervisor notices.  Ctrl-C exits 0.
"""
import argparse
import os
import sys
import threading

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('port')
    ap.add_argument('rx_fifo', help='FIFO the peer reads node output from')
    ap.add_argument('tx_fifo', help='FIFO the peer writes commands into')
    a = ap.parse_args()

    for f in (a.rx_fifo, a.tx_fifo):
        if not os.path.exists(f):
            os.mkfifo(f)

    ser = serial.Serial(a.port, 115200, timeout=0.1)
    print(f'uartpipe: {a.port} <-> rx={a.rx_fifo} tx={a.tx_fifo}', flush=True)

    def die(msg):
        print(f'uartpipe: {msg}', file=sys.stderr, flush=True)
        os._exit(1)

    def pump_rx():                       # serial -> fifo
        while True:
            fd = os.open(a.rx_fifo, os.O_WRONLY)   # blocks until a reader
            try:
                while True:
                    try:
                        d = ser.read(4096)
                    except (OSError, serial.SerialException) as e:
                        die(f'serial read failed: {e}')
                    if d:
                        os.write(fd, d)
            except BrokenPipeError:                # reader left; wait for next
                os.close(fd)

    def pump_tx():                       # fifo -> serial
        while True:
            fd = os.open(a.tx_fifo, os.O_RDONLY)   # blocks until a writer
            while True:
                d = os.read(fd, 4096)
                if not d:                          # writer left; wait for next
                    os.close(fd)
                    break
                try:
                    ser.write(d)
                except (OSError, serial.SerialException) as e:
                    die(f'serial write failed: {e}')

    threading.Thread(target=pump_rx, daemon=True).start()
    threading.Thread(target=pump_tx, daemon=True).start()
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
