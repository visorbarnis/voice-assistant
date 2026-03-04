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
FIRMWARE_SETTINGS="$FIRMWARE_DIR/settings.bin"

BUILD_DIR="$ROOT_DIR/.pio/build/$ENV_NAME"
BUILD_BOOTLOADER="$BUILD_DIR/bootloader.bin"
BUILD_PARTITIONS="$BUILD_DIR/partitions.bin"
BUILD_APP="$BUILD_DIR/firmware.bin"
BUILD_SRMODELS="$BUILD_DIR/srmodels/srmodels.bin"
BUILD_SETTINGS="$BUILD_DIR/settings.bin"

PARTITIONS_CSV="$ROOT_DIR/partitions.csv"
ESPTOOL_PY="$ROOT_DIR/.pio_core/packages/tool-esptoolpy/esptool.py"

BOOTLOADER_OFFSET="0x0"
PARTITION_TABLE_OFFSET="0x8000"
FLASH_MODE="${FLASH_MODE:-dio}"
FLASH_FREQ="${FLASH_FREQ:-80m}"
FLASH_SIZE="${FLASH_SIZE:-16MB}"
UPLOAD_BAUD="${UPLOAD_BAUD:-115200}"
MCU="${MCU:-esp32s3}"
LAST_UPLOAD_PORT=""
APP_OFFSET=""
MODEL_OFFSET=""
NVS_OFFSET=""
NVS_SIZE=""

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

require_file() {
  local path="$1"
  if [[ ! -s "$path" ]]; then
    echo "Required file is missing or empty: $path" >&2
    return 1
  fi
}

find_settings_csv() {
  local candidate
  for candidate in "$ROOT_DIR/settings.csv" "$ROOT_DIR/config/settings.csv"; do
    if [[ -f "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  done
  return 1
}

detect_settings_wake_word_model() {
  local settings_csv
  settings_csv="$(find_settings_csv || true)"
  if [[ -z "$settings_csv" ]]; then
    return 1
  fi

  "$PIO_PYTHON" - "$settings_csv" <<'PY'
import csv
import sys

if len(sys.argv) < 2:
    sys.exit(0)

path = sys.argv[1]
namespace = ""

try:
    with open(path, newline="", encoding="utf-8") as handle:
        reader = csv.reader(handle)
        for row in reader:
            if not row:
                continue

            key = row[0].strip() if len(row) > 0 else ""
            if not key or key.startswith("#"):
                continue

            row_type = row[1].strip().lower() if len(row) > 1 else ""
            value = row[3].strip() if len(row) > 3 else ""

            if row_type == "namespace":
                namespace = key
                continue

            if namespace == "server_settings" and key == "wake_word_model" and value:
                print(value)
                sys.exit(0)
except Exception:
    sys.exit(0)
PY
}

ensure_nvs_generator() {
  if "$PIO_PYTHON" -c "import esp_idf_nvs_partition_gen" >/dev/null 2>&1; then
    return 0
  fi

  echo "Installing minimal settings generator dependency: esp-idf-nvs-partition-gen"
  "$PIO_PYTHON" -m pip install --disable-pip-version-check --upgrade esp-idf-nvs-partition-gen
}

resolve_monitor_baud() {
  if [[ -n "${MONITOR_BAUD:-}" ]]; then
    echo "$MONITOR_BAUD"
    return 0
  fi

  local conf_file="$ROOT_DIR/platformio.ini"
  if [[ ! -f "$conf_file" ]]; then
    if [[ "${UPLOAD_BAUD:-}" =~ ^[0-9]+$ ]]; then
      echo "$UPLOAD_BAUD"
    else
      echo "115200"
    fi
    return 0
  fi

  local detected=""
  detected="$("$PIO_PYTHON" - "$conf_file" "$ENV_NAME" <<'PY'
import configparser
import sys
from pathlib import Path

if len(sys.argv) < 3:
    print("")
    raise SystemExit(0)

conf = Path(sys.argv[1])
env_name = sys.argv[2]
section = f"env:{env_name}"

parser = configparser.ConfigParser(inline_comment_prefixes=(";", "#"))
parser.optionxform = str

try:
    with conf.open("r", encoding="utf-8") as handle:
        parser.read_file(handle)
except Exception:
    print("")
    raise SystemExit(0)

for key in ("monitor_speed", "upload_speed"):
    value = ""
    try:
        value = parser.get(section, key, fallback="").strip()
    except Exception:
        value = ""
    if value.isdigit():
        print(value)
        raise SystemExit(0)

print("")
PY
)"

  if [[ "$detected" =~ ^[0-9]+$ ]]; then
    echo "$detected"
    return 0
  fi

  if [[ "${UPLOAD_BAUD:-}" =~ ^[0-9]+$ ]]; then
    echo "$UPLOAD_BAUD"
  else
    echo "115200"
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

detect_cached_wake_word_model() {
  if [[ ! -f "$FIRMWARE_MANIFEST" ]]; then
    return 1
  fi
  sed -n 's/^WAKE_WORD_MODEL=//p' "$FIRMWARE_MANIFEST" | head -n1
}

sync_wake_word_model_with_settings() {
  local settings_model current_model
  settings_model="$(detect_settings_wake_word_model || true)"
  if [[ -z "$settings_model" ]]; then
    echo "Warning: wake_word_model was not found in settings.csv. Keeping current build model." >&2
    return 0
  fi

  current_model="$(detect_wake_word_model || true)"
  if [[ "$current_model" == "$settings_model" ]]; then
    return 0
  fi

  echo "Wake-word model from settings: $settings_model (build config: ${current_model:-unknown}). Updating sdkconfig files..."
  "$PIO_PYTHON" - "$ROOT_DIR" "$ENV_NAME" "$settings_model" <<'PY'
import re
import sys
from pathlib import Path

if len(sys.argv) < 4:
    sys.exit(1)

project_root = Path(sys.argv[1])
env_name = sys.argv[2]
model = sys.argv[3].strip()
if not model:
    sys.exit(0)

selected_symbol = "CONFIG_SR_WN_" + re.sub(r"[^A-Z0-9]+", "_", model.upper()).strip("_")
paths = [
    project_root / "sdkconfig.defaults",
    project_root / f"sdkconfig.{env_name}",
    project_root / "sdkconfig",
]

updated_any = False

for path in paths:
    if not path.exists() or not path.is_file():
        continue

    updated_any = True
    content = path.read_text(encoding="utf-8")
    lines = content.replace("\r\n", "\n").split("\n")

    changed = False
    has_model_line = False
    has_selected_symbol = False

    for idx, line in enumerate(lines):
        trimmed = line.strip()
        if not trimmed:
            continue

        if trimmed.startswith("CONFIG_WAKE_WORD_MODEL=") or trimmed.startswith("# CONFIG_WAKE_WORD_MODEL is not set"):
            next_line = f'CONFIG_WAKE_WORD_MODEL="{model}"'
            if line != next_line:
                lines[idx] = next_line
                changed = True
            has_model_line = True
            continue

        symbol = None
        if trimmed.startswith("# CONFIG_SR_WN_") and trimmed.endswith(" is not set"):
            symbol = trimmed[2 : -len(" is not set")].strip()
        elif trimmed.startswith("CONFIG_SR_WN_"):
            sep = trimmed.find("=")
            if sep > 0:
                symbol = trimmed[:sep].strip()

        if not symbol:
            continue

        if symbol == selected_symbol:
            next_line = f"{symbol}=y"
            if line != next_line:
                lines[idx] = next_line
                changed = True
            has_selected_symbol = True
            continue

        if symbol.endswith("_NONE"):
            continue

        next_line = f"# {symbol} is not set"
        if line != next_line:
            lines[idx] = next_line
            changed = True

    if not has_model_line:
        lines.append(f'CONFIG_WAKE_WORD_MODEL="{model}"')
        changed = True
    if not has_selected_symbol:
        lines.append(f"{selected_symbol}=y")
        changed = True

    if changed:
        output = "\n".join(lines)
        if not output.endswith("\n"):
            output += "\n"
        path.write_text(output, encoding="utf-8")

if not updated_any:
    print("No sdkconfig files were found to sync wake-word model.", file=sys.stderr)
    sys.exit(1)
PY
}

cache_wake_word_model_matches_settings() {
  local settings_model cached_model
  settings_model="$(detect_settings_wake_word_model || true)"
  if [[ -z "$settings_model" ]]; then
    echo "Warning: wake_word_model was not found in settings.csv. Skipping firmware model cache check." >&2
    return 0
  fi

  cached_model="$(detect_cached_wake_word_model || true)"
  if [[ -z "$cached_model" ]]; then
    echo "Firmware cache model is unknown (manifest WAKE_WORD_MODEL is missing). Rebuild required."
    return 1
  fi

  if [[ "$cached_model" != "$settings_model" ]]; then
    echo "Firmware cache model '$cached_model' differs from settings model '$settings_model'. Rebuild required."
    return 1
  fi

  return 0
}

load_partition_offsets() {
  if [[ ! -f "$PARTITIONS_CSV" ]]; then
    echo "Partition file not found: $PARTITIONS_CSV" >&2
    return 1
  fi

  local output
  output="$("$PIO_PYTHON" - "$PARTITIONS_CSV" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
app_offset = ""
model_offset = ""
nvs_offset = ""
nvs_size = ""
fallback_app = ""

for raw in path.read_text(encoding="utf-8").splitlines():
    clean = raw.split("#", 1)[0].strip()
    if not clean:
        continue
    fields = [item.strip() for item in clean.split(",")]
    if len(fields) < 5:
        continue

    name = fields[0]
    ptype = fields[1]
    offset = fields[3]
    if not offset:
        continue

    if name == "factory":
        app_offset = offset
    if not fallback_app and ptype == "app":
        fallback_app = offset
    if name == "model":
        model_offset = offset
    if name == "nvs":
        nvs_offset = offset
        nvs_size = fields[4]

if not app_offset:
    app_offset = fallback_app

print(f"APP_OFFSET={app_offset}")
print(f"MODEL_OFFSET={model_offset}")
print(f"NVS_OFFSET={nvs_offset}")
print(f"NVS_SIZE={nvs_size}")
PY
)"

  APP_OFFSET="$(printf '%s\n' "$output" | sed -n 's/^APP_OFFSET=//p' | head -n1)"
  MODEL_OFFSET="$(printf '%s\n' "$output" | sed -n 's/^MODEL_OFFSET=//p' | head -n1)"
  NVS_OFFSET="$(printf '%s\n' "$output" | sed -n 's/^NVS_OFFSET=//p' | head -n1)"
  NVS_SIZE="$(printf '%s\n' "$output" | sed -n 's/^NVS_SIZE=//p' | head -n1)"

  if [[ -z "$APP_OFFSET" ]]; then
    echo "Failed to resolve application offset from $PARTITIONS_CSV" >&2
    return 1
  fi
  if [[ -z "$MODEL_OFFSET" ]]; then
    echo "Failed to resolve model offset from $PARTITIONS_CSV" >&2
    return 1
  fi
  if [[ -z "$NVS_OFFSET" ]]; then
    echo "Failed to resolve NVS offset from $PARTITIONS_CSV" >&2
    return 1
  fi
  if [[ -z "$NVS_SIZE" ]]; then
    echo "Failed to resolve NVS size from $PARTITIONS_CSV" >&2
    return 1
  fi
}

cache_is_ready() {
  [[ -d "$FIRMWARE_DIR" ]] || return 1
  [[ -s "$FIRMWARE_BOOTLOADER" ]] || return 1
  [[ -s "$FIRMWARE_PARTITIONS" ]] || return 1
  [[ -s "$FIRMWARE_APP" ]] || return 1
  [[ -s "$FIRMWARE_SRMODELS" ]] || return 1
  return 0
}

sync_firmware_cache_from_build() {
  require_file "$BUILD_BOOTLOADER"
  require_file "$BUILD_PARTITIONS"
  require_file "$BUILD_APP"
  require_file "$BUILD_SRMODELS"
  load_partition_offsets

  mkdir -p "$FIRMWARE_DIR"
  cp -f "$BUILD_BOOTLOADER" "$FIRMWARE_BOOTLOADER"
  cp -f "$BUILD_PARTITIONS" "$FIRMWARE_PARTITIONS"
  cp -f "$BUILD_APP" "$FIRMWARE_APP"
  cp -f "$BUILD_SRMODELS" "$FIRMWARE_SRMODELS"
  # settings.bin is intentionally not cached in firmware/ to avoid committing local settings.
  rm -f "$FIRMWARE_SETTINGS"

  local wake_model git_commit utc_now
  wake_model="$(detect_wake_word_model)"
  git_commit="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
  utc_now="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

  cat > "$FIRMWARE_MANIFEST" <<EOF
GENERATED_AT_UTC=$utc_now
PIO_ENV=$ENV_NAME
WAKE_WORD_MODEL=$wake_model
GIT_COMMIT=$git_commit
FLASH_MODE=$FLASH_MODE
FLASH_FREQ=$FLASH_FREQ
FLASH_SIZE=$FLASH_SIZE
BOOTLOADER_OFFSET=$BOOTLOADER_OFFSET
PARTITION_TABLE_OFFSET=$PARTITION_TABLE_OFFSET
APP_OFFSET=$APP_OFFSET
MODEL_OFFSET=$MODEL_OFFSET
NVS_OFFSET=$NVS_OFFSET
EOF
}

ensure_esptool() {
  if [[ ! -f "$ESPTOOL_PY" ]]; then
    echo "Installing minimal upload dependency: tool-esptoolpy"
    "$PIO_BIN" pkg install --global --tool platformio/tool-esptoolpy
  fi
  if [[ ! -f "$ESPTOOL_PY" ]]; then
    echo "esptool.py was not found: $ESPTOOL_PY" >&2
    return 1
  fi
}

build_fresh_settings_image() {
  load_partition_offsets

  local settings_csv
  settings_csv="$(find_settings_csv || true)"
  if [[ -z "$settings_csv" ]]; then
    echo "settings.csv was not found. Expected one of:" >&2
    echo "  - $ROOT_DIR/settings.csv" >&2
    echo "  - $ROOT_DIR/config/settings.csv" >&2
    return 1
  fi

  ensure_nvs_generator
  mkdir -p "$BUILD_DIR"
  rm -f "$BUILD_SETTINGS"

  echo "Generating fresh settings.bin from: $settings_csv"
  "$PIO_PYTHON" -m esp_idf_nvs_partition_gen generate "$settings_csv" "$BUILD_SETTINGS" "$NVS_SIZE"
  require_file "$BUILD_SETTINGS"
}

resolve_port_or_fail() {
  local port
  port="$(resolve_upload_port || true)"
  if [[ -z "$port" ]]; then
    echo "Upload port was not detected." >&2
    echo "Set one of the following and retry:" >&2
    echo "  1) export UPLOAD_PORT=/dev/ttyACM0" >&2
    echo "  2) echo /dev/ttyACM0 > $UPLOAD_PORT_FILE" >&2
    echo "You can inspect ports with: $PIO_BIN device list" >&2
    return 1
  fi
  assert_upload_port_access "$port"
  echo "$port"
}

build_upload_and_cache() {
  echo "Firmware cache is missing, incomplete, or outdated. Building from sources..."
  "$PIO_BIN" run -e "$ENV_NAME" -t fullclean
  rm -f sdkconfig

  if ! "$PIO_BIN" run -e "$ENV_NAME"; then
    echo "Build error: firmware was not uploaded." >&2
    return 1
  fi

  sync_firmware_cache_from_build
  echo "Firmware cache updated in: $FIRMWARE_DIR"

  local upload_port
  upload_port="$(resolve_port_or_fail)"
  LAST_UPLOAD_PORT="$upload_port"
  echo "Using upload port: $upload_port"
  "$PIO_BIN" run -e "$ENV_NAME" -t upload --upload-port "$upload_port"
  echo "Source build upload completed."
}

upload_cached_firmware() {
  build_fresh_settings_image
  ensure_esptool

  local upload_port
  upload_port="$(resolve_port_or_fail)"
  LAST_UPLOAD_PORT="$upload_port"
  echo "Using upload port: $upload_port"

  local -a cmd=(
    "$PIO_PYTHON" "$ESPTOOL_PY"
    --chip "$MCU"
    --port "$upload_port"
    --baud "$UPLOAD_BAUD"
    --before default_reset
    --after hard_reset
    write_flash
    --flash_mode "$FLASH_MODE"
    --flash_freq "$FLASH_FREQ"
    --flash_size "$FLASH_SIZE"
    "$BOOTLOADER_OFFSET" "$FIRMWARE_BOOTLOADER"
    "$PARTITION_TABLE_OFFSET" "$FIRMWARE_PARTITIONS"
    "$APP_OFFSET" "$FIRMWARE_APP"
    "$MODEL_OFFSET" "$FIRMWARE_SRMODELS"
    "$NVS_OFFSET" "$BUILD_SETTINGS"
  )
  echo "Including fresh settings.bin generated from current settings."

  "${cmd[@]}"
  echo "Cached firmware upload completed without rebuild."
}

start_serial_monitor() {
  local monitor_port="$1"
  local monitor_baud monitor_project_dir
  monitor_baud="$(resolve_monitor_baud)"
  monitor_project_dir="$LOCAL_DIR/pio-monitor"
  mkdir -p "$monitor_project_dir"
  echo "Starting serial monitor on: $monitor_port (baud: $monitor_baud)"
  "$PIO_BIN" device monitor \
    -d "$monitor_project_dir" \
    --port "$monitor_port" \
    --baud "$monitor_baud"
}

ensure_local_platformio
export PLATFORMIO_CORE_DIR="$ROOT_DIR/.pio_core"
sync_wake_word_model_with_settings

if cache_is_ready && cache_wake_word_model_matches_settings; then
  echo "Found ready firmware cache in $FIRMWARE_DIR. Rebuilding settings and uploading..."
  upload_cached_firmware
else
  build_upload_and_cache
fi

start_serial_monitor "$LAST_UPLOAD_PORT"
