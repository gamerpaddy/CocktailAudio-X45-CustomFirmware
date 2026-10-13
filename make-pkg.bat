@echo off
rem Builds the firmware package on Windows. The build itself runs inside WSL (Windows Subsystem for Linux).
rem Double click this file, or run it from a command prompt.
setlocal
cd /d "%~dp0"

where wsl >nul 2>nul
if errorlevel 1 (
    echo WSL is not installed. Install it once, then run this file again:
    echo   1. Open PowerShell as administrator and run:  wsl --install -d Ubuntu
    echo   2. Restart the computer when asked, and finish the Ubuntu first start-up ^(choose a user name and password^).
    pause
    exit /b 1
)

echo The build runs in WSL Ubuntu. It may ask for your Ubuntu password once to install a few programs.
echo The first run downloads roughly 1 GB, needs about 10 GB of disk space and takes 20 minutes to an hour.
echo.
wsl --cd "%cd%" -- bash -lc "./make-pkg.sh"
if errorlevel 1 (
    echo.
    echo The build stopped with an error. Read the last lines above; running this file again continues where it stopped.
    pause
    exit /b 1
)

echo.
echo The package is in the dist folder next to this file. Run install.bat to put it on a USB stick.
pause
