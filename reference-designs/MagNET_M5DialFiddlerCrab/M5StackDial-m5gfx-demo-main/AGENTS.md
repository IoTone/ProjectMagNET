Agent guidance for this repo

Scope: Entire repository

- PlatformIO core dir
  - Use the user cache at `~/.platformio` (fast, persistent).
  - Do not create a project‑local core dir (e.g. `.pio-home`) — it bloats the repo directory and is slow on cloud‑synced paths.
  - VS Code is already configured in `.vscode/settings.json` with `"platformio-ide.coreDir": "/Users/adamknowles/.platformio"`.

- Builds
- Build: `pio run -e dev`
- Upload: `pio run -e dev -t upload`
  - Monitor: `pio device monitor -b 115200`

- Don’ts
  - Don’t rely on `PLATFORMIO_HOME_DIR` env vars for this project; use the VS Code setting above.
  - Don’t store large toolchains or caches inside the project tree.

- Cleanup note
  - If a `.pio-home/` folder appears in this repo, it’s a leftover local core dir and can be safely deleted.
