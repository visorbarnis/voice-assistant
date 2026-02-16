param(
    [ValidateSet("current", "all")]
    [string]$Mode = "current"
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Resolve-Path (Join-Path $ScriptDir "../..")
$OutDir = Join-Path $ProjectRoot "tools"
$AppName = "settings-configurator"

$LocalDir = Join-Path $ProjectRoot ".local"
$GoRoot = Join-Path $LocalDir "go"
$GoExe = Join-Path $GoRoot "bin\go.exe"

function Resolve-GoArch {
    $archRaw = $env:PROCESSOR_ARCHITECTURE
    if ($env:PROCESSOR_ARCHITEW6432) {
        $archRaw = $env:PROCESSOR_ARCHITEW6432
    }
    $archRaw = $archRaw.ToUpperInvariant()

    switch ($archRaw) {
        "AMD64" { return "amd64" }
        "X64" { return "amd64" }
        "ARM64" { return "arm64" }
        default { throw "Unsupported architecture for local Go bootstrap: $archRaw" }
    }
}

function Resolve-GoVersion {
    if ($env:GO_VERSION) {
        return $env:GO_VERSION
    }

    try {
        $text = (Invoke-WebRequest -UseBasicParsing -Uri "https://go.dev/VERSION?m=text" -TimeoutSec 20).Content
        $line = ($text -split "`n" | Select-Object -First 1).Trim()
        if ($line -match "^go[0-9]") {
            return $line
        }
    }
    catch {
    }

    return "go1.24.0"
}

function Ensure-Go {
    if (Test-Path $GoExe) {
        return
    }

    $arch = Resolve-GoArch
    $version = Resolve-GoVersion
    $url = "https://go.dev/dl/{0}.windows-{1}.zip" -f $version, $arch

    Write-Host "Installing local Go toolchain: $version (windows/$arch)"

    New-Item -ItemType Directory -Force -Path $LocalDir | Out-Null
    if (Test-Path $GoRoot) {
        Remove-Item -Recurse -Force $GoRoot
    }

    $tmpZip = Join-Path ([System.IO.Path]::GetTempPath()) ("go-{0}-{1}.zip" -f $version, $arch)
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $tmpZip
    Expand-Archive -Path $tmpZip -DestinationPath $LocalDir -Force
    Remove-Item -Force $tmpZip

    if (-not (Test-Path $GoExe)) {
        throw "Local Go installation failed: $GoExe not found"
    }
}

Push-Location $ScriptDir
try {
    Ensure-Go

    $env:CGO_ENABLED = "0"
    & $GoExe mod tidy | Out-Null

    if ($Mode -eq "current") {
        $goos = (& $GoExe env GOOS).Trim()
        $ext = if ($goos -eq "windows") { ".exe" } else { "" }
        $out = Join-Path $OutDir ($AppName + $ext)
        $env:CGO_ENABLED = "0"
        & $GoExe build -trimpath -ldflags "-s -w" -o $out .
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed"
        }
        Write-Host "Built: $out"
        exit 0
    }

    $targets = @(
        @{ GOOS = "darwin"; GOARCH = "amd64" },
        @{ GOOS = "darwin"; GOARCH = "arm64" },
        @{ GOOS = "linux"; GOARCH = "amd64" },
        @{ GOOS = "linux"; GOARCH = "arm64" },
        @{ GOOS = "windows"; GOARCH = "amd64" },
        @{ GOOS = "windows"; GOARCH = "arm64" }
    )

    foreach ($t in $targets) {
        $ext = if ($t.GOOS -eq "windows") { ".exe" } else { "" }
        $out = Join-Path $OutDir ("{0}-{1}-{2}{3}" -f $AppName, $t.GOOS, $t.GOARCH, $ext)

        Write-Host "Building $($t.GOOS)/$($t.GOARCH) ..."
        $env:CGO_ENABLED = "0"
        $env:GOOS = $t.GOOS
        $env:GOARCH = $t.GOARCH
        & $GoExe build -trimpath -ldflags "-s -w" -o $out .
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed for $($t.GOOS)/$($t.GOARCH)"
        }
        Write-Host "  OK: $out"
    }
}
finally {
    Remove-Item Env:GOOS -ErrorAction SilentlyContinue
    Remove-Item Env:GOARCH -ErrorAction SilentlyContinue
    Remove-Item Env:CGO_ENABLED -ErrorAction SilentlyContinue
    Pop-Location
}
