#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_DIR="$PROJECT_ROOT/tools"
APP_NAME="settings-configurator"

LOCAL_DIR="$PROJECT_ROOT/.local"
GO_ROOT="$LOCAL_DIR/go"
GO_BIN="$GO_ROOT/bin/go"

require_cmd() {
  local cmd="$1"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "Required tool not found: $cmd" >&2
    return 1
  fi
}

map_go_platform() {
  local os arch
  os="$(uname -s)"
  arch="$(uname -m)"

  case "$os" in
    Darwin) os="darwin" ;;
    Linux) os="linux" ;;
    *)
      echo "Unsupported OS for local Go bootstrap: $os" >&2
      return 1
      ;;
  esac

  case "$arch" in
    x86_64|amd64) arch="amd64" ;;
    arm64|aarch64) arch="arm64" ;;
    *)
      echo "Unsupported architecture for local Go bootstrap: $arch" >&2
      return 1
      ;;
  esac

  echo "$os/$arch"
}

resolve_go_version() {
  if [[ -n "${GO_VERSION:-}" ]]; then
    echo "$GO_VERSION"
    return 0
  fi

  if command -v curl >/dev/null 2>&1; then
    local v
    v="$(curl -fsSL https://go.dev/VERSION?m=text | head -n1 || true)"
    if [[ "$v" =~ ^go[0-9] ]]; then
      echo "$v"
      return 0
    fi
  elif command -v wget >/dev/null 2>&1; then
    local v
    v="$(wget -qO- https://go.dev/VERSION?m=text | head -n1 || true)"
    if [[ "$v" =~ ^go[0-9] ]]; then
      echo "$v"
      return 0
    fi
  fi

  echo "go1.24.0"
}

download_file() {
  local url="$1"
  local out="$2"

  if command -v curl >/dev/null 2>&1; then
    curl -fL "$url" -o "$out"
    return 0
  fi
  if command -v wget >/dev/null 2>&1; then
    wget -qO "$out" "$url"
    return 0
  fi

  echo "Neither curl nor wget is available." >&2
  return 1
}

install_local_go() {
  local platform os arch version url tmp
  platform="$(map_go_platform)"
  os="${platform%/*}"
  arch="${platform#*/}"

  require_cmd tar
  if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
    echo "curl or wget is required to download local Go toolchain." >&2
    return 1
  fi

  version="$(resolve_go_version)"
  url="https://go.dev/dl/${version}.${os}-${arch}.tar.gz"

  echo "==> Installing local Go toolchain: ${version} (${os}/${arch})"
  mkdir -p "$LOCAL_DIR"
  rm -rf "$GO_ROOT"

  tmp="$(mktemp)"
  download_file "$url" "$tmp"
  tar -C "$LOCAL_DIR" -xzf "$tmp"
  rm -f "$tmp"

  if [[ ! -x "$GO_BIN" ]]; then
    echo "Local Go installation failed: $GO_BIN not found" >&2
    return 1
  fi
}

ensure_go() {
  if [[ -x "$GO_BIN" ]]; then
    return 0
  fi
  install_local_go
}

build_current() {
  echo "==> Building current platform (portable binary)"
  local goos
  goos="$($GO_BIN env GOOS)"
  local ext=""
  [[ "$goos" == "windows" ]] && ext=".exe"

  (
    cd "$SCRIPT_DIR"
    CGO_ENABLED=0 "$GO_BIN" mod tidy
    CGO_ENABLED=0 "$GO_BIN" build -trimpath -ldflags='-s -w' -o "$OUT_DIR/$APP_NAME$ext" .
  )

  echo "Built: $OUT_DIR/$APP_NAME$ext"
}

build_all() {
  echo "==> Building multi-platform portable binaries"

  local targets=(
    "darwin/amd64"
    "darwin/arm64"
    "linux/amd64"
    "linux/arm64"
    "windows/amd64"
    "windows/arm64"
  )

  (cd "$SCRIPT_DIR" && CGO_ENABLED=0 "$GO_BIN" mod tidy)

  local failed=0
  local target goos goarch ext out
  for target in "${targets[@]}"; do
    goos="${target%/*}"
    goarch="${target#*/}"
    ext=""
    [[ "$goos" == "windows" ]] && ext=".exe"

    out="$OUT_DIR/${APP_NAME}-${goos}-${goarch}${ext}"

    echo " -> $goos/$goarch"
    if (
      cd "$SCRIPT_DIR"
      CGO_ENABLED=0 GOOS="$goos" GOARCH="$goarch" "$GO_BIN" build -trimpath -ldflags='-s -w' -o "$out" .
    ); then
      echo "    OK: $out"
    else
      echo "    FAILED: $goos/$goarch"
      failed=1
    fi
  done

  if [[ "$failed" -ne 0 ]]; then
    echo "One or more targets failed" >&2
    return 1
  fi
}

mode="${1:-current}"
ensure_go
case "$mode" in
  current)
    build_current
    ;;
  all)
    build_all
    ;;
  *)
    echo "Usage: $0 [current|all]"
    exit 1
    ;;
esac
