@echo off
setlocal
rem MultiSubFilter installer (x64). Run as Administrator.
set HERE=%~dp0
set DLL=%HERE%MultiSubFilter.dll

if not exist "%DLL%" (
  echo ERROR: MultiSubFilter.dll not found next to this script.
  exit /b 1
)

echo Registering: %DLL%
regsvr32 /s "%DLL%"
if errorlevel 1 (
  echo ERROR: regsvr32 failed. Are you running as Administrator?
  exit /b 1
)

echo.
echo Registered successfully.
echo.
echo This filter has its own identity (MultiSubFilter) and does NOT replace
echo XySubFilter: both can be registered at the same time and chosen per player.
echo.
echo MPC-HC setup:
echo   1. Options ^> External Filters ^> Add Filter ^> select MultiSubFilter.dll,
echo      then set it to "Prefer".
echo   2. Play a video. Open Play ^> Filters ^> MultiSubFilter to reach the
echo      "Dual Subtitles" page.
echo   3. Enable dual subtitles and choose a secondary: an external file, or an
echo      embedded stream (set "secondary_embedded_pid" to the stream's PID for
echo      TS/BDMV content).
echo.
echo Settings live under HKCU\Software\MultiSubFilter\MultiSubFilter
echo (separate from XySubFilter's).
echo.
pause
