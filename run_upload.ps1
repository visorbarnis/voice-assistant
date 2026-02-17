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
        $installerName = "Miniforge3-Windows-$arch.exe"
        $url = "https://github.com/conda-forge/miniforge/releases/latest/download/$installerName"

        Write-Host "Installing local Python (Miniforge) into: $MiniforgeDir"

        New-Item -ItemType Directory -Force -Path $LocalDir | Out-Null
        if (Test-Path $MiniforgeDir) {
            Remove-Item -Recurse -Force $MiniforgeDir
        }

        $installerPath = Join-Path $LocalDir $installerName

        try {
            Download-File -Url $url -OutFile $installerPath

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
        if ($env:UPLOAD_PORT) {
            return $env:UPLOAD_PORT.Trim()
        }

        if (Test-Path $UploadPortFile) {
            $saved = (Get-Content -Path $UploadPortFile -ErrorAction SilentlyContinue | Select-Object -First 1)
            if ($saved) {
                $saved = $saved.Trim()
                if ($saved) {
                    return $saved
                }
            }
        }

        $json = & $PioExe device list --json-output 2>$null
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($json)) {
            return $null
        }

        try {
            $devices = $json | ConvertFrom-Json
        }
        catch {
            return $null
        }

        if (-not $devices) {
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

    Ensure-Windows
    Ensure-LocalPlatformIO
    $env:PLATFORMIO_CORE_DIR = Join-Path $RootDir ".pio_core"
    $ProjectConfigPath = Resolve-ProjectConfigPath

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
