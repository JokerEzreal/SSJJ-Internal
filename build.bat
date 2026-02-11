@echo off
setlocal

set "SRCDIR=C:\Program Files (x86)\Wooduan\SSJJ-4399\SSJJ-Internal"
set "BUILDDIR=%SRCDIR%\build"
set "VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community"
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat"
set "NINJA=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if not exist "%VCVARS%" (
    echo [!] vcvarsall.bat not found at: %VCVARS%
    pause
    exit /b 1
)

echo [*] Setting up MSVC...
call "%VCVARS%" x64 >nul 2>&1

if not exist "%BUILDDIR%" mkdir "%BUILDDIR%"
cd /d "%BUILDDIR%"

echo [*] CMake configure...
cmake "%SRCDIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="%NINJA%"
if errorlevel 1 (
    echo [!] CMake FAILED
    pause
    exit /b 1
)

echo [*] Building...
"%NINJA%"
if errorlevel 1 (
    echo [!] Build FAILED
    pause
    exit /b 1
)

echo [+] SUCCESS: ssjj_overlay.dll built
echo [+] Output: %BUILDDIR%\ssjj_overlay.dll
pause
