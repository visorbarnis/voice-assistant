@echo off
setlocal

set "ROOT=%~dp0"
set "PS1=%ROOT%configure_settings.ps1"

if not exist "%PS1%" (
  echo Script not found: %PS1%
  endlocal & exit /b 1
)

where powershell >nul 2>&1
if %ERRORLEVEL%==0 (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
  set "EC=%ERRORLEVEL%"
  endlocal & exit /b %EC%
)

where pwsh >nul 2>&1
if %ERRORLEVEL%==0 (
  pwsh -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
  set "EC=%ERRORLEVEL%"
  endlocal & exit /b %EC%
)

echo Neither powershell nor pwsh was found in PATH.
endlocal & exit /b 1
