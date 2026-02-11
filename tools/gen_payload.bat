@echo off
echo === Generating Payload ===

set CSC=C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe
set MANAGED_DIR=C:\Program Files (x86)\Wooduan\SSJJ-4399\battle\18_64\SSJJ_BattleClient_Unity_Data\Managed

REM Unity references - need both the facade DLL and the CoreModule for MonoBehaviour etc.
set UNITY_DLL=%MANAGED_DIR%\UnityEngine.dll
set CORE_DLL=%MANAGED_DIR%\UnityEngine.CoreModule.dll

if not exist "%UNITY_DLL%" (
    echo [!] UnityEngine.dll not found at: %UNITY_DLL%
    echo [!] Please set the correct MANAGED_DIR in this script.
    pause
    exit /b 1
)

echo [*] Using managed DLLs from: %MANAGED_DIR%
echo [*] Compiling Payload.cs...
"%CSC%" /target:library /reference:"%UNITY_DLL%" /reference:"%CORE_DLL%" /out:Payload.dll ..\payload\Payload.cs
if errorlevel 1 (
    echo [!] Compilation failed.
    pause
    exit /b 1
)

echo [*] Generating payload_bytes.h...
python bin2header.py Payload.dll > ..\src\payload\payload_bytes.h
if errorlevel 1 (
    echo [!] Header generation failed.
    pause
    exit /b 1
)

echo [+] Done! payload_bytes.h has been updated.
del Payload.dll 2>nul
pause
