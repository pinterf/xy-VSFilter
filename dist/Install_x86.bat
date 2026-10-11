@echo off
setlocal
rem MultiSubFilter installer (x86). Run as Administrator.
rem Use only with a 32-bit player.
set HERE=%~dp0
set DLL=%HERE%MultiSubFilter.dll
if not exist "%DLL%" ( echo ERROR: MultiSubFilter.dll not found. & exit /b 1 )
regsvr32 /s "%DLL%"
if errorlevel 1 ( echo ERROR: regsvr32 failed. Run as Administrator. & exit /b 1 )
echo Registered: %DLL%
pause
