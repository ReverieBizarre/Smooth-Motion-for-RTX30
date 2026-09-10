@echo off
rem quick iteration build (no cmake): cl.exe directly after vcvars
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars failed & exit /b 1)
cd /d "%~dp0"
if not exist build mkdir build

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /W3 /MP ^
   /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /I src ^
   src\vfi.cpp tools\selftest.cpp ^
   /Fe:build\vfi_selftest.exe /Fo:build\ ^
   /link d3d12.lib d3dcompiler.lib dxgi.lib
if errorlevel 1 exit /b 1
echo built build\vfi_selftest.exe
endlocal
