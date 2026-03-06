param(
    [string]$EnvName = $(if ($env:PIO_ENV) { $env:PIO_ENV } else { "esp32-s3-devkitc-1" })
)

$ErrorActionPreference = "Stop"

$RootDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$LocalDir = Join-Path $RootDir ".local"
$PioVenvDir = Join-Path $LocalDir "pio-venv"
$PioPython = Join-Path $PioVenvDir "Scripts\python.exe"
$PioExe = Join-Path $PioVenvDir "Scripts\pio.exe"
$MiniforgeDir = Join-Path $LocalDir "miniforge3"
$MiniforgePython = Join-Path $MiniforgeDir "python.exe"
$UploadPortFile = Join-Path $LocalDir "upload_port"
$ProjectConfigPath = Join-Path $RootDir "platformio.ini"

$FirmwareDir = Join-Path $RootDir "firmware"
$FirmwareManifest = Join-Path $FirmwareDir "manifest.env"
$FirmwareBootloader = Join-Path $FirmwareDir "bootloader.bin"
$FirmwarePartitions = Join-Path $FirmwareDir "partitions.bin"
$FirmwareApp = Join-Path $FirmwareDir "firmware.bin"
$FirmwareSrmodels = Join-Path $FirmwareDir "srmodels.bin"
$FirmwareSettings = Join-Path $FirmwareDir "settings.bin"

$BuildDir = Join-Path $RootDir ".pio\build\$EnvName"
$BuildBootloader = Join-Path $BuildDir "bootloader.bin"
$BuildPartitions = Join-Path $BuildDir "partitions.bin"
$BuildApp = Join-Path $BuildDir "firmware.bin"
$BuildSrmodels = Join-Path $BuildDir "srmodels\srmodels.bin"
$BuildSettings = Join-Path $BuildDir "settings.bin"

$PartitionsCsv = Join-Path $RootDir "partitions.csv"
$EsptoolPy = Join-Path $RootDir ".pio_core\packages\tool-esptoolpy\esptool.py"

$BootloaderOffset = "0x0"
$PartitionTableOffset = "0x8000"
$FlashMode = if ($env:FLASH_MODE) { $env:FLASH_MODE } else { "dio" }
$FlashFreq = if ($env:FLASH_FREQ) { $env:FLASH_FREQ } else { "80m" }
$FlashSize = if ($env:FLASH_SIZE) { $env:FLASH_SIZE } else { "16MB" }
$UploadBaud = if ($env:UPLOAD_BAUD) { $env:UPLOAD_BAUD } else { "115200" }
$Mcu = if ($env:MCU) { $env:MCU } else { "esp32s3" }

$script:LastUploadPort = ""
$script:AppOffset = ""
$script:ModelOffset = ""
$script:NvsOffset = ""
$script:NvsSize = ""

Push-Location $RootDir

try {
    function Ensure-Windows {
        if (($env:OS -eq "Windows_NT") -or ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT)) {
            return
        }
        throw "run_upload_firmware.ps1 is intended for Windows. Use run_upload_firmware.sh on Unix-like systems."
    }

    function Resolve-WindowsArch {
        try {
            $cpuArch = Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Architecture
            switch ([int]$cpuArch) {
                12 { return "arm64" }
                9 { return "x86_64" }
            }
        }
        catch {
        }

        $archRaw = $env:PROCESSOR_ARCHITECTURE
        if ($env:PROCESSOR_ARCHITEW6432) {
            $archRaw = $env:PROCESSOR_ARCHITEW6432
        }
        $archRaw = $archRaw.ToUpperInvariant()

        switch ($archRaw) {
            "AMD64" { return "x86_64" }
            "X64" { return "x86_64" }
            "ARM64" { return "arm64" }
            default { throw "Unsupported architecture for local Python bootstrap: $archRaw" }
        }
    }

    function Download-File {
        param(
            [Parameter(Mandatory = $true)][string]$Url,
            [Parameter(Mandatory = $true)][string]$OutFile
        )

        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $OutFile
    }

    function Install-LocalPython {
        if (Test-Path $MiniforgePython) {
            return
        }

        $arch = Resolve-WindowsArch
        $candidateInstallers = @("Miniforge3-Windows-$arch.exe")
        if ($arch -eq "arm64") {
            $candidateInstallers += "Miniforge3-Windows-x86_64.exe"
        }

        Write-Host "Installing local Python (Miniforge) into: $MiniforgeDir"

        New-Item -ItemType Directory -Force -Path $LocalDir | Out-Null
        if (Test-Path $MiniforgeDir) {
            Remove-Item -Recurse -Force $MiniforgeDir
        }

        $installerPath = $null

        try {
            foreach ($installerName in $candidateInstallers) {
                $url = "https://github.com/conda-forge/miniforge/releases/latest/download/$installerName"
                $candidatePath = Join-Path $LocalDir $installerName
                try {
                    Download-File -Url $url -OutFile $candidatePath
                    $installerPath = $candidatePath
                    if ($installerName -ne "Miniforge3-Windows-$arch.exe") {
                        Write-Host "Miniforge fallback selected: $installerName"
                    }
                    break
                }
                catch {
                    Write-Host "Installer unavailable: $installerName"
                    Remove-Item -Force $candidatePath -ErrorAction SilentlyContinue
                }
            }

            if (-not $installerPath) {
                throw "Failed to download Miniforge installer for Windows/$arch."
            }

            $arguments = @(
                "/InstallationType=JustMe"
                "/RegisterPython=0"
                "/AddToPath=0"
                "/S"
                "/D=$MiniforgeDir"
            )

            $proc = Start-Process -FilePath $installerPath -ArgumentList $arguments -PassThru -Wait
            if ($proc.ExitCode -ne 0) {
                throw "Miniforge installer exited with code $($proc.ExitCode)."
            }
        }
        finally {
            Remove-Item -Force $installerPath -ErrorAction SilentlyContinue
        }

        if (-not (Test-Path $MiniforgePython)) {
            throw "Local Python bootstrap failed: $MiniforgePython not found."
        }
    }

    function Resolve-PythonLauncher {
        if (Test-Path $MiniforgePython) {
            return [pscustomobject]@{ Command = $MiniforgePython; Prefix = @() }
        }

        $localPython = Join-Path $LocalDir "python\python.exe"
        if (Test-Path $localPython) {
            return [pscustomobject]@{ Command = $localPython; Prefix = @() }
        }

        return $null
    }

    function Invoke-Python {
        param(
            [Parameter(Mandatory = $true)][psobject]$Launcher,
            [Parameter(Mandatory = $true)][string[]]$Args
        )

        & $Launcher.Command @($Launcher.Prefix) @Args
        return $LASTEXITCODE
    }

    function Ensure-LocalPythonLauncher {
        $launcher = Resolve-PythonLauncher
        if ($launcher) {
            return $launcher
        }

        Install-LocalPython

        $launcher = Resolve-PythonLauncher
        if (-not $launcher) {
            throw "Python 3 not found and local bootstrap failed."
        }

        return $launcher
    }

    function Ensure-PipInVenv {
        if (-not (Test-Path $PioPython)) {
            return $false
        }

        & $PioPython -m pip --version *> $null
        if ($LASTEXITCODE -eq 0) {
            return $true
        }

        & $PioPython -m ensurepip --upgrade *> $null
        & $PioPython -m pip --version *> $null
        if ($LASTEXITCODE -eq 0) {
            return $true
        }

        $getPipPath = Join-Path $LocalDir "get-pip.py"
        try {
            Download-File -Url "https://bootstrap.pypa.io/get-pip.py" -OutFile $getPipPath
            & $PioPython $getPipPath *> $null
            & $PioPython -m pip --version *> $null
            return ($LASTEXITCODE -eq 0)
        }
        finally {
            Remove-Item -Force $getPipPath -ErrorAction SilentlyContinue
        }
    }

    function Ensure-LocalPlatformIO {
        $launcher = Ensure-LocalPythonLauncher

        function Recreate-LocalVenv {
            Write-Host "Recreating local PlatformIO environment: $PioVenvDir"
            if (Test-Path $PioVenvDir) {
                Remove-Item -Recurse -Force $PioVenvDir
            }
            New-Item -ItemType Directory -Force -Path $LocalDir | Out-Null

            $code = Invoke-Python -Launcher $launcher -Args @("-m", "venv", $PioVenvDir)
            if ($code -eq 0) {
                return
            }

            $codeNoPip = Invoke-Python -Launcher $launcher -Args @("-m", "venv", "--without-pip", $PioVenvDir)
            if ($codeNoPip -ne 0) {
                throw "Failed to create local Python venv at $PioVenvDir"
            }

            if (-not (Ensure-PipInVenv)) {
                throw "Failed to bootstrap pip in local venv at $PioVenvDir"
            }
        }

        foreach ($attempt in 1..2) {
            if (-not (Test-Path $PioPython)) {
                Recreate-LocalVenv
            }

            & $PioPython -c "import sys; print(sys.version)" *> $null
            if ($LASTEXITCODE -ne 0) {
                Write-Host "Local venv python is broken. Cleaning and retrying..."
                if (Test-Path $PioVenvDir) {
                    Remove-Item -Recurse -Force $PioVenvDir
                }
                continue
            }

            if (-not (Ensure-PipInVenv)) {
                Write-Host "pip is unavailable in local venv. Cleaning and retrying..."
                if (Test-Path $PioVenvDir) {
                    Remove-Item -Recurse -Force $PioVenvDir
                }
                continue
            }

            if (-not (Test-Path $PioExe)) {
                & $PioPython -m pip install --upgrade pip setuptools wheel
                if ($LASTEXITCODE -ne 0) {
                    Write-Host "Failed to upgrade pip in local venv. Cleaning and retrying..."
                    if (Test-Path $PioVenvDir) {
                        Remove-Item -Recurse -Force $PioVenvDir
                    }
                    continue
                }

                & $PioPython -m pip install --upgrade platformio
                if ($LASTEXITCODE -ne 0) {
                    Write-Host "Failed to install PlatformIO in local venv. Cleaning and retrying..."
                    if (Test-Path $PioVenvDir) {
                        Remove-Item -Recurse -Force $PioVenvDir
                    }
                    continue
                }
            }

            & $PioExe --version *> $null
            if ($LASTEXITCODE -eq 0) {
                return
            }

            Write-Host "Local PlatformIO is not healthy. Cleaning and retrying..."
            if (Test-Path $PioVenvDir) {
                Remove-Item -Recurse -Force $PioVenvDir
            }
        }

        throw "Failed to prepare local PlatformIO CLI in $PioVenvDir"
    }

    function Resolve-ProjectConfigPath {
        $baseConfig = Join-Path $RootDir "platformio.ini"
        if (-not (Test-Path $baseConfig)) {
            throw "platformio.ini not found: $baseConfig"
        }

        if ((Resolve-WindowsArch) -ne "arm64") {
            return $baseConfig
        }

        $primarySpec = "platformio/toolchain-riscv32-esp@14.2.0+20241119"
        $fallbackSpec = "platformio/toolchain-riscv32-esp@13.2.0+20240530"
        $content = Get-Content -Raw -Path $baseConfig

        if ($content -notmatch [regex]::Escape($primarySpec)) {
            return $baseConfig
        }

        $patched = $content -replace [regex]::Escape($primarySpec), $fallbackSpec
        New-Item -ItemType Directory -Force -Path $LocalDir | Out-Null

        $arm64Config = Join-Path $LocalDir "platformio.windows-arm64.ini"
        Set-Content -Path $arm64Config -Value $patched -Encoding Ascii
        Write-Host "Windows ARM64 detected: using fallback toolchain '$fallbackSpec'."
        return $arm64Config
    }

    function Invoke-Pio {
        param(
            [Parameter(Mandatory = $true)][string[]]$Args
        )

        $effectiveArgs = @($Args)
        if ($effectiveArgs.Count -gt 0 -and $effectiveArgs[0] -eq "run") {
            $tail = @()
            if ($effectiveArgs.Count -gt 1) {
                $tail = $effectiveArgs[1..($effectiveArgs.Count - 1)]
            }
            $effectiveArgs = @("run", "-d", $RootDir, "-c", $ProjectConfigPath) + $tail
        }

        & $PioExe @effectiveArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed: $PioExe $($effectiveArgs -join ' ')"
        }
    }

    function Resolve-UploadPort {
        $envPort = $null
        if ($env:UPLOAD_PORT) {
            $envPort = $env:UPLOAD_PORT.Trim()
        }

        $savedPort = $null
        if (Test-Path $UploadPortFile) {
            $saved = (Get-Content -Path $UploadPortFile -ErrorAction SilentlyContinue | Select-Object -First 1)
            if ($saved) {
                $saved = $saved.Trim()
                if ($saved) {
                    $savedPort = $saved
                }
            }
        }

        $devices = @()
        $json = & $PioExe device list --json-output 2>$null
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($json)) {
            try {
                $devices = @($json | ConvertFrom-Json)
            }
            catch {
                $devices = @()
            }
        }

        if (-not [string]::IsNullOrWhiteSpace($envPort)) {
            if ($devices.Count -gt 0) {
                $match = @($devices | Where-Object { ([string]$_.port).Trim() -ieq $envPort })
                if ($match.Count -eq 0) {
                    Write-Warning "UPLOAD_PORT points to missing device: $envPort"
                }
            }
            return $envPort
        }

        if (-not [string]::IsNullOrWhiteSpace($savedPort)) {
            if ($devices.Count -eq 0) {
                return $savedPort
            }

            $savedMatch = @($devices | Where-Object { ([string]$_.port).Trim() -ieq $savedPort })
            if ($savedMatch.Count -gt 0) {
                return $savedPort
            }

            Write-Host "Saved upload port is stale: $savedPort. Auto-detecting active serial port..."
            Remove-Item -Path $UploadPortFile -Force -ErrorAction SilentlyContinue
        }

        if (-not $devices -or $devices.Count -eq 0) {
            return $null
        }

        $scored = @()
        foreach ($d in $devices) {
            $port = [string]$d.port
            if ([string]::IsNullOrWhiteSpace($port)) {
                continue
            }
            $text = ("{0} {1} {2} {3} {4}" -f $d.description, $d.hwid, $d.manufacturer, $d.product, $d.port).ToLowerInvariant()
            $score = 0
            if ($text -match "303a|espressif|esp32|cp210|ch340|ftdi|usb serial|usb jtag|uart") {
                $score += 10
            }
            if ($port -match "^COM\\d+$") {
                $score += 1
            }
            $scored += [pscustomobject]@{ Score = $score; Port = $port }
        }

        if (-not $scored -or $scored.Count -eq 0) {
            return $null
        }

        $topScore = ($scored | Measure-Object -Property Score -Maximum).Maximum
        $top = @($scored | Where-Object { $_.Score -eq $topScore })

        if ($topScore -gt 0 -and $top.Count -eq 1) {
            return $top[0].Port
        }
        if ($scored.Count -eq 1) {
            return $scored[0].Port
        }

        return $null
    }

    function Require-File {
        param(
            [Parameter(Mandatory = $true)][string]$Path
        )

        if (-not (Test-Path $Path)) {
            throw "Required file is missing: $Path"
        }

        $item = Get-Item -Path $Path
        if ($item.PSIsContainer -or $item.Length -le 0) {
            throw "Required file is missing or empty: $Path"
        }
    }

    function Find-SettingsCsv {
        $candidates = @(
            (Join-Path $RootDir "settings.csv")
            (Join-Path $RootDir "config\settings.csv")
        )

        foreach ($candidate in $candidates) {
            if (Test-Path $candidate) {
                return $candidate
            }
        }
        return $null
    }

    function Detect-SettingsWakeWordModel {
        $settingsCsv = Find-SettingsCsv
        if ([string]::IsNullOrWhiteSpace($settingsCsv)) {
            return $null
        }

        $code = @'
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
'@

        $result = $code | & $PioPython - $settingsCsv
        if ($LASTEXITCODE -ne 0) {
            return $null
        }

        $value = (($result -join "`n").Trim())
        if ([string]::IsNullOrWhiteSpace($value)) {
            return $null
        }
        return $value
    }

    function Ensure-NvsGenerator {
        & $PioPython -c "import esp_idf_nvs_partition_gen" *> $null
        if ($LASTEXITCODE -eq 0) {
            return
        }

        Write-Host "Installing minimal settings generator dependency: esp-idf-nvs-partition-gen"
        & $PioPython -m pip install --disable-pip-version-check --upgrade esp-idf-nvs-partition-gen
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to install esp-idf-nvs-partition-gen."
        }
    }

    function Resolve-MonitorBaud {
        if ($env:MONITOR_BAUD -and ($env:MONITOR_BAUD.Trim() -match '^\d+$')) {
            return $env:MONITOR_BAUD.Trim()
        }

        $confFile = Join-Path $RootDir "platformio.ini"
        if (-not (Test-Path $confFile)) {
            if ($UploadBaud -match '^\d+$') {
                return $UploadBaud
            }
            return "115200"
        }

        $code = @'
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
'@

        $detected = $code | & $PioPython - $confFile $EnvName
        if ($LASTEXITCODE -eq 0) {
            $detectedValue = (($detected -join "`n").Trim())
            if ($detectedValue -match '^\d+$') {
                return $detectedValue
            }
        }

        if ($UploadBaud -match '^\d+$') {
            return $UploadBaud
        }
        return "115200"
    }

    function Detect-WakeWordModel {
        $candidates = @(
            (Join-Path $RootDir "sdkconfig.$EnvName")
            (Join-Path $RootDir "sdkconfig.defaults")
        )

        foreach ($path in $candidates) {
            if (-not (Test-Path $path)) {
                continue
            }

            $match = Select-String -Path $path -Pattern '^CONFIG_WAKE_WORD_MODEL=\"([^\"]*)\"$' -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($match -and $match.Matches.Count -gt 0) {
                return $match.Matches[0].Groups[1].Value
            }
        }

        return "unknown"
    }

    function Detect-CachedWakeWordModel {
        if (-not (Test-Path $FirmwareManifest)) {
            return $null
        }

        $match = Select-String -Path $FirmwareManifest -Pattern '^WAKE_WORD_MODEL=(.*)$' -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $match -or $match.Matches.Count -eq 0) {
            return $null
        }

        $value = $match.Matches[0].Groups[1].Value.Trim()
        if ([string]::IsNullOrWhiteSpace($value)) {
            return $null
        }
        return $value
    }

    function Sync-WakeWordModelWithSettings {
        $settingsModel = Detect-SettingsWakeWordModel
        if ([string]::IsNullOrWhiteSpace($settingsModel)) {
            Write-Warning "wake_word_model was not found in settings.csv. Keeping current build model."
            return
        }

        $currentModel = Detect-WakeWordModel
        if ($currentModel -eq $settingsModel) {
            return
        }

        Write-Host "Wake-word model from settings: $settingsModel (build config: $currentModel). Updating sdkconfig files..."

        $code = @'
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
'@

        $null = $code | & $PioPython - $RootDir $EnvName $settingsModel
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to sync wake-word model with settings."
        }
    }

    function Cache-WakeWordModelMatchesSettings {
        $settingsModel = Detect-SettingsWakeWordModel
        if ([string]::IsNullOrWhiteSpace($settingsModel)) {
            Write-Warning "wake_word_model was not found in settings.csv. Skipping firmware model cache check."
            return $true
        }

        $cachedModel = Detect-CachedWakeWordModel
        if ([string]::IsNullOrWhiteSpace($cachedModel)) {
            Write-Host "Firmware cache model is unknown (manifest WAKE_WORD_MODEL is missing). Rebuild required."
            return $false
        }

        if ($cachedModel -ne $settingsModel) {
            Write-Host "Firmware cache model '$cachedModel' differs from settings model '$settingsModel'. Rebuild required."
            return $false
        }

        return $true
    }

    function Load-PartitionOffsets {
        if (-not (Test-Path $PartitionsCsv)) {
            throw "Partition file not found: $PartitionsCsv"
        }

        $code = @'
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
'@

        $output = $code | & $PioPython - $PartitionsCsv
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to resolve partition offsets from $PartitionsCsv"
        }

        $values = @{
            APP_OFFSET = ""
            MODEL_OFFSET = ""
            NVS_OFFSET = ""
            NVS_SIZE = ""
        }

        foreach ($line in @($output)) {
            if ($line -match '^([A-Z_]+)=(.*)$') {
                $key = $matches[1]
                $val = $matches[2].Trim()
                if ($values.ContainsKey($key)) {
                    $values[$key] = $val
                }
            }
        }

        $script:AppOffset = $values["APP_OFFSET"]
        $script:ModelOffset = $values["MODEL_OFFSET"]
        $script:NvsOffset = $values["NVS_OFFSET"]
        $script:NvsSize = $values["NVS_SIZE"]

        if ([string]::IsNullOrWhiteSpace($script:AppOffset)) {
            throw "Failed to resolve application offset from $PartitionsCsv"
        }
        if ([string]::IsNullOrWhiteSpace($script:ModelOffset)) {
            throw "Failed to resolve model offset from $PartitionsCsv"
        }
        if ([string]::IsNullOrWhiteSpace($script:NvsOffset)) {
            throw "Failed to resolve NVS offset from $PartitionsCsv"
        }
        if ([string]::IsNullOrWhiteSpace($script:NvsSize)) {
            throw "Failed to resolve NVS size from $PartitionsCsv"
        }
    }

    function Cache-IsReady {
        $files = @(
            $FirmwareBootloader
            $FirmwarePartitions
            $FirmwareApp
            $FirmwareSrmodels
        )

        if (-not (Test-Path $FirmwareDir)) {
            return $false
        }

        foreach ($file in $files) {
            if (-not (Test-Path $file)) {
                return $false
            }
            $item = Get-Item -Path $file
            if ($item.PSIsContainer -or $item.Length -le 0) {
                return $false
            }
        }
        return $true
    }

    function Sync-FirmwareCacheFromBuild {
        Require-File -Path $BuildBootloader
        Require-File -Path $BuildPartitions
        Require-File -Path $BuildApp
        Require-File -Path $BuildSrmodels
        Load-PartitionOffsets

        New-Item -ItemType Directory -Force -Path $FirmwareDir | Out-Null
        Copy-Item -Path $BuildBootloader -Destination $FirmwareBootloader -Force
        Copy-Item -Path $BuildPartitions -Destination $FirmwarePartitions -Force
        Copy-Item -Path $BuildApp -Destination $FirmwareApp -Force
        Copy-Item -Path $BuildSrmodels -Destination $FirmwareSrmodels -Force
        Remove-Item -Path $FirmwareSettings -Force -ErrorAction SilentlyContinue

        $wakeModel = Detect-WakeWordModel
        $gitCommit = "unknown"
        $gitOut = & git -C $RootDir rev-parse --short HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($gitOut)) {
            $gitCommit = (@($gitOut) -join "`n").Trim()
        }
        $utcNow = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")

        $manifest = @"
GENERATED_AT_UTC=$utcNow
PIO_ENV=$EnvName
WAKE_WORD_MODEL=$wakeModel
GIT_COMMIT=$gitCommit
FLASH_MODE=$FlashMode
FLASH_FREQ=$FlashFreq
FLASH_SIZE=$FlashSize
BOOTLOADER_OFFSET=$BootloaderOffset
PARTITION_TABLE_OFFSET=$PartitionTableOffset
APP_OFFSET=$script:AppOffset
MODEL_OFFSET=$script:ModelOffset
NVS_OFFSET=$script:NvsOffset
"@
        Set-Content -Path $FirmwareManifest -Value $manifest -Encoding Ascii
    }

    function Ensure-Esptool {
        if (-not (Test-Path $EsptoolPy)) {
            Write-Host "Installing minimal upload dependency: tool-esptoolpy"
            & $PioExe pkg install --global --tool platformio/tool-esptoolpy
            if ($LASTEXITCODE -ne 0) {
                throw "Failed to install platformio/tool-esptoolpy."
            }
        }
        if (-not (Test-Path $EsptoolPy)) {
            throw "esptool.py was not found: $EsptoolPy"
        }
    }

    function Build-FreshSettingsImage {
        Load-PartitionOffsets

        $settingsCsv = Find-SettingsCsv
        if ([string]::IsNullOrWhiteSpace($settingsCsv)) {
            throw ("settings.csv was not found. Expected one of:`n  - {0}`n  - {1}" -f (Join-Path $RootDir "settings.csv"), (Join-Path $RootDir "config\settings.csv"))
        }

        Ensure-NvsGenerator
        New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
        Remove-Item -Path $BuildSettings -Force -ErrorAction SilentlyContinue

        Write-Host "Generating fresh settings.bin from: $settingsCsv"
        & $PioPython -m esp_idf_nvs_partition_gen generate $settingsCsv $BuildSettings $script:NvsSize
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to generate settings.bin from $settingsCsv"
        }
        Require-File -Path $BuildSettings
    }

    function Resolve-PortOrFail {
        $port = Resolve-UploadPort
        if ([string]::IsNullOrWhiteSpace($port)) {
            throw ("Upload port was not detected. Set one of the following and retry:`n  1) `$env:UPLOAD_PORT=COM5`n  2) echo COM5 > {0}`nYou can inspect ports with: {1} device list" -f $UploadPortFile, $PioExe)
        }

        New-Item -ItemType Directory -Force -Path $LocalDir | Out-Null
        Set-Content -Path $UploadPortFile -Value $port -NoNewline -Encoding Ascii
        return $port
    }

    function Build-UploadAndCache {
        Write-Host "Firmware cache is missing, incomplete, or outdated. Building from sources..."
        Invoke-Pio -Args @("run", "-e", $EnvName, "-t", "fullclean")

        if (Test-Path "sdkconfig") {
            Remove-Item "sdkconfig" -Force
        }

        try {
            Invoke-Pio -Args @("run", "-e", $EnvName)
        }
        catch {
            Write-Error "Build error: firmware was not uploaded."
            throw
        }

        Sync-FirmwareCacheFromBuild
        Write-Host "Firmware cache updated in: $FirmwareDir"

        $uploadPort = Resolve-PortOrFail
        $script:LastUploadPort = $uploadPort
        Write-Host "Using upload port: $uploadPort"
        Invoke-Pio -Args @("run", "-e", $EnvName, "-t", "upload", "--upload-port", $uploadPort)
        Write-Host "Source build upload completed."
    }

    function Upload-CachedFirmware {
        Build-FreshSettingsImage
        Ensure-Esptool

        $uploadPort = Resolve-PortOrFail
        $script:LastUploadPort = $uploadPort
        Write-Host "Using upload port: $uploadPort"

        $args = @(
            $EsptoolPy
            "--chip"
            $Mcu
            "--port"
            $uploadPort
            "--baud"
            $UploadBaud
            "--before"
            "default_reset"
            "--after"
            "hard_reset"
            "write_flash"
            "--flash_mode"
            $FlashMode
            "--flash_freq"
            $FlashFreq
            "--flash_size"
            $FlashSize
            $BootloaderOffset
            $FirmwareBootloader
            $PartitionTableOffset
            $FirmwarePartitions
            $script:AppOffset
            $FirmwareApp
            $script:ModelOffset
            $FirmwareSrmodels
            $script:NvsOffset
            $BuildSettings
        )

        Write-Host "Including fresh settings.bin generated from current settings."
        & $PioPython @args
        if ($LASTEXITCODE -ne 0) {
            throw "Cached firmware upload failed."
        }
        Write-Host "Cached firmware upload completed without rebuild."
    }

    function Start-SerialMonitor {
        param(
            [Parameter(Mandatory = $true)][string]$MonitorPort
        )

        $monitorBaud = Resolve-MonitorBaud
        $monitorProjectDir = Join-Path $LocalDir "pio-monitor"
        New-Item -ItemType Directory -Force -Path $monitorProjectDir | Out-Null

        Write-Host "Starting serial monitor on: $MonitorPort (baud: $monitorBaud)"
        & $PioExe device monitor -d $monitorProjectDir --port $MonitorPort --baud $monitorBaud
        if ($LASTEXITCODE -ne 0) {
            throw "Serial monitor exited with code $LASTEXITCODE."
        }
    }

    Ensure-Windows
    Ensure-LocalPlatformIO
    $env:PLATFORMIO_CORE_DIR = Join-Path $RootDir ".pio_core"
    $ProjectConfigPath = Resolve-ProjectConfigPath
    Sync-WakeWordModelWithSettings

    if ((Cache-IsReady) -and (Cache-WakeWordModelMatchesSettings)) {
        Write-Host "Found ready firmware cache in $FirmwareDir. Rebuilding settings and uploading..."
        Upload-CachedFirmware
    }
    else {
        Build-UploadAndCache
    }

    if ([string]::IsNullOrWhiteSpace($script:LastUploadPort)) {
        throw "Upload completed, but upload port is unknown. Monitor was not started."
    }

    Start-SerialMonitor -MonitorPort $script:LastUploadPort
}
finally {
    Pop-Location
}
