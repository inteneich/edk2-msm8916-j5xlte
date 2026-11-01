@echo off
setlocal EnableExtensions
rem ---------------------------------------------------------------------------
rem  Edit the WinPE BCD inside the local "sd card" folder (the files that get
rem  copied to the phone's SD card with TWRP afterwards).
rem
rem  Usage (from an *elevated* cmd - bcdedit needs admin even for a file store):
rem    bcd.cmd show       list the BCD entries (default)
rem    bcd.cmd onecpu     boot Windows on CPU0 only
rem    bcd.cmd smp        allow all CPUs again
rem    bcd.cmd rebuild    recreate the whole BCD (old one kept as BCD.bak)
rem
rem  The "sd card" folder defaults to the parent of this script's folder;
rem  pass a different one as the 2nd argument:  bcd.cmd show "D:\my sd card"
rem
rem  Copy back to the SD card with TWRP afterwards (whole efi\ folder).
rem  Every device entry is [boot]-relative, so the card's drive letter or
rem  partition layout doesn't matter.
rem ---------------------------------------------------------------------------

set ACTION=%~1
if "%ACTION%"=="" set ACTION=show

if "%~2"=="" (
  for %%I in ("%~dp0..") do set ROOT=%%~fI
) else (
  set ROOT=%~f2
)
set S=%ROOT%\efi\microsoft\boot\BCD
set PE={d6e0a3c1-5b2f-4c7e-9a41-2f8e7b3c9d10}

net session >nul 2>&1
if errorlevel 1 (
  echo This needs an elevated command prompt ^(Run as administrator^).
  exit /b 1
)

echo SD card folder: %ROOT%
echo.

if /i "%ACTION%"=="show"    goto show
if /i "%ACTION%"=="onecpu"  goto onecpu
if /i "%ACTION%"=="smp"     goto smp
if /i "%ACTION%"=="rebuild" goto rebuild
echo Unknown action "%ACTION%". Use show, onecpu, smp or rebuild.
exit /b 1

:check
for %%F in ("%ROOT%\efi\boot\bootaa64.efi" "%ROOT%\boot\boot.sdi" "%ROOT%\sources\boot.wim" "%S%") do (
  if exist %%F (echo   OK      %%~F) else (echo   MISSING %%~F)
)
echo.
exit /b 0

:show
call :check
if not exist "%S%" exit /b 1
bcdedit /store "%S%" /enum all
exit /b %errorlevel%

:onecpu
if not exist "%S%" (echo No BCD found, run "bcd.cmd rebuild" first. & exit /b 1)
bcdedit /store "%S%" /set %PE% onecpu on || exit /b 1
bcdedit /store "%S%" /enum %PE%
exit /b 0

:smp
if not exist "%S%" (echo No BCD found, run "bcd.cmd rebuild" first. & exit /b 1)
bcdedit /store "%S%" /deletevalue %PE% onecpu
bcdedit /store "%S%" /enum %PE%
exit /b 0

:rebuild
call :check
choice /m "Recreate %S%"
if errorlevel 2 exit /b 0

if exist "%S%.bak" del "%S%.bak"
if exist "%S%" ren "%S%" BCD.bak
del "%S%.LOG*" 2>nul
if not exist "%ROOT%\efi\microsoft\boot" mkdir "%ROOT%\efi\microsoft\boot"

bcdedit /createstore "%S%" || exit /b 1
bcdedit /store "%S%" /create {bootmgr} /d "Windows Boot Manager"
bcdedit /store "%S%" /set {bootmgr} device boot
bcdedit /store "%S%" /set {bootmgr} timeout 5

bcdedit /store "%S%" /create {ramdiskoptions} /d "Ramdisk Options"
bcdedit /store "%S%" /set {ramdiskoptions} ramdisksdidevice boot
bcdedit /store "%S%" /set {ramdiskoptions} ramdisksdipath \boot\boot.sdi

bcdedit /store "%S%" /create %PE% /d "Windows PE" /application osloader
bcdedit /store "%S%" /set %PE% device ramdisk=[boot]\sources\boot.wim,{ramdiskoptions}
bcdedit /store "%S%" /set %PE% osdevice ramdisk=[boot]\sources\boot.wim,{ramdiskoptions}
bcdedit /store "%S%" /set %PE% path \windows\system32\boot\winload.efi
bcdedit /store "%S%" /set %PE% systemroot \windows
bcdedit /store "%S%" /set %PE% winpe yes
bcdedit /store "%S%" /set %PE% detecthal yes
rem Pre-release ARM64 build + unsigned/ported drivers
bcdedit /store "%S%" /set %PE% testsigning yes
bcdedit /store "%S%" /set %PE% nointegritychecks yes
rem Firmware has no MP parking yet, keep Windows on CPU0
bcdedit /store "%S%" /set %PE% onecpu on

bcdedit /store "%S%" /set {bootmgr} default %PE%
bcdedit /store "%S%" /set {bootmgr} displayorder %PE%

echo.
bcdedit /store "%S%" /enum all
exit /b 0
