@echo off
setlocal
rem MultiSubFilter uninstaller (x64). Run as Administrator.
regsvr32 /u /s "%~dp0MultiSubFilter.dll"
echo Unregistered (if it was registered).
pause
