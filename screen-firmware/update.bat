@echo off
rem Ceratek lathe: build the firmware and write it to the board on COM7, in one double-click.
rem Before running: 12 V supply on, then the USB cable plugged in.
rem Another port: change COM7 below.
setlocal
set PORT=COM7
cd /d "%~dp0"

echo.
echo ==== 1/2  Building the firmware ====
call "%~dp0lathe.bat" build
if errorlevel 1 goto failed

echo.
echo ==== 2/2  Writing it to the board on %PORT% ====
call "%~dp0lathe.bat" flash -Port %PORT%
if errorlevel 1 goto failed

echo.
echo ==== Done. Unplug the 12 V supply for 5 seconds and plug it back in. ====
echo.
pause
exit /b 0

:failed
echo.
echo ==== FAILED. Copy the last lines above and send them to Claude. ====
echo If it says COM7 does not exist: is the 12 V supply on, and is the USB cable plugged in?
echo.
pause
exit /b 1
