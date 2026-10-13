"""
magnet_hcp — reference host SDK for the MagNET Hanasu Host Control Protocol.

Implements the §11.3 grammar: one logical message per line, class identified by
the first character (sigil), exactly one terminal response (+/-) per command,
optional @tag correlation, unsolicited ! events, ignorable # comments.

The point of the sigil design is that a driver never confuses "the answer to my
command" with "a message just arrived" — this SDK encodes that guarantee:
commands block for their matching response; events go to callbacks.

    from magnet_hcp import MagnetNode

    with MagnetNode("/dev/cu.usbmodem101") as node:
        node.on("chat", lambda ev: print(f"<{ev.fields[2]}> {ev.text}"))
        print(node.status())
        node.chat("hello mesh")

Transport is pluggable: SerialTransport today, BLE/WebSocket later (§11.2) —
the framing layer above it is identical, which is the whole point of HCP.
"""
from __future__ import annotations

import queue
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional


class HCPError(RuntimeError):
    """A command returned -ERR. `.code` is the stable E_* token (§11.3.4)."""

    def __init__(self, code: str, message: str = ""):
        super().__init__(f"{code} {message}".strip())
        self.code = code
        self.message = message


class HCPTimeout(RuntimeError):
    pass


@dataclass
class Event:
    """An unsolicited `!` line: `!CHAT <chan> <from_id> <from_name> <text...>`."""

    name: str                      # lowercase, without the '!' — e.g. "chat"
    fields: List[str] = field(default_factory=list)
    raw: str = ""

    @property
    def text(self) -> str:
        """Trailing free-text payload, by event type (§11.3.3 shapes)."""
        trailing_from = {"chat": 3, "dm": 2, "warn": 1}.get(self.name)
        if trailing_from is None or len(self.fields) <= trailing_from:
            return ""
        return " ".join(self.fields[trailing_from:])

    def __str__(self) -> str:
        return self.raw


# --------------------------------------------------------------------------- #
# transports
# --------------------------------------------------------------------------- #
class SerialTransport:
    """USB-CDC / raw UART binding (§11.2). Requires pyserial."""

    def __init__(self, port: str, baud: int = 115200):
        import serial  # imported lazily so the SDK loads without pyserial

        self.port = port
        self._ser = serial.Serial(port, baud, timeout=0.05)

    def write(self, data: bytes) -> None:
        self._ser.write(data)

    def read(self, size: int = 4096) -> bytes:
        try:
            return self._ser.read(size)
        except Exception:
            return b""

    def close(self) -> None:
        try:
            self._ser.close()
        except Exception:
            pass

    @staticmethod
    def discover() -> List[str]:
        """Ports that look like an ESP32-C6 USB-serial-JTAG (VID 0x303a)."""
        from serial.tools import list_ports

        return [p.device for p in list_ports.comports()
                if (p.vid == 0x303A) or "usbmodem" in p.device]


# --------------------------------------------------------------------------- #
# the node
# --------------------------------------------------------------------------- #
class MagnetNode:
    """A connected MagNET node, driven over HCP.

    Thread-safe for one caller; a reader thread owns the transport's input and
    routes lines by sigil. Commands are serialized by a lock so tags can't be
    confused between concurrent callers.
    """

    def __init__(self, port: str | SerialTransport, timeout: float = 5.0,
                 keep_comments: bool = False):
        self._t = port if isinstance(port, SerialTransport) else SerialTransport(port)
        self.timeout = timeout
        self.keep_comments = keep_comments

        self._responses: "queue.Queue[tuple[Optional[str], str]]" = queue.Queue()
        self._handlers: Dict[str, List[Callable[[Event], None]]] = {}
        self._comments: List[str] = []
        self._cmd_lock = threading.Lock()
        self._tag_seq = 0
        self._stop = threading.Event()
        self._rx = threading.Thread(target=self._reader, daemon=True)
        self._rx.start()

    # ---- lifecycle ----
    def close(self) -> None:
        self._stop.set()
        self._rx.join(timeout=1.0)
        self._t.close()

    def __enter__(self) -> "MagnetNode":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # ---- event subscription (host-side; see also the node's SUB/UNSUB) ----
    def on(self, event_name: str, fn: Callable[[Event], None]) -> None:
        """Register a callback for an event class ("chat", "peer_join", "*")."""
        self._handlers.setdefault(event_name.lower(), []).append(fn)

    # ---- the reader: the sigil router ----
    def _reader(self) -> None:
        buf = b""
        while not self._stop.is_set():
            chunk = self._t.read()
            if not chunk:
                time.sleep(0.005)
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self._dispatch(raw.decode("utf-8", "replace").strip("\r\n"))

    def _dispatch(self, line: str) -> None:
        if not line:
            return
        tag = None
        body = line
        if line.startswith("@"):                      # tagged response
            parts = line.split(" ", 1)
            tag = parts[0][1:]
            body = parts[1] if len(parts) > 1 else ""
        sigil = body[:1]
        if sigil in "+-":
            self._responses.put((tag, body))
        elif sigil == "!":
            self._emit(body)
        elif sigil == "#":
            if self.keep_comments:
                self._comments.append(body[1:].strip())
        # anything else: not HCP (stray log line) — ignore, per §11.3

    def _emit(self, body: str) -> None:
        parts = body[1:].split(" ")
        ev = Event(name=parts[0].lower(), fields=parts[1:], raw=body)
        for fn in self._handlers.get(ev.name, []) + self._handlers.get("*", []):
            try:
                fn(ev)
            except Exception:                          # a bad callback must not
                pass                                   # kill the reader thread

    # ---- command/response ----
    def command(self, line: str, timeout: Optional[float] = None) -> str:
        """Send a command, return its `+OK ...` body (without the sigil).

        Raises HCPError on `-ERR`, HCPTimeout if no terminal response arrives.
        """
        timeout = self.timeout if timeout is None else timeout
        with self._cmd_lock:
            self._tag_seq = (self._tag_seq + 1) % 10000
            tag = f"h{self._tag_seq:04d}"
            while not self._responses.empty():         # drop stale responses
                self._responses.get_nowait()
            self._comments.clear()
            self._t.write(f"@{tag} {line}\n".encode())

            deadline = time.time() + timeout
            while time.time() < deadline:
                try:
                    rtag, body = self._responses.get(timeout=0.05)
                except queue.Empty:
                    continue
                if rtag not in (None, tag):
                    continue                            # someone else's reply
                if body.startswith("-"):
                    rest = body[1:].split(" ", 2)       # "ERR CODE msg"
                    code = rest[1] if len(rest) > 1 else "E_UNKNOWN"
                    msg = rest[2] if len(rest) > 2 else ""
                    raise HCPError(code, msg)
                return body[1:].lstrip()                # strip '+', keep "OK ..."
            raise HCPTimeout(f"no response to {line!r} within {timeout}s")

    @property
    def comments(self) -> List[str]:
        """`#` lines emitted by the last command (needs keep_comments=True)."""
        return list(self._comments)

    # ---- typed helpers over the verb set ----
    @staticmethod
    def _kv(body: str) -> Dict[str, str]:
        out: Dict[str, str] = {}
        for tok in body.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                out[k] = v
        return out

    def ping(self) -> bool:
        return self.command("PING").startswith("PONG")

    def status(self) -> Dict[str, str]:
        return self._kv(self.command("STATUS"))

    def whoami(self) -> Dict[str, str]:
        return self._kv(self.command("WHOAMI"))

    def caps(self) -> Dict[str, str]:
        return self._kv(self.command("CAPS"))

    def channel(self) -> Dict[str, str]:
        return self._kv(self.command("CHANNEL SHOW"))

    def set_channel(self, credential: str, timeout: float = 30.0) -> Dict[str, str]:
        """Join/derive a channel. Path B passphrases stretch — allow time."""
        return self._kv(self.command(f"CHANNEL SET {credential}", timeout=timeout))

    def set_name(self, name: str) -> None:
        self.command(f"NAME {name}")

    def chat(self, text: str) -> None:
        self.command(f"CHAT {text}")

    def dm(self, peer_ipv6: str, text: str) -> None:
        self.command(f"DM {peer_ipv6} {text}")

    def subscribe(self, *classes: str) -> None:
        self.command("SUB " + ",".join(classes))

    def unsubscribe(self, *classes: str) -> None:
        self.command("UNSUB " + ",".join(classes))

    def terse(self, on: bool = True) -> None:
        self.command("MODE TERSE" if on else "MODE HUMAN")

    def stats(self) -> Dict[str, str]:
        """tx/rx counters. Emitted as # lines, so capture them."""
        prev, self.keep_comments = self.keep_comments, True
        try:
            self.command("STATS")
            out: Dict[str, str] = {}
            for line in self._comments:
                if line.startswith("stats "):
                    kind = line.split()[1]              # tx | rx
                    for k, v in self._kv(line).items():
                        out[f"{kind}_{k}"] = v
            return out
        finally:
            self.keep_comments = prev

    def peers(self) -> List[Dict[str, str]]:
        prev, self.keep_comments = self.keep_comments, True
        try:
            self.command("PEERS")
            out = []
            for line in self._comments:
                if line.startswith("peer "):
                    p = line.split()
                    if len(p) >= 4:
                        out.append({"id": p[1], "name": p[2], "ipv6": p[3]})
            return out
        finally:
            self.keep_comments = prev

    def selftest(self) -> bool:
        try:
            return "pass" in self.command("SELFTEST", timeout=15.0)
        except HCPError:
            return False

    # ---- Forth / automation (E-E) ----
    def forth(self, source: str, settle: float = 0.4) -> None:
        """Evaluate a line in the node's Forth REPL, then return to HCP mode.

        Forth mode suspends HCP framing guarantees (§12.4), so this brackets
        the evaluation rather than exposing a persistent mode switch.
        """
        with self._cmd_lock:
            self._t.write(b"FORTH\n")
            time.sleep(settle)
            self._t.write(source.encode() + b"\n")
            time.sleep(settle)
            self._t.write(b".hcp\n")
            time.sleep(settle)

    def hook(self, kind: str, word: str) -> None:
        """Bind a Forth word to inbound traffic: kind is "chat" or "cmd"."""
        self.command(f"HOOK {kind.upper()} {word}")

    def clear_hooks(self) -> None:
        self.command("HOOK CLEAR")

    def save_script(self, source: str) -> None:
        """Persist a boot script. Newlines are encoded as ';;' on the wire."""
        self.command("SCRIPT SET " + source.replace("\n", ";;"))


def discover_nodes(timeout: float = 3.0) -> List[MagnetNode]:
    """Open every attached MagNET node that answers PING."""
    found = []
    for port in SerialTransport.discover():
        try:
            node = MagnetNode(port, timeout=timeout)
            if node.ping():
                found.append(node)
            else:
                node.close()
        except Exception:
            pass
    return found
