@echo off
rem ===========================================================================
rem  deploy_version_dll.bat - copy build\Release\version.dll into MPC-HC
rem
rem  Fails loudly if MPC-HC is running (it holds the DLL open), and keeps a
rem  timestamped backup of whatever was deployed before. Nothing is deleted.
rem ===========================================================================
setlocal
set SRC=%~dp0build\Release\version.dll
set DST=C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\VERSION.dll

if not exist "%SRC%" (
  echo [!] not built yet: "%SRC%"
  exit /b 1
)

tasklist /FI "IMAGENAME eq mpc-hc64.exe" 2>nul | find /I "mpc-hc64.exe" >nul
if not errorlevel 1 (
  echo [!] mpc-hc64.exe is running and holds VERSION.dll open.
  echo     Close the player first, then run this again.
  exit /b 2
)

for /f "tokens=1-4 delims=/: " %%a in ("%DATE% %TIME%") do set STAMP=%%a%%b%%c_%%d
set STAMP=%STAMP: =0%
set STAMP=%STAMP::=%

if exist "%DST%" (
  copy /y "%DST%" "%DST%.prev_%STAMP%" >nul
  echo [i] previous deployed DLL backed up as VERSION.dll.prev_%STAMP%
)

copy /y "%SRC%" "%DST%" >nul
if errorlevel 1 (
  echo [!] copy failed
  exit /b 3
)

echo.
echo [ok] deployed:
certutil -hashfile "%DST%" MD5 | findstr /V ":" | findstr /R "[0-9a-f]"
echo   from: %SRC%
echo   to  : %DST%
echo.
echo Knobs (set before launching the player, then restart MPC-HC):
echo   SM86_SHADOW_WAITABLE=0      revert the frame-latency contract (A/B)
echo   SM86_SHADOW_MAX_LATENCY=n  1..3, default 1
echo   SM86_SHADOW_BUFFERS=n      4..8, default 6
echo   SM86_DIAG=1                per-present CSV -^> sm86_present.csv
endlocal
