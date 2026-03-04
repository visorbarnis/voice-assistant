"""
Post-upload script for flashing srmodels.bin to the model partition

This script automatically flashes models after the main firmware upload.
Models are written to address 0x610000 (the 'model' partition in partitions.csv).
"""

import subprocess
from shlex import quote
from pathlib import Path

from SCons.Script import Import  # type: ignore

Import("env")  # type: ignore

# Model partition address in flash (must match partitions.csv)
SRMODEL_OFFSET = "0x610000"
MIN_VALID_SRMODEL_SIZE_BYTES = 5


def _flash_srmodels(target, source, env):
    """Flash the model file after the main firmware upload."""
    build_dir = Path(env.subst("$BUILD_DIR"))
    srmodels_path = build_dir / "srmodels" / "srmodels.bin"
    
    if not srmodels_path.exists():
        print(f"--- [ESP-SR] Flash skipped: {srmodels_path} not found")
        return
    
    size_bytes = srmodels_path.stat().st_size
    if size_bytes < MIN_VALID_SRMODEL_SIZE_BYTES:
        print(
            f"--- [ESP-SR] Flash aborted: invalid srmodels.bin size={size_bytes} bytes "
            "(models are not packed)"
        )
        env.Exit(1)
        return

    size_kb = size_bytes / 1024
    print(f"--- [ESP-SR] Flashing srmodels.bin ({size_kb:.1f} KB) to address {SRMODEL_OFFSET} ---")
    
    port = env.subst("$UPLOAD_PORT") or env.AutodetectUploadPort()
    if not port:
        print("--- [ESP-SR] Flash aborted: upload port was not detected")
        env.Exit(1)
        return

    baud = env.subst("$UPLOAD_SPEED") or "460800"
    mcu = env.BoardConfig().get("build.mcu", "esp32s3")
    
    cmd = [
        env.subst("$PYTHONEXE"),
        env.subst("$UPLOADER"),
        "--chip", mcu,
        "--port", port,
        "--baud", baud,
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        SRMODEL_OFFSET,
        str(srmodels_path),
    ]
    
    print(f"--- [ESP-SR] Command: {' '.join(quote(part) for part in cmd)}")
    proc = subprocess.run(cmd, check=False)
    if proc.returncode != 0:
        print(f"--- [ESP-SR] Flash failed with exit code {proc.returncode}")
        env.Exit(1)
        return

    print("--- [ESP-SR] Models flashed successfully ---")


# Add post-action to upload target
env.AddPostAction("upload", _flash_srmodels)
