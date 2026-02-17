@echo off
setlocal

set "ROOT=%~dp0"
set "PS1=%ROOT%run_upload.ps1"
set "POWERSHELL_EXE="

if not exist "%PS1%" (
  echo Script not found: %PS1%
  endlocal & exit /b 1
)

if defined PROCESSOR_ARCHITEW6432 (
  if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" (
    set "POWERSHELL_EXE=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  )
)
if not defined POWERSHELL_EXE (
  if exist "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" (
    set "POWERSHELL_EXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
  )
)
if defined POWERSHELL_EXE (
  "%POWERSHELL_EXE%" -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
  set "EC=%ERRORLEVEL%"
  endlocal & exit /b %EC%
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
