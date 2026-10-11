@echo off
setlocal
rem ===========================================================================
rem  MultiSubFilter - one-shot setup for MPC-HC (64-bit)
rem
rem  Does three things, all reversible:
rem    1. registers MultiSubFilter.dll (this folder)
rem    2. sets MPC-HC's subtitle renderer to VS Filter  (NOT the "XySubFilter"
rem       entry: MPC-HC exits when that filter is not installed)
rem    3. adds MultiSubFilter to MPC-HC's External Filters list as "Prefer"
rem
rem  Run as Administrator (registration needs it). Safe to re-run.
rem ===========================================================================

set "HERE=%~dp0"
set "DLL=%HERE%MultiSubFilter.dll"

if not exist "%DLL%" (
    echo ERROR: MultiSubFilter.dll not found next to this script.
    pause
    exit /b 1
)

echo.
echo === 1/3  Registering MultiSubFilter.dll ===
powershell -NoProfile -Command "Unblock-File -LiteralPath '%DLL%'" >nul 2>&1
regsvr32 /s "%DLL%"
if errorlevel 1 (
    echo ERROR: regsvr32 failed. Run as Administrator, or unblock the DLL
    echo        ^(right-click ^> Properties ^> Unblock^), then retry.
    pause
    exit /b 1
)
echo     registered.

echo.
echo === 2/3  Configuring MPC-HC settings ===
if not exist "%APPDATA%\MPC-HC" if not exist "%USERPROFILE%\..\..\Software\MPC-HC" (
    reg query "HKCU\Software\MPC-HC\MPC-HC" >nul 2>&1
)
reg query "HKCU\Software\MPC-HC\MPC-HC" >nul 2>&1
if errorlevel 1 (
    echo     WARNING: MPC-HC settings not found. Run MPC-HC once, then re-run
    echo              this script; registration above is already done.
) else (
    reg add "HKCU\Software\MPC-HC\MPC-HC\Settings" /v SubtitleRenderer /t REG_DWORD /d 1 /f >nul
    reg add "HKCU\Software\MPC-HC\MPC-HC\Settings" /v BlockVSFilter    /t REG_DWORD /d 0 /f >nul
    echo     SubtitleRenderer = 1 ^(VS Filter / DirectVobSub^), BlockVSFilter = 0
)

echo.
echo === 3/3  Adding to MPC-HC External Filters as "Prefer" ===
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
 "$base='HKCU:\Software\MPC-HC\MPC-HC\Filters\x64';" ^
 "$clsid='{67CDB97D-9EC5-44F0-B901-C788491156D5}';" ^
 "$ds='@device:sw:{083863F1-70DE-11D0-BD40-00A0C911CE86}\'+$clsid;" ^
 "if (-not (Test-Path $base)) { 'WARNING: MPC-HC filter list not found; add it manually in Options > External Filters.'; exit 0 }" ^
 "$slot=$null;" ^
 "for ($i=0; $i -lt 32; $i++) { $k = Join-Path $base ('{0:D4}' -f $i);" ^
 "  if (Test-Path $k) { if ((Get-ItemProperty $k -EA SilentlyContinue).CLSID -eq $clsid) { $slot=$k; break } }" ^
 "  elseif (-not $slot) { $slot=$k; } }" ^
 "if (-not $slot) { 'WARNING: no free filter slot; add it manually.'; exit 0 }" ^
 "New-Item -Path $slot -Force | Out-Null;" ^
 "New-ItemProperty -Path $slot -Name SourceType  -PropertyType DWord -Value 0 -Force | Out-Null;" ^
 "New-ItemProperty -Path $slot -Name Enabled     -PropertyType DWord -Value 1 -Force | Out-Null;" ^
 "New-ItemProperty -Path $slot -Name Name        -PropertyType String -Value 'MultiSubFilter' -Force | Out-Null;" ^
 "New-ItemProperty -Path $slot -Name CLSID       -PropertyType String -Value $clsid -Force | Out-Null;" ^
 "New-ItemProperty -Path $slot -Name DisplayName -PropertyType String -Value $ds -Force | Out-Null;" ^
 "New-ItemProperty -Path $slot -Name LoadType    -PropertyType DWord -Value 1 -Force | Out-Null;" ^
 "New-ItemProperty -Path $slot -Name Merit       -PropertyType DWord -Value 0x800002 -Force | Out-Null;" ^
 "'     added at ' + $slot + '  (LoadType=1 Prefer)'"

echo.
echo ===========================================================================
echo  Done.
echo    * Play a video; in  Play ^> Filters  confirm "MultiSubFilter" is present
echo      and does NOT read "(=> !!!)".
echo    * Configure tracks in  Filters ^> MultiSubFilter ^> Dual Subtitles
echo      (or HKCU\Software\MultiSubFilter\MultiSubFilter).
echo ===========================================================================
pause
