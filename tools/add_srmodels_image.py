"""
Post-build script for adding srmodels.bin to the firmware image

Checks whether the generated model file exists.
"""

import os
from pathlib import Path

from SCons.Script import Import  # type: ignore

Import("env")  # type: ignore

print("--- [ESP-SR] Checking srmodels.bin ---")

build_dir = Path(env.subst("$BUILD_DIR"))
srmodels_path = build_dir / "srmodels" / "srmodels.bin"

if srmodels_path.exists():
    size_kb = srmodels_path.stat().st_size / 1024
    print(f"--- [ESP-SR] srmodels.bin found: {size_kb:.1f} KB ---")
else:
    print(f"--- [ESP-SR] Warning: srmodels.bin not found at {srmodels_path}")
    print("    Models will be generated during the next build")
