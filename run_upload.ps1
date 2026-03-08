param(
    [string]$EnvName = "esp32-s3-devkitc-1",
    [switch]$SkipClean,
    [switch]$SkipMonitor
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
$BuildDir = Join-Path $RootDir ".pio\build\$EnvName"
$BuildBootloader = Join-Path $BuildDir "bootloader.bin"
$BuildPartitions = Join-Path $BuildDir "partitions.bin"
$BuildApp = Join-Path $BuildDir "firmware.bin"
$BuildSrmodelsDefault = Join-Path $BuildDir "srmodels\srmodels.bin"

Push-Location $RootDir

try {
    function Ensure-Windows {
        if (($env:OS -eq "Windows_NT") -or ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT)) {
            return
        }
        throw "run_upload.ps1 is intended for Windows. Use run_upload.sh on Unix-like systems."
    }

    function Resolve-WindowsArch {
        # In emulated shells (for example x64 PowerShell on Windows ARM64),
        # PROCESSOR_ARCHITECTURE can report AMD64. Query hardware architecture first.
        try {
            $cpuArch = Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Architecture
            switch ([int]$cpuArch) {
                12 { return "arm64" }  # ARM64
                9 { return "x86_64" }  # x64
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
            # Some Miniforge releases do not publish a native Windows ARM64 installer.
            # Fall back to x86_64 installer (runs under emulation).
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

    function Invoke-PioStreaming {
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

        & $PioExe @effectiveArgs 2>&1 | Out-Host
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

    function Find-BuildSrmodels {
        $candidates = @(
            $BuildSrmodelsDefault
            (Join-Path $BuildDir "srmodels.bin")
        )

        foreach ($candidate in $candidates) {
            if (-not (Test-Path $candidate)) {
                continue
            }

            $item = Get-Item -Path $candidate
            if (-not $item.PSIsContainer -and $item.Length -gt 0) {
                return $candidate
            }
        }

        $discovered = Get-ChildItem -Path $BuildDir -Filter "srmodels.bin" -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($discovered -and $discovered.Length -gt 0) {
            return $discovered.FullName
        }

        return $null
    }

    function Ensure-BuildSrmodels {
        $srmodelsPath = Find-BuildSrmodels
        if (-not [string]::IsNullOrWhiteSpace($srmodelsPath)) {
            return $srmodelsPath
        }

        Write-Host "srmodels.bin is missing after the first build. Re-running build once now that managed components are available..."
        try {
            Invoke-PioStreaming -Args @("run", "-e", $EnvName)
        }
        catch {
            Write-Error "Build error: srmodels.bin was not generated."
            throw
        }

        $srmodelsPath = Find-BuildSrmodels
        if ([string]::IsNullOrWhiteSpace($srmodelsPath)) {
            throw "srmodels.bin is still missing after the retry build."
        }

        return $srmodelsPath
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

    function Purge-FirmwareCache {
        New-Item -ItemType Directory -Force -Path $FirmwareDir | Out-Null

        $items = Get-ChildItem -Path $FirmwareDir -Force -ErrorAction SilentlyContinue
        foreach ($item in $items) {
            if ($item.Name -eq "README.md") {
                continue
            }
            Remove-Item -Path $item.FullName -Recurse -Force -ErrorAction Stop
        }
    }

    function Sync-FirmwareCacheFromBuild {
        Require-File -Path $BuildBootloader
        Require-File -Path $BuildPartitions
        Require-File -Path $BuildApp

        $buildSrmodels = Ensure-BuildSrmodels
        Require-File -Path $buildSrmodels

        New-Item -ItemType Directory -Force -Path $FirmwareDir | Out-Null
        Copy-Item -Path $BuildBootloader -Destination $FirmwareBootloader -Force
        Copy-Item -Path $BuildPartitions -Destination $FirmwarePartitions -Force
        Copy-Item -Path $BuildApp -Destination $FirmwareApp -Force
        Copy-Item -Path $buildSrmodels -Destination $FirmwareSrmodels -Force

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
"@
        Set-Content -Path $FirmwareManifest -Value $manifest -Encoding Ascii
    }

    Ensure-Windows
    Ensure-LocalPlatformIO
    $env:PLATFORMIO_CORE_DIR = Join-Path $RootDir ".pio_core"
    $ProjectConfigPath = Resolve-ProjectConfigPath

    Write-Host "Clearing firmware cache in: $FirmwareDir"
    Purge-FirmwareCache

    if (-not $SkipClean) {
        Invoke-Pio -Args @("run", "-e", $EnvName, "-t", "fullclean")
    }

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
    Write-Host "Firmware cache refreshed from the fresh source build."

    $uploadPort = Resolve-UploadPort
    if ([string]::IsNullOrWhiteSpace($uploadPort)) {
        throw "Upload port was not detected. Set `$env:UPLOAD_PORT (e.g. COM5) or create $UploadPortFile with one port value."
    }

    New-Item -ItemType Directory -Force -Path $LocalDir | Out-Null
    Set-Content -Path $UploadPortFile -Value $uploadPort -NoNewline -Encoding Ascii
    Write-Host "Using upload port: $uploadPort"

    $uploadArgs = @("run", "-e", $EnvName, "-t", "upload", "--upload-port", $uploadPort)
    if (-not $SkipMonitor) {
        $uploadArgs += @("-t", "monitor", "--monitor-port", $uploadPort)
    }
    Invoke-Pio -Args $uploadArgs
}
finally {
    Pop-Location
}
