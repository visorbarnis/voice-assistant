"""
Pre-build script for generating srmodels.bin

This script calls movemodel.py from the esp-sr component to generate
a binary file with speech recognition models.

IMPORTANT: The script does not stop the build if the component is not downloaded yet.
"""

import os
import subprocess
import sys
from pathlib import Path

Import("env")  # type: ignore

print("--- [ESP-SR] Generating srmodels.bin ---")

project_dir = Path(env.subst("$PROJECT_DIR"))
build_dir = Path(env.subst("$BUILD_DIR"))

# Search for esp-sr component in multiple locations
possible_component_dirs = [
    project_dir / "components" / "esp-sr",
    project_dir / "managed_components" / "espressif__esp-sr",
    build_dir / "managed_components" / "espressif__esp-sr",
]

cache_root = project_dir / ".local" / "esp-sr-cache"
if cache_root.exists():
    for cached_dir in sorted(cache_root.glob("esp-sr-*"), reverse=True):
        possible_component_dirs.insert(0, cached_dir)

component_dir = None
for path in possible_component_dirs:
    if path.exists():
        component_dir = path.resolve()
        break

if not component_dir:
    print("--- [ESP-SR] Skip: esp-sr component not found")
    print("    Component will be downloaded during full CMake configuration")
    # Do NOT call SystemExit - allow build to continue
else:
    sdkconfig_path = project_dir / f"sdkconfig.{env.subst('$PIOENV')}"
    if not sdkconfig_path.exists():
        sdkconfig_path = project_dir / "sdkconfig"
    
    if not sdkconfig_path.exists():
        print(f"--- [ESP-SR] Skip: sdkconfig not found")
    else:
        movemodel_script = component_dir / "model" / "movemodel.py"
        if not movemodel_script.exists():
            print(f"--- [ESP-SR] Skip: movemodel.py not found")
        else:
            python_exe = env.subst("$PYTHONEXE")
            if not python_exe or "${" in python_exe:
                python_exe = sys.executable
            
            command = [
                python_exe,
                str(movemodel_script),
                "-d1", str(sdkconfig_path),
                "-d2", str(component_dir),
                "-d3", str(build_dir),
            ]
            
            try:
                print(f"--- [ESP-SR] Command: {' '.join(command)}")
                env_vars = os.environ.copy()
                env_vars["SDKCONFIG"] = str(sdkconfig_path)
                subprocess.run(command, check=True, env=env_vars)
                print("--- [ESP-SR] srmodels.bin generated successfully ---")
            except subprocess.CalledProcessError as exc:
                print(f"--- [ESP-SR] movemodel.py error: code {exc.returncode}")
            except FileNotFoundError as exc:
                print(f"--- [ESP-SR] File not found: {exc}")

print("--- [ESP-SR] Pre-build completed ---")
