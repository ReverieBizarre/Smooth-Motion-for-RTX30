@echo off
setlocal
rem ===================================================================
rem  sm86_smooth build
rem  Requires: Visual Studio 2022 (Desktop C++), Windows 10/11 SDK,
rem            CMake >= 3.20
rem ===================================================================

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
  echo [!] vcvars64.bat not found - edit VCVARS in build.bat
  exit /b 1
)
call %VCVARS% >nul
if errorlevel 1 exit /b 1

if /i "%1"=="clean" (
  if exist build rmdir /s /q build
  echo cleaned.
  exit /b 0
)

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1

echo.
echo build ok.
echo   proxy dll  : build\Release\version.dll
echo   live test  : build\Release\nvp_live_test.exe
echo   benchmark  : build\Release\nvp_perf_bench.exe
echo   vfi test   : build\Release\vfi_selftest.exe
endlocal
