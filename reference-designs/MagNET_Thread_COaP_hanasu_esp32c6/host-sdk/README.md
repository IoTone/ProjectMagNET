# MagNET Hanasu — host SDK

Reference host-side implementations of the **Host Control Protocol** (design
proposal §11.3). The point of HCP is that a node is an *add-on to any device*:
the same line grammar rides USB-CDC, raw UART, BLE-GATT, or a WebSocket, so a
host library written once works across all of them.

| Language | Status | Path |
|----------|--------|------|
| Python | ✅ validated 16/16 against the 4-node bench (2026-07-31) | `python/magnet_hcp.py` |
| TypeScript (WebSerial/WebBluetooth, for the WebXR clients) | planned | — |
| Swift (iOS companion) | planned | — |

## Python

Requires `pyserial`. No other dependencies.

```python
from magnet_hcp import MagnetNode

with MagnetNode("/dev/cu.usbmodem101") as node:
    node.on("chat", lambda ev: print(f"<{ev.fields[2]}> {ev.text}"))
    print(node.status())          # {'state': 'READY', 'role': 'leader', ...}
    node.set_name("alice")
    node.chat("hello mesh")
```

### Why the sigil design matters

HCP marks every line by its first character: `+`/`-` are the single terminal
response to *your* command, `!` is unsolicited (a message arrived, a peer
joined), `#` is human commentary a parser may ignore. The SDK turns that into
the guarantee a driver actually wants:

- `node.command(...)` blocks for **its own** response (matched by `@tag`) and
  raises `HCPError` carrying the stable `E_*` code on failure.
- events never satisfy a command; they go to `node.on(...)` callbacks.

Validated on hardware: with chat events streaming in from three other nodes,
six consecutive `STATUS` calls each returned their own correct reply — zero
cross-talk.

### API sketch

```
ping() status() whoami() caps() channel()      # typed key=value dicts
set_channel(cred)  set_name(n)                 # provisioning
chat(text)  dm(ipv6, text)                     # messaging
subscribe(*classes)  unsubscribe(*classes)     # event filtering (node-side)
terse(bool)                                    # suppress '#' lines
stats()  peers()  selftest()                   # diagnostics
forth(src)  hook(kind, word)  save_script(src)  # E-E automation
command(line)                                  # any verb, verbatim
discover_nodes()                               # every attached node that PINGs
```

`HCPError.code` is the machine-readable token: `E_RATE_LIMITED`,
`E_UNKNOWN_VERB`, `E_BAD_STATE`, `E_NOT_ADMIN`, … (§11.3.4).

### CLI

```bash
python3 magnet_cli.py                      # auto-discovers a node
python3 magnet_cli.py /dev/cu.usbmodem101 --name alice
```

A terminal chat client: type to send, `/status`, `/peers`, `/join <cred>`,
`/dm <ipv6> <text>`, `/raw <hcp line>`, `/quit`. Doubles as a worked example —
it is ~100 lines because the SDK carries the protocol.

### Secrets

`CHANNEL SHOW` and friends return **public** values only (name, selector,
multicast group). The node never emits a passphrase, root secret, epoch key, or
private key over any binding (§11.3.5), and the SDK has no API that asks for
one — `set_channel()` sends a credential and gets back only the derived public
identifiers.
