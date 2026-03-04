"""
Pre-build script for generating srmodels.bin

This script calls movemodel.py from the esp-sr component to generate
a binary file with speech recognition models.

IMPORTANT: The script does not stop the build if the component is not downloaded yet.
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

Import("env")  # type: ignore

print("--- [ESP-SR] Generating srmodels.bin ---")

project_dir = Path(env.subst("$PROJECT_DIR"))
build_dir = Path(env.subst("$BUILD_DIR"))
pio_env = env.subst("$PIOENV")

# Search for esp-sr component in multiple locations
possible_component_dirs = [
    project_dir / "components" / "esp-sr",
    project_dir / "managed_components" / "espressif__esp-sr",
    build_dir / "managed_components" / "espressif__esp-sr",
]

cache_root = project_dir / ".local" / "esp-sr-cache"
if cache_root.exists():
    for cached_dir in sorted(cache_root.glob("esp-sr-*"), reverse=True):
        # Keep cache as fallback only; prefer the exact managed component version
        # resolved for the current project to avoid sdkconfig/model mismatches.
        possible_component_dirs.append(cached_dir)

component_dir = None
for path in possible_component_dirs:
    if path.exists():
        component_dir = path.resolve()
        break


def parse_wake_model(lines: list[str]) -> str:
    pattern = re.compile(r'^CONFIG_WAKE_WORD_MODEL="([^"]+)"$')
    for line in lines:
        match = pattern.match(line.strip())
        if match:
            return match.group(1).strip()
    return ""


def model_to_symbol(model_name: str) -> str:
    normalized = re.sub(r"[^A-Z0-9]+", "_", model_name.upper()).strip("_")
    return f"CONFIG_SR_WN_{normalized}"


def ensure_wake_model_symbol(staged_sdkconfig: Path, component_root: Path) -> None:
    raw = staged_sdkconfig.read_text(encoding="utf-8")
    lines = raw.replace("\r\n", "\n").split("\n")
    wake_model = parse_wake_model(lines)
    if not wake_model:
        return

    wake_model_dir = component_root / "model" / "wakenet_model" / wake_model
    if not wake_model_dir.is_dir():
        raise ValueError(
            f"wake-word model '{wake_model}' was not found in "
            f"{component_root / 'model' / 'wakenet_model'}"
        )

    selected_symbol = model_to_symbol(wake_model)
    changed = False
    found_enabled = False

    for idx, line in enumerate(lines):
        trimmed = line.strip()
        if not trimmed:
            continue
        if trimmed.startswith("# " + selected_symbol + " is not set"):
            lines[idx] = selected_symbol + "=y"
            changed = True
            found_enabled = True
            continue
        if trimmed.startswith(selected_symbol + "="):
            if trimmed != selected_symbol + "=y":
                lines[idx] = selected_symbol + "=y"
                changed = True
            found_enabled = True
            continue

    if not found_enabled:
        lines.append(selected_symbol + "=y")
        changed = True

    if changed:
        output = "\n".join(lines)
        if not output.endswith("\n"):
            output += "\n"
        staged_sdkconfig.write_text(output, encoding="utf-8")
        print(f"--- [ESP-SR] Auto-enabled {selected_symbol} for model '{wake_model}' ---")

if not component_dir:
    print("--- [ESP-SR] Skip: esp-sr component not found")
    print("    Component will be downloaded during full CMake configuration")
    # Do NOT call SystemExit - allow build to continue
else:
    sdkconfig_candidates = []

    env_sdkconfig = env.get("SDKCONFIG")
    if env_sdkconfig:
        sdkconfig_candidates.append(Path(str(env_sdkconfig)))

    sdkconfig_candidates.extend([
        project_dir / f"sdkconfig.{pio_env}",
        project_dir / "sdkconfig",
        build_dir / "config" / "sdkconfig",
    ])

    sdkconfig_source = None
    for candidate in sdkconfig_candidates:
        if candidate.exists() and candidate.is_file():
            sdkconfig_source = candidate.resolve()
            break

    if not sdkconfig_source:
        print(f"--- [ESP-SR] Skip: sdkconfig not found")
    else:
        movemodel_script = component_dir / "model" / "movemodel.py"
        if not movemodel_script.exists():
            print(f"--- [ESP-SR] Skip: movemodel.py not found")
        else:
            sdkconfig_project_dir = build_dir / ".esp_sr_project"
            sdkconfig_project_dir.mkdir(parents=True, exist_ok=True)
            staged_sdkconfig = sdkconfig_project_dir / "sdkconfig"
            shutil.copy2(sdkconfig_source, staged_sdkconfig)
            try:
                ensure_wake_model_symbol(staged_sdkconfig, component_dir)
            except ValueError as exc:
                print(f"--- [ESP-SR] Error: {exc}")
                raise SystemExit(1)

            python_exe = env.subst("$PYTHONEXE")
            if not python_exe or "${" in python_exe:
                python_exe = sys.executable
            
            env_vars = os.environ.copy()
            env_vars["SDKCONFIG"] = str(sdkconfig_source)

            sdkconfig_arg_candidates = [
                str(staged_sdkconfig),   # esp-sr variants expecting file path
                str(sdkconfig_project_dir),  # esp-sr variants expecting directory with sdkconfig
            ]

            generation_succeeded = False
            for sdkconfig_arg in sdkconfig_arg_candidates:
                command = [
                    python_exe,
                    str(movemodel_script),
                    "-d1",
                    sdkconfig_arg,
                    "-d2",
                    str(component_dir),
                    "-d3",
                    str(build_dir),
                ]
                print(f"--- [ESP-SR] Command: {' '.join(command)}")
                try:
                    subprocess.run(command, check=True, env=env_vars)
                    generation_succeeded = True
                    break
                except subprocess.CalledProcessError as exc:
                    print(
                        f"--- [ESP-SR] movemodel.py failed for -d1={sdkconfig_arg} "
                        f"(code {exc.returncode})"
                    )
                except FileNotFoundError as exc:
                    print(f"--- [ESP-SR] File not found: {exc}")
                    raise SystemExit(1)

            if not generation_succeeded:
                print("--- [ESP-SR] Error: unable to generate srmodels.bin with supported -d1 formats")
                raise SystemExit(1)

            generated_file = build_dir / "srmodels" / "srmodels.bin"
            if not generated_file.exists():
                print(f"--- [ESP-SR] Error: generated file not found: {generated_file}")
                raise SystemExit(1)

            generated_size = generated_file.stat().st_size
            if generated_size <= 4:
                print(
                    f"--- [ESP-SR] Error: invalid srmodels.bin size: {generated_size} bytes "
                    "(expected real model payload)"
                )
                raise SystemExit(1)

            print("--- [ESP-SR] srmodels.bin generated successfully ---")

print("--- [ESP-SR] Pre-build completed ---")
