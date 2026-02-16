#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$ROOT_DIR/tools"
CONFIG_DIR="$TOOLS_DIR/configuration"
BUILD_SCRIPT="$CONFIG_DIR/build.sh"

detect_os() {
  local u
  u="$(uname -s)"
  case "$u" in
    Darwin) echo "darwin" ;;
    Linux) echo "linux" ;;
    *)
      echo "Unsupported OS: $u" >&2
      exit 1
      ;;
  esac
}

detect_arch() {
  local m
  m="$(uname -m)"
  case "$m" in
    x86_64|amd64) echo "amd64" ;;
    arm64|aarch64) echo "arm64" ;;
    *)
      echo "Unsupported architecture: $m" >&2
      exit 1
      ;;
  esac
}

OS="$(detect_os)"
ARCH="$(detect_arch)"

BIN_TARGET="$TOOLS_DIR/settings-configurator-${OS}-${ARCH}"
BIN_FALLBACK="$TOOLS_DIR/settings-configurator"

ensure_executable() {
  local path="$1"
  [[ -f "$path" ]] || return 1

  if [[ ! -x "$path" ]]; then
    chmod +x "$path" 2>/dev/null || true
  fi

  [[ -x "$path" ]]
}

pick_binary() {
  local candidate
  for candidate in "$BIN_FALLBACK" "$BIN_TARGET"; do
    if ensure_executable "$candidate"; then
      echo "$candidate"
      return 0
    fi
  done
  return 1
}

binary_needs_rebuild() {
  local bin="$1"
  local source

  [[ -n "$bin" ]] || return 0
  [[ -f "$bin" ]] || return 0

  for source in \
    "$CONFIG_DIR/main.go" \
    "$CONFIG_DIR/wake_models.go" \
    "$CONFIG_DIR/go.mod" \
    "$CONFIG_DIR/go.sum"
  do
    if [[ -f "$source" && "$source" -nt "$bin" ]]; then
      return 0
    fi
  done

  return 1
}

BIN="$(pick_binary || true)"
if [[ -z "$BIN" ]] || binary_needs_rebuild "$BIN"; then
  echo "Configurator binary missing or outdated. Building current platform..."
  if [[ ! -f "$BUILD_SCRIPT" ]]; then
    echo "Build script not found: $BUILD_SCRIPT" >&2
    exit 1
  fi

  # Run via bash to avoid failures when execute bit is missing.
  bash "$BUILD_SCRIPT" current

  BIN="$(pick_binary || true)"
  if [[ -z "$BIN" ]]; then
    echo "Failed to prepare configurator binary. Checked:" >&2
    echo "  - $BIN_TARGET" >&2
    echo "  - $BIN_FALLBACK" >&2
    exit 1
  fi
fi

ensure_tui_terminal() {
  if [[ ! -t 0 || ! -t 1 || ! -t 2 ]]; then
    echo "Error: settings configurator requires an interactive terminal (TTY)." >&2
    echo "Run it in Terminal/iTerm without redirected input/output." >&2
    return 1
  fi

  local term="${TERM:-}"
  if [[ -n "$term" && "$term" != "dumb" ]]; then
    return 0
  fi

  # TERM=dumb (or empty) breaks tcell/tview in some shells. Use a safe fallback.
  if command -v infocmp >/dev/null 2>&1; then
    if infocmp xterm-256color >/dev/null 2>&1; then
      export TERM="xterm-256color"
      return 0
    fi
    if infocmp xterm >/dev/null 2>&1; then
      export TERM="xterm"
      return 0
    fi
  fi

  export TERM="xterm"
}

run_configurator() {
  local -a args
  if [[ $# -eq 0 ]]; then
    args=(--file "$ROOT_DIR/settings.csv")
  else
    args=("$@")
  fi

  ensure_tui_terminal

  local err_file
  err_file="$(mktemp "${TMPDIR:-/tmp}/settings-configurator.XXXXXX")"

  if "$BIN" "${args[@]}" 2> >(tee "$err_file" >&2); then
    rm -f "$err_file"
    return 0
  fi

  local exit_code=$?
  if grep -Eq "terminal not cursor addressable|terminal entry not found|no terminfo entry" "$err_file" && [[ "${TERM:-}" != "xterm-256color" ]]; then
    echo "Detected terminal capability issue for TERM='${TERM:-}'. Retrying with TERM=xterm-256color..." >&2
    if TERM="xterm-256color" "$BIN" "${args[@]}"; then
      rm -f "$err_file"
      return 0
    fi
    exit_code=$?
  fi

  rm -f "$err_file"
  return "$exit_code"
}

run_configurator "$@"
