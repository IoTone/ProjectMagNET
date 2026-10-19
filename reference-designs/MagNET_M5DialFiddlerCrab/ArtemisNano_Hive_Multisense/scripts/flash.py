#!/usr/bin/env python3
"""flash.py - upload a Zephyr image to the RedBoard Artemis Nano over USB via the SVL.

The SparkFun Variable Loader (SVL) sits at 0xC000 and runs applications from 0x10000.
This script speaks the SVL serial protocol directly (same wire format as SparkFun's
svl.py, github.com/sparkfun/Apollo3_Uploader_SVL, MIT), and adds what svl.py lacks:

  * refuses images not linked at 0x10000 (a stock Zephyr Apollo3 image links at 0xC000,
    which would never boot from the SVL),
  * pads the image to a multiple of 4 bytes (the SVL rejects a short last frame),
  * exits non-zero on failure, so it can sit in scripts and CI,
  * --monitor: reopens the port afterwards and prints the console.

Usage:
  python flash.py [--port COM7] [--build-dir C:/zephyr-artemis/build/multisense] [--monitor 10]
  python flash.py --bin path/to/zephyr.bin --port COM7

Requires pyserial (installed in the Zephyr venv by setup_zephyr_ws.sh).
"""

import argparse
import math
import sys
import time
from pathlib import Path

import serial
import serial.tools.list_ports as list_ports

APP_BASE = 0x10000          # SVL application start; matches CONFIG_FLASH_LOAD_OFFSET
FLASH_END = 0x100000        # 1 MB
SRAM_BASE = 0x10000000
SRAM_END = 0x10060000       # 384 KB
FRAME_SIZE = 512 * 4
RESEND_MAX = 4
SETUP_TRIES = 3
T_RESET = 0.15              # SparkFun-measured minimum 0.095 s for the SVL to come out of reset

SVL_CMD_VER = 0x01
SVL_CMD_BL = 0x02
SVL_CMD_NEXT = 0x03
SVL_CMD_FRAME = 0x04
SVL_CMD_RETRY = 0x05
SVL_CMD_DONE = 0x06


class FlashError(Exception):
    pass


def _crc16_table():
    # CRC-16, polynomial 0x8005, MSB-first, init 0 - same table as the SVL firmware.
    table = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) if (c & 0x8000) else (c << 1)
        table.append(c & 0xFFFF)
    return table


_CRC_TABLE = _crc16_table()


def crc16(data):
    crc = 0
    for b in data:
        crc = (_CRC_TABLE[b ^ (crc >> 8)] ^ (crc << 8)) & 0xFFFF
    return crc


def check_image(image):
    """Validate the vector table: initial SP in SRAM, reset handler inside the app area."""
    if len(image) < 8:
        raise FlashError("image is smaller than a vector table")
    sp = int.from_bytes(image[0:4], "little")
    reset = int.from_bytes(image[4:8], "little")
    if not (SRAM_BASE < sp <= SRAM_END):
        raise FlashError(f"initial SP 0x{sp:08x} is not in SRAM; is this a Zephyr zephyr.bin?")
    if not (reset & 1):
        raise FlashError(f"reset vector 0x{reset:08x} is not a Thumb address")
    if not (APP_BASE <= (reset & ~1) < FLASH_END):
        raise FlashError(
            f"reset vector 0x{reset:08x} is outside 0x{APP_BASE:x}-0x{FLASH_END:x}: the image "
            f"is not linked for the SVL. Build for sparkfun_artemis_nano "
            f"(CONFIG_FLASH_LOAD_OFFSET=0x{APP_BASE:x}).")
    if APP_BASE + len(image) > FLASH_END - 0x4000:
        raise FlashError("image runs into the storage partition at 0xFC000")
    return sp, reset


def pad4(image):
    rem = len(image) % 4
    return image + b"\xff" * (4 - rem) if rem else image


def send_packet(ser, cmd, data=b""):
    payload = bytes([cmd]) + bytes(data)
    payload += crc16(payload).to_bytes(2, "big")
    ser.write(len(payload).to_bytes(2, "big"))
    ser.write(payload)


def wait_packet(ser):
    """Return (cmd, data) or None on timeout/CRC error."""
    n = ser.read(2)
    if len(n) < 2:
        return None
    length = int.from_bytes(n, "big")
    payload = ser.read(length)
    if len(payload) != length or length < 3 or crc16(payload) != 0:
        return None
    return payload[0], payload[1:-2]


def enter_bootloader(ser):
    ser.reset_input_buffer()
    ser.write(b"U")                     # baud-rate detection character
    pkt = wait_packet(ser)
    if pkt is None or pkt[0] != SVL_CMD_VER:
        return None
    send_packet(ser, SVL_CMD_BL)
    return int.from_bytes(pkt[1], "big")


def upload(ser, image):
    total = math.ceil(len(image) / FRAME_SIZE)
    frame = 0
    resends = 0
    while True:
        pkt = wait_packet(ser)
        if pkt is None:
            raise FlashError(f"timeout or bad CRC waiting for the SVL after frame {frame}/{total}")
        cmd = pkt[0]
        if cmd == SVL_CMD_NEXT:
            frame += 1
            resends = 0
        elif cmd == SVL_CMD_RETRY:
            resends += 1
            if resends >= RESEND_MAX:
                raise FlashError(f"frame {frame}/{total} rejected {RESEND_MAX} times")
        else:
            raise FlashError(f"unexpected SVL command 0x{cmd:02x}")

        if frame > total:
            send_packet(ser, SVL_CMD_DONE)
            return
        chunk = image[(frame - 1) * FRAME_SIZE:frame * FRAME_SIZE]
        send_packet(ser, SVL_CMD_FRAME, chunk)
        print(f"\r  frame {frame}/{total}", end="", flush=True)


def find_port():
    ports = [p for p in list_ports.comports() if "CH340" in (p.description or "")
             or (p.vid, p.pid) == (0x1A86, 0x7523)]
    if len(ports) == 1:
        return ports[0].device
    found = "\n".join(f"  {p.device}: {p.description}" for p in list_ports.comports())
    if not ports:
        raise FlashError("no CH340 serial port found; pass --port. Ports:\n" + (found or "  (none)"))
    raise FlashError("several CH340 ports found; pass --port. Ports:\n" + found)


def monitor(port, seconds):
    # Reopening the port pulses RTS, which resets the board; the SVL times out waiting
    # for 'U' and jumps to the app, so this shows the boot banner from the start.
    print(f"--- monitor {port} @115200 for {seconds}s ---")
    with serial.Serial(port, 115200, timeout=0.1) as ser:
        end = time.time() + seconds
        while time.time() < end:
            data = ser.read(256)
            if data:
                # Boot can emit a stray non-UTF-8 byte; keep output printable on any console codepage.
                text = data.decode("utf-8", errors="replace")
                enc = sys.stdout.encoding or "utf-8"
                sys.stdout.write(text.encode(enc, errors="backslashreplace").decode(enc))
                sys.stdout.flush()
    print("\n--- end monitor ---")


def main():
    ap = argparse.ArgumentParser(description="Upload a Zephyr image to the Artemis Nano via the SVL.")
    ap.add_argument("--port", help="serial port (default: auto-detect the CH340)")
    ap.add_argument("--build-dir", default="C:/zephyr-artemis/build/multisense",
                    help="west build directory (uses <dir>/zephyr/zephyr.bin)")
    ap.add_argument("--bin", help="explicit image file instead of --build-dir")
    ap.add_argument("-b", "--baud", type=int, default=115200,
                    help="upload baud rate (SVL auto-detects; 115200..921600)")
    ap.add_argument("-t", "--timeout", type=float, default=0.5, help="serial timeout, seconds")
    ap.add_argument("--check-only", action="store_true", help="validate the image and exit")
    ap.add_argument("--monitor", type=float, default=0, metavar="SECONDS",
                    help="after upload, print the console for this many seconds")
    ap.add_argument("--monitor-only", action="store_true",
                    help="skip the upload; reset the board and print the console")
    args = ap.parse_args()

    try:
        path = Path(args.bin) if args.bin else Path(args.build_dir) / "zephyr" / "zephyr.bin"
        if not path.is_file():
            raise FlashError(f"{path} not found; run west build first")
        raw = path.read_bytes()
        sp, reset = check_image(raw)
        image = pad4(raw)
        print(f"{path}: {len(raw)} bytes"
              + (f" (padded to {len(image)})" if len(image) != len(raw) else "")
              + f", SP=0x{sp:08x}, reset=0x{reset:08x}")
        if args.check_only:
            return 0

        port = args.port or find_port()
        if args.monitor_only:
            monitor(port, args.monitor or 10)
            return 0
        for attempt in range(1, SETUP_TRIES + 1):
            with serial.Serial(port, args.baud, timeout=args.timeout) as ser:
                time.sleep(T_RESET)
                version = enter_bootloader(ser)
                if version is None:
                    print(f"  no SVL response on {port} (attempt {attempt}/{SETUP_TRIES})")
                    continue
                print(f"{port}: SVL v{version}, uploading at {args.baud} baud")
                upload(ser, image)
                print("\nupload complete")
                break
        else:
            raise FlashError(f"board on {port} never answered the SVL handshake. "
                             "Check the port, close other serial monitors, and confirm the SVL is present.")

        if args.monitor:
            monitor(port, args.monitor)
        return 0
    except FlashError as e:
        print(f"\nflash.py: {e}", file=sys.stderr)
        return 1
    except serial.SerialException as e:
        print(f"\nflash.py: serial error: {e} (is another program holding the port?)", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
