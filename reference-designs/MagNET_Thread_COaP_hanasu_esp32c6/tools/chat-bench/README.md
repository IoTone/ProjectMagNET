# chat-bench — two nodes chatting side-by-side in a browser

A visual test rig for mesh chat: one SolidJS page with two chat panes, one per
attached node, each showing live lifecycle state, role, channel, encryption
badge, and **per-message end-to-end latency** (host serial → envelope/AEAD →
802.15.4 → receiver's serial, all timed on the bridge's single clock).

```
../../../../.venv/bin/python bridge.py            # /dev/ttyACM0 + /dev/ttyACM1
# → open http://127.0.0.1:8642/
```

- `bridge.py` — stdlib-only HTTP server wrapping a `MagnetNode` (host-sdk)
  around each port. Events stream to the page over SSE; sends are POSTs.
  Opening the ports **resets both boards** (DTR/RTS) — they re-attach in
  ~30 s and the panes go red → amber → green as they come up.
- `index.html` — the whole frontend. SolidJS via esm.sh (needs internet on
  first load), tagged-template syntax, no build step, no node_modules.

Latency is correlated by message text: pane A's send timestamp (taken by the
bridge just before the serial write) vs. the bridge timestamp of pane B's
`!CHAT` arrival. Both bubbles get the ⏱ stamp; the footer keeps a running
average. System lines (`!STATE`, `!ROLE`, `!WARN`, peer join/leave) appear
inline, dimmed.

Don't run `hcp.py`/`xfer.py` against the same ports while the bridge holds
them — one process per serial port.
