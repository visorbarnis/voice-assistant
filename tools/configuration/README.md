# Go Settings Configurator (TUI)

Cross-platform terminal utility with menu-based ncurses-like interface for editing `settings.csv`.

## Features
- Full-screen terminal UI (`tview`/`tcell`)
- Menu navigation and section editors (`WiFi`, `Server`, `Audio`)
- Wake-word model selector in `Server` editor with automatic `esp-sr` discovery/download
- Wake strictness selector in `Server` editor (`normal`/`strict` -> `DET_MODE_90/95`)
- Wake sensitivity level in `Server` editor (`0..10`, where 10 is most sensitive)
- Playback volume in `Audio` editor (`0..100%`)
- Built-in validation and canonical ESP NVS CSV output
- Portable binaries built with `CGO_ENABLED=0` (no platform GUI libs required)

## Project Layout
- `main.go` - TUI application source
- `go.mod` - module dependencies
- `build.sh` / `build.ps1` - build helpers for current/all targets

## Build Current Platform

```bash
cd tools/configuration
./build.sh current
```

## Build Multiple Targets

```bash
cd tools/configuration
./build.sh all
```

PowerShell:

```powershell
cd tools/configuration
./build.ps1 all
```

Generated binaries are placed in `tools/`.
If Go is missing, build scripts install a project-local Go toolchain into `.local/go` and use it from there.
Build scripts do not install system packages (`apt/yum/brew/choco` are not used).
Required base tools for bootstrap: `tar` and one downloader (`curl` or `wget`).

## Run

```bash
./tools/settings-configurator
```

From project root (auto-detect platform):

```bash
bash ./configure_settings.sh
```

```powershell
.\configure_settings.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\configure_settings.ps1
```

Optional file path:

```bash
./tools/settings-configurator --file settings.csv
```

## TUI Controls
- `Enter` select menu item
- `Ctrl+S` save
- `q` exit
