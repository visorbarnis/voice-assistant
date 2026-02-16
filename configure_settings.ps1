param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ArgsForConfigurator
)

$ErrorActionPreference = "Stop"

$RootDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$ToolsDir = Join-Path $RootDir "tools"
$ConfigDir = Join-Path $ToolsDir "configuration"

# Compatible with Windows PowerShell 5.1 and PowerShell 7+
# Use environment check first, then fallback to PlatformID.
$platform = [System.Environment]::OSVersion.Platform
if (($env:OS -ne "Windows_NT") -and ($platform -ne [System.PlatformID]::Win32NT)) {
    throw "configure_settings.ps1 is intended for Windows. Use configure_settings.sh on Unix-like systems."
}

$archRaw = $env:PROCESSOR_ARCHITECTURE
if ($env:PROCESSOR_ARCHITEW6432) {
    $archRaw = $env:PROCESSOR_ARCHITEW6432
}
$archRaw = ($archRaw | ForEach-Object { $_.ToUpperInvariant() })

$os = "windows"
$arch = switch ($archRaw) {
    "AMD64" { "amd64" }
    "X64" { "amd64" }
    "ARM64" { "arm64" }
    default { throw "Unsupported architecture: $archRaw" }
}

$binTarget = Join-Path $ToolsDir ("settings-configurator-{0}-{1}.exe" -f $os, $arch)
$binFallback = Join-Path $ToolsDir "settings-configurator.exe"
$binFallbackNoExt = Join-Path $ToolsDir "settings-configurator"

if (Test-Path $binTarget) {
    $bin = $binTarget
}
elseif (Test-Path $binFallback) {
    $bin = $binFallback
}
elseif (Test-Path $binFallbackNoExt) {
    $bin = $binFallbackNoExt
}
else {
    Write-Host "Configurator binary not found. Building current platform..."
    & (Join-Path $ConfigDir "build.ps1") current

    if (Test-Path $binTarget) {
        $bin = $binTarget
    }
    elseif (Test-Path $binFallback) {
        $bin = $binFallback
    }
    elseif (Test-Path $binFallbackNoExt) {
        $bin = $binFallbackNoExt
    }
    else {
        throw "Failed to build configurator binary."
    }
}

if (-not $ArgsForConfigurator -or $ArgsForConfigurator.Count -eq 0) {
    & $bin --file (Join-Path $RootDir "settings.csv")
    exit $LASTEXITCODE
}

& $bin @ArgsForConfigurator
exit $LASTEXITCODE
