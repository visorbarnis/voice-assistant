#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

ENV_NAME="${PIO_ENV:-esp32-s3-devkitc-1}"
LOCAL_DIR="$ROOT_DIR/.local"
PIO_VENV_DIR="$LOCAL_DIR/pio-venv"
PIO_PYTHON="$PIO_VENV_DIR/bin/python"
PIO_BIN="$PIO_VENV_DIR/bin/pio"
MINIFORGE_DIR="$LOCAL_DIR/miniforge3"
MINIFORGE_PYTHON="$MINIFORGE_DIR/bin/python3"
UPLOAD_PORT_FILE="$LOCAL_DIR/upload_port"
FIRMWARE_DIR="$ROOT_DIR/firmware"
FIRMWARE_MANIFEST="$FIRMWARE_DIR/manifest.env"
FIRMWARE_BOOTLOADER="$FIRMWARE_DIR/bootloader.bin"
FIRMWARE_PARTITIONS="$FIRMWARE_DIR/partitions.bin"
FIRMWARE_APP="$FIRMWARE_DIR/firmware.bin"
FIRMWARE_SRMODELS="$FIRMWARE_DIR/srmodels.bin"
BUILD_DIR="$ROOT_DIR/.pio/build/$ENV_NAME"
BUILD_BOOTLOADER="$BUILD_DIR/bootloader.bin"
BUILD_PARTITIONS="$BUILD_DIR/partitions.bin"
BUILD_APP="$BUILD_DIR/firmware.bin"
BUILD_SRMODELS="$BUILD_DIR/srmodels/srmodels.bin"

resolve_python3() {
  local candidates=(
    "$MINIFORGE_PYTHON"
    "$LOCAL_DIR/python/bin/python3"
  )

  local py
  for py in "${candidates[@]}"; do
    if [[ -n "$py" && -x "$py" ]]; then
      echo "$py"
      return 0
    fi
  done
  return 1
}

detect_miniforge_installer() {
  local os arch
  os="$(uname -s)"
  arch="$(uname -m)"

  case "$os" in
    Darwin) os="MacOSX" ;;
    Linux) os="Linux" ;;
    *)
      echo "Unsupported OS for local Python bootstrap: $os" >&2
      return 1
      ;;
  esac

  case "$arch" in
    x86_64|amd64) arch="x86_64" ;;
    arm64|aarch64)
      if [[ "$os" == "Linux" ]]; then
        arch="aarch64"
      else
        arch="arm64"
      fi
      ;;
    *)
      echo "Unsupported architecture for local Python bootstrap: $arch" >&2
      return 1
      ;;
  esac

  echo "Miniforge3-${os}-${arch}.sh"
}

have_downloader() {
  command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1
}

download_file() {
  local url="$1"
  local out="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fL "$url" -o "$out"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$out" "$url"
  else
    return 1
  fi
}

install_local_python() {
  if [[ -x "$MINIFORGE_PYTHON" ]]; then
    return 0
  fi

  if ! have_downloader; then
    echo "Cannot install local Python automatically: curl/wget not found." >&2
    return 1
  fi

  local installer_name url tmp_dir installer_path
  installer_name="$(detect_miniforge_installer)" || return 1
  url="https://github.com/conda-forge/miniforge/releases/latest/download/${installer_name}"

  echo "Installing local Python (Miniforge) into: $MINIFORGE_DIR"
  rm -rf "$MINIFORGE_DIR"
  mkdir -p "$LOCAL_DIR"

  tmp_dir="$(mktemp -d)"
  installer_path="$tmp_dir/miniforge.sh"

  if ! download_file "$url" "$installer_path"; then
    rm -rf "$tmp_dir"
    echo "Failed to download Miniforge installer: $url" >&2
    return 1
  fi

  if ! bash "$installer_path" -b -p "$MINIFORGE_DIR"; then
    rm -rf "$tmp_dir"
    echo "Failed to install local Python into $MINIFORGE_DIR" >&2
    return 1
  fi

  rm -rf "$tmp_dir"

  if [[ ! -x "$MINIFORGE_PYTHON" ]]; then
    echo "Local Python bootstrap failed: $MINIFORGE_PYTHON not found" >&2
    return 1
  fi
}

recreate_local_venv() {
  local py="$1"
  local errfile
  errfile="$(mktemp)"

  echo "Recreating local PlatformIO environment: $PIO_VENV_DIR"
  rm -rf "$PIO_VENV_DIR"
  mkdir -p "$LOCAL_DIR"

  if "$py" -m venv "$PIO_VENV_DIR" 2>"$errfile"; then
    rm -f "$errfile"
    return 0
  fi

  # Debian/Ubuntu often has venv but without ensurepip. Fallback to without-pip.
  if "$py" -m venv --without-pip "$PIO_VENV_DIR" 2>>"$errfile"; then
    if [[ ! -x "$PIO_PYTHON" ]]; then
      cat "$errfile" >&2 || true
      rm -f "$errfile"
      return 1
    fi

    if ! have_downloader; then
      echo "Cannot bootstrap pip: curl/wget not found." >&2
      cat "$errfile" >&2 || true
      rm -f "$errfile"
      return 1
    fi

    local get_pip="$LOCAL_DIR/get-pip.py"
    download_file "https://bootstrap.pypa.io/get-pip.py" "$get_pip"
    "$PIO_PYTHON" "$get_pip"
    rm -f "$get_pip" "$errfile"
    return 0
  fi

  cat "$errfile" >&2 || true
  rm -f "$errfile"
  return 1
}

ensure_local_platformio() {
  local py
  py="$(resolve_python3 || true)"
  if [[ -z "$py" ]]; then
    install_local_python || true
    py="$(resolve_python3 || true)"
  fi
  if [[ -z "$py" ]]; then
    echo "Python 3 not found and local bootstrap failed." >&2
    echo "Provide local interpreter at one of:" >&2
    echo "  - $MINIFORGE_PYTHON" >&2
    echo "  - $LOCAL_DIR/python/bin/python3" >&2
    return 1
  fi

  local attempt
  for attempt in 1 2; do
    if [[ ! -x "$PIO_PYTHON" ]]; then
      recreate_local_venv "$py" || true
    fi

    if [[ ! -x "$PIO_PYTHON" ]]; then
      echo "Failed to create local venv (attempt $attempt)." >&2
      rm -rf "$PIO_VENV_DIR"
      continue
    fi

    "$PIO_PYTHON" -c "import sys; print(sys.version)" >/dev/null 2>&1 || {
      echo "Local venv python is broken. Cleaning..."
      rm -rf "$PIO_VENV_DIR"
      continue
    }

    if ! "$PIO_PYTHON" -m pip --version >/dev/null 2>&1; then
      "$PIO_PYTHON" -m ensurepip --upgrade >/dev/null 2>&1 || true
    fi

    if ! "$PIO_PYTHON" -m pip --version >/dev/null 2>&1; then
      echo "pip is unavailable in local venv. Cleaning and retrying..."
      rm -rf "$PIO_VENV_DIR"
      continue
    fi

    if [[ ! -x "$PIO_BIN" ]] || ! "$PIO_BIN" --version >/dev/null 2>&1; then
      "$PIO_PYTHON" -m pip install --upgrade pip setuptools wheel
      "$PIO_PYTHON" -m pip install --upgrade platformio
    fi

    if [[ -x "$PIO_BIN" ]] && "$PIO_BIN" --version >/dev/null 2>&1; then
      return 0
    fi

    echo "Local PlatformIO is not healthy. Cleaning and retrying..."
    rm -rf "$PIO_VENV_DIR"
  done

  echo "Failed to prepare local PlatformIO CLI in $PIO_VENV_DIR" >&2
  return 1
}

ensure_local_platformio
export PLATFORMIO_CORE_DIR="$ROOT_DIR/.pio_core"

require_file() {
  local path="$1"
  if [[ ! -s "$path" ]]; then
    echo "Required file is missing or empty: $path" >&2
    return 1
  fi
}

detect_wake_word_model() {
  local config_file model
  for config_file in "$ROOT_DIR/sdkconfig.$ENV_NAME" "$ROOT_DIR/sdkconfig.defaults"; do
    if [[ -f "$config_file" ]]; then
      model="$(sed -n 's/^CONFIG_WAKE_WORD_MODEL=\"\([^\"]*\)\"$/\1/p' "$config_file" | head -n1)"
      if [[ -n "$model" ]]; then
        echo "$model"
        return 0
      fi
    fi
  done
  echo "unknown"
}

purge_firmware_cache() {
  mkdir -p "$FIRMWARE_DIR"
  find "$FIRMWARE_DIR" -mindepth 1 -maxdepth 1 ! -name 'README.md' -exec rm -rf {} +
}

sync_firmware_cache_from_build() {
  require_file "$BUILD_BOOTLOADER"
  require_file "$BUILD_PARTITIONS"
  require_file "$BUILD_APP"
  require_file "$BUILD_SRMODELS"

  mkdir -p "$FIRMWARE_DIR"
  cp -f "$BUILD_BOOTLOADER" "$FIRMWARE_BOOTLOADER"
  cp -f "$BUILD_PARTITIONS" "$FIRMWARE_PARTITIONS"
  cp -f "$BUILD_APP" "$FIRMWARE_APP"
  cp -f "$BUILD_SRMODELS" "$FIRMWARE_SRMODELS"

  local wake_model git_commit utc_now
  wake_model="$(detect_wake_word_model)"
  git_commit="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
  utc_now="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

  cat > "$FIRMWARE_MANIFEST" <<EOF
GENERATED_AT_UTC=$utc_now
PIO_ENV=$ENV_NAME
WAKE_WORD_MODEL=$wake_model
GIT_COMMIT=$git_commit
EOF
}

resolve_upload_port() {
  local port=""
  local cached_port=""
  local device_json=""

  if [[ -n "${UPLOAD_PORT:-}" ]]; then
    port="$UPLOAD_PORT"
  fi

  if [[ -f "$UPLOAD_PORT_FILE" ]]; then
    cached_port="$(head -n1 "$UPLOAD_PORT_FILE" | tr -d '\r' | xargs)"
  fi

  device_json="$("$PIO_BIN" device list --json-output 2>/dev/null || true)"

  if [[ -n "$port" ]]; then
    if [[ "$port" == /dev/* ]] && [[ ! -e "$port" ]]; then
      echo "Warning: UPLOAD_PORT points to missing device: $port" >&2
    fi
    echo "$port"
    return 0
  fi

  if [[ -n "$cached_port" ]]; then
    if [[ "$cached_port" == /dev/* ]] && [[ -e "$cached_port" ]]; then
      echo "$cached_port"
      return 0
    fi

    if [[ -n "$device_json" ]] && "$PIO_PYTHON" - "$cached_port" "$device_json" <<'PY'
import json
import sys

if len(sys.argv) < 3:
    sys.exit(1)

target = sys.argv[1].strip().lower()
if not target:
    sys.exit(1)

try:
    devices = json.loads(sys.argv[2] or "[]")
except Exception:
    sys.exit(1)

if not isinstance(devices, list):
    sys.exit(1)

for dev in devices:
    port = str(dev.get("port", "")).strip().lower()
    if port == target:
        sys.exit(0)

sys.exit(1)
PY
    then
      echo "$cached_port"
      return 0
    fi

    echo "Saved upload port is stale: $cached_port. Auto-detecting active serial port..." >&2
    rm -f "$UPLOAD_PORT_FILE"
  fi

  if [[ -z "$device_json" ]]; then
    return 1
  fi

  port="$("$PIO_PYTHON" - "$device_json" <<'PY'
import json
import re
import sys

if len(sys.argv) < 2:
    sys.exit(0)

try:
    devices = json.loads(sys.argv[1] or "[]")
except Exception:
    sys.exit(0)

if not isinstance(devices, list) or not devices:
    sys.exit(0)

def score(dev):
    port = str(dev.get("port", ""))
    text = " ".join(
        str(dev.get(k, "")) for k in ("description", "hwid", "manufacturer", "product", "port")
    ).lower()
    s = 0
    if re.search(r"(303a|espressif|esp32|cp210|ch340|ftdi|usb serial|usb jtag|uart)", text):
        s += 10
    if re.search(r"(/dev/ttyusb|/dev/ttyacm|/dev/cu\\.|/dev/tty\\.|com\\d+)", port.lower()):
        s += 1
    return s

candidates = []
for d in devices:
    p = str(d.get("port", "")).strip()
    if p:
        candidates.append((score(d), p))

if not candidates:
    sys.exit(0)

candidates.sort(key=lambda x: x[0], reverse=True)
best_score = candidates[0][0]
best = [p for s, p in candidates if s == best_score]

if best_score > 0 and len(best) == 1:
    print(best[0])
elif len(candidates) == 1:
    print(candidates[0][1])
PY
)"

  if [[ -n "$port" ]]; then
    mkdir -p "$LOCAL_DIR"
    printf '%s\n' "$port" > "$UPLOAD_PORT_FILE"
    echo "$port"
    return 0
  fi

  return 1
}

assert_upload_port_access() {
  local port="$1"
  if [[ "$port" == /dev/* ]] && [[ ! -e "$port" ]]; then
    echo "Serial port does not exist: $port" >&2
    echo "Tip: reconnect the board and retry, or remove stale cache: rm -f $UPLOAD_PORT_FILE" >&2
    return 1
  fi
  if [[ -c "$port" ]] && [[ ! -r "$port" || ! -w "$port" ]]; then
    echo "Serial port exists but access is denied: $port" >&2
    echo "On Linux this is an OS-level permission/udev issue and cannot be installed locally in the project." >&2
    echo "Fix by running as root or by configuring dialout/udev rules on the host OS." >&2
    return 1
  fi
}

echo "Clearing firmware cache in: $FIRMWARE_DIR"
purge_firmware_cache
"$PIO_BIN" run -e "$ENV_NAME" -t fullclean
rm -f sdkconfig

if ! "$PIO_BIN" run -e "$ENV_NAME"; then
  echo "Build error: firmware was not uploaded." >&2
  exit 1
fi

sync_firmware_cache_from_build
echo "Firmware cache refreshed from the fresh source build."

UPLOAD_PORT_VALUE="$(resolve_upload_port || true)"
if [[ -z "$UPLOAD_PORT_VALUE" ]]; then
  echo "Upload port was not detected." >&2
  echo "Set one of the following and retry:" >&2
  echo "  1) export UPLOAD_PORT=/dev/ttyACM0" >&2
  echo "  2) echo /dev/ttyACM0 > $UPLOAD_PORT_FILE" >&2
  echo "You can inspect ports with: $PIO_BIN device list" >&2
  exit 1
fi

assert_upload_port_access "$UPLOAD_PORT_VALUE"
echo "Using upload port: $UPLOAD_PORT_VALUE"
"$PIO_BIN" run -e "$ENV_NAME" -t upload -t monitor --upload-port "$UPLOAD_PORT_VALUE" --monitor-port "$UPLOAD_PORT_VALUE"
