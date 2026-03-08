"""
Post-upload script for flashing settings.bin into the NVS partition.

The script flashes only if .pio/build/<env>/settings.bin exists.
"""

import hashlib
import json
import subprocess
from shlex import quote
from pathlib import Path

from SCons.Script import Import  # type: ignore

Import("env")  # type: ignore

NVS_PARTITION_NAME = "nvs"
SETTINGS_BIN_NAME = "settings.bin"
SETTINGS_META_SUFFIX = ".meta.json"
SETTINGS_CSV_CANDIDATES = (
    "settings.csv",
    "config/settings.csv",
)


def _parse_nvs_offset(partitions_path: Path):
    if not partitions_path.exists():
        raise FileNotFoundError(f"Partition table not found: {partitions_path}")

    with partitions_path.open("r", encoding="utf-8") as file:
        for line in file:
            clean = line.split("#", 1)[0].strip()
            if not clean:
                continue

            fields = [item.strip() for item in clean.split(",")]
            if len(fields) < 5:
                continue

            if fields[0] == NVS_PARTITION_NAME:
                offset = fields[3]
                if not offset:
                    raise ValueError(
                        f"NVS partition has empty offset in {partitions_path}"
                    )
                return offset

    raise ValueError(f"NVS partition '{NVS_PARTITION_NAME}' not found in {partitions_path}")


def _find_settings_csv(project_dir: Path):
    for rel_path in SETTINGS_CSV_CANDIDATES:
        candidate = project_dir / rel_path
        if candidate.exists():
            return candidate
    return None


def _sha256_file(path: Path):
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _flash_settings(target, source, env):
    build_dir = Path(env.subst("$BUILD_DIR"))
    settings_bin = build_dir / SETTINGS_BIN_NAME
    settings_meta = settings_bin.with_name(settings_bin.name + SETTINGS_META_SUFFIX)
    project_dir = Path(env.subst("$PROJECT_DIR"))

    if not settings_bin.exists():
        print(f"--- [NVS] Flash skipped: {settings_bin} not found")
        return

    if not settings_meta.exists():
        print(f"--- [NVS] Flash error: metadata file not found: {settings_meta}")
        env.Exit(1)
        return

    settings_csv = _find_settings_csv(project_dir)
    if not settings_csv:
        print("--- [NVS] Flash error: settings.csv not found in project")
        env.Exit(1)
        return

    try:
        metadata = json.loads(settings_meta.read_text(encoding="utf-8"))
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"--- [NVS] Flash error: failed to read metadata: {exc}")
        env.Exit(1)
        return

    expected_sha = str(metadata.get("settings_csv_sha256", "")).strip()
    current_sha = _sha256_file(settings_csv)
    if not expected_sha:
        print(f"--- [NVS] Flash error: settings fingerprint missing in {settings_meta}")
        env.Exit(1)
        return

    if current_sha != expected_sha:
        print(
            "--- [NVS] Flash error: settings.bin is stale for current settings.csv "
            f"(expected {expected_sha}, got {current_sha})"
        )
        env.Exit(1)
        return

    partitions_path = project_dir / "partitions.csv"

    try:
        nvs_offset = _parse_nvs_offset(partitions_path)
    except (FileNotFoundError, ValueError) as exc:
        print(f"--- [NVS] Flash error: {exc}")
        return

    port = env.subst("$UPLOAD_PORT") or env.AutodetectUploadPort()
    if not port:
        print("--- [NVS] Flash aborted: upload port was not detected")
        env.Exit(1)
        return

    baud = env.subst("$UPLOAD_SPEED") or "460800"
    mcu = env.BoardConfig().get("build.mcu", "esp32s3")

    cmd = [
        env.subst("$PYTHONEXE"),
        env.subst("$UPLOADER"),
        "--chip",
        mcu,
        "--port",
        port,
        "--baud",
        baud,
        "--before",
        "default_reset",
        "--after",
        "hard_reset",
        "write_flash",
        nvs_offset,
        str(settings_bin),
    ]

    size_kb = settings_bin.stat().st_size / 1024
    print(
        f"--- [NVS] Flashing {SETTINGS_BIN_NAME} ({size_kb:.1f} KB) "
        f"to {nvs_offset} ---"
    )
    print(
        f"--- [NVS] Settings source: {settings_csv} "
        f"(sha256={current_sha}, volume_pct={metadata.get('volume_pct', 'unknown')}) ---"
    )
    print(f"--- [NVS] Command: {' '.join(quote(part) for part in cmd)}")

    proc = subprocess.run(cmd, check=False)
    if proc.returncode != 0:
        print(f"--- [NVS] Flash failed with exit code {proc.returncode}")
        env.Exit(1)
        return

    print("--- [NVS] settings.bin flashed successfully ---")


env.AddPostAction("upload", _flash_settings)
