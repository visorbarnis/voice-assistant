"""
Pre-build script for generating settings.bin from settings.csv.

Expected CSV format follows ESP-IDF nvs_partition_gen.py conventions.
If settings.csv is missing, generation is skipped.
"""

import os
import subprocess
import sys
from pathlib import Path

from SCons.Script import Import  # type: ignore

Import("env")  # type: ignore

NVS_PARTITION_NAME = "nvs"
SETTINGS_CSV_CANDIDATES = [
    "settings.csv",
    "config/settings.csv",
]


def _find_settings_csv(project_dir: Path):
    for rel_path in SETTINGS_CSV_CANDIDATES:
        candidate = project_dir / rel_path
        if candidate.exists():
            return candidate
    return None


def _parse_nvs_partition(partitions_path: Path):
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
                size = fields[4]
                if not offset or not size:
                    raise ValueError(
                        f"NVS partition has empty offset/size in {partitions_path}"
                    )
                return offset, size

    raise ValueError(f"NVS partition '{NVS_PARTITION_NAME}' not found in {partitions_path}")


def _find_nvs_generator_script():
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        candidate = (
            Path(idf_path)
            / "components"
            / "nvs_flash"
            / "nvs_partition_generator"
            / "nvs_partition_gen.py"
        )
        if candidate.exists():
            return candidate

    framework_dir = env.PioPlatform().get_package_dir("framework-espidf")
    if framework_dir:
        candidate = (
            Path(framework_dir)
            / "components"
            / "nvs_flash"
            / "nvs_partition_generator"
            / "nvs_partition_gen.py"
        )
        if candidate.exists():
            return candidate

    return None


def _ensure_nvs_generator_module(python_exe: str):
    check_cmd = [python_exe, "-c", "import esp_idf_nvs_partition_gen"]
    check = subprocess.run(check_cmd, capture_output=True, text=True)
    if check.returncode == 0:
        return

    print("--- [NVS] Missing module: esp_idf_nvs_partition_gen")
    print("--- [NVS] Installing esp-idf-nvs-partition-gen into PlatformIO env")

    install_cmd = [
        python_exe,
        "-m",
        "pip",
        "install",
        "--disable-pip-version-check",
        "esp-idf-nvs-partition-gen",
    ]

    try:
        subprocess.run(install_cmd, check=True)
    except subprocess.CalledProcessError as exc:
        print(f"--- [NVS] Failed to install dependency: {exc}")
        raise SystemExit(1) from exc


print("--- [NVS] Generating settings.bin ---")

project_dir = Path(env.subst("$PROJECT_DIR"))
build_dir = Path(env.subst("$BUILD_DIR"))
settings_csv = _find_settings_csv(project_dir)

if not settings_csv:
    print("--- [NVS] Skip: settings.csv not found")
else:
    partitions_path = project_dir / "partitions.csv"

    try:
        nvs_offset, nvs_size = _parse_nvs_partition(partitions_path)
    except (FileNotFoundError, ValueError) as exc:
        print(f"--- [NVS] Error: {exc}")
        raise SystemExit(1) from exc

    settings_bin = build_dir / "settings.bin"
    settings_bin.parent.mkdir(parents=True, exist_ok=True)

    python_exe = env.subst("$PYTHONEXE")
    if not python_exe or "${" in python_exe:
        python_exe = sys.executable

    generator_script = _find_nvs_generator_script()
    generator_cmd = None
    generator_name = ""

    # IDF 5.5 nvs_partition_gen.py is only a thin wrapper that runs:
    #   python -m esp_idf_nvs_partition_gen
    # Ensure this module is available regardless of backend choice.
    _ensure_nvs_generator_module(python_exe)

    if generator_script:
        generator_cmd = [python_exe, str(generator_script)]
        generator_name = str(generator_script)
    else:
        print(
            "--- [NVS] nvs_partition_gen.py not found in IDF/framework; "
            "falling back to Python module backend"
        )
        generator_cmd = [python_exe, "-m", "esp_idf_nvs_partition_gen"]
        generator_name = "esp_idf_nvs_partition_gen (module)"

    command = [
        *generator_cmd,
        "generate",
        str(settings_csv),
        str(settings_bin),
        nvs_size,
    ]

    print(
        "--- [NVS] settings.csv="
        f"{settings_csv} -> settings.bin={settings_bin} "
        f"(offset={nvs_offset}, size={nvs_size})"
    )
    print(f"--- [NVS] Generator: {generator_name}")
    print(f"--- [NVS] Command: {' '.join(command)}")

    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as exc:
        print(f"--- [NVS] Generator failed with code {exc.returncode}")
        raise SystemExit(exc.returncode) from exc

    size_bytes = settings_bin.stat().st_size
    print(f"--- [NVS] settings.bin generated ({size_bytes} bytes)")
