@echo off
rem Copies the package to a USB stick as firmware.pkg and checks the copy.
setlocal enabledelayedexpansion
cd /d "%~dp0"

set MODE=install
if /i "%~1"=="/remove" set MODE=remove
set PKG=
if "%MODE%"=="remove" (
    for %%f in (dist\X45-REMOVE-*.pkg) do set PKG=%%f
) else (
    for %%f in (dist\X45-CA-*.pkg) do set PKG=%%f
)
if "%PKG%"=="" (
    echo No package found in the dist folder. Run make-pkg.bat first.
    pause
    exit /b 1
)
echo Package: %PKG%
echo.
echo Drives:
powershell -NoProfile -Command "Get-CimInstance Win32_LogicalDisk | Select-Object DeviceID,VolumeName,@{n='SizeGB';e={[math]::Round($_.Size/1GB,1)}} | Format-Table -AutoSize"
echo.
set /p DRIVE=Letter of your USB stick, for example E: 
if "%DRIVE%"=="" exit /b 1
if not exist "%DRIVE%\" (
    echo Drive %DRIVE% does not exist.
    pause
    exit /b 1
)
if exist "%DRIVE%\firmware.pkg" (
    set /p A=%DRIVE%\firmware.pkg exists. Replace it? [y/N] 
    if /i not "!A!"=="y" exit /b 1
)
echo Copying ...
copy /y "%PKG%" "%DRIVE%\firmware.pkg" >nul
if errorlevel 1 (
    echo Copying failed.
    pause
    exit /b 1
)
for /f %%h in ('powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 '%PKG%').Hash"') do set H1=%%h
for /f %%h in ('powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 '%DRIVE%\firmware.pkg').Hash"') do set H2=%%h
if /i not "%H1%"=="%H2%" (
    del "%DRIVE%\firmware.pkg"
    echo The copy differs from the original. The stick may be damaged. I removed the bad file. Try another stick.
    pause
    exit /b 1
)
echo Copy verified.
echo.
if "%MODE%"=="remove" (
    echo Now on the unit:
    echo   1. Eject the stick in Windows, then plug it into the USB port of the unit.
    echo   2. Open the unit's normal firmware update ^(Setup menu^), pick firmware.pkg from the stick and confirm.
    echo   3. Wait until the unit has restarted by itself. It starts the original software again.
    echo.
    echo This removes the new software from the unit. Your settings stay, so installing again picks them up.
    pause
    exit /b 0
)
echo Now on the unit:
echo   1. Eject the stick in Windows, then plug it into the USB port of the unit.
echo   2. Open the unit's normal firmware update: the Setup menu, then the firmware update entry.
echo      Pick firmware.pkg from the stick and confirm.
echo   3. Wait until the unit has restarted by itself. The first start of the new software takes a little longer.
echo.
echo If the unit does not offer a firmware update, hold the MENU key on the unit while switching the power on, with
echo the stick plugged in. To go back to the original software run remove.bat and use that package the same way.
pause
