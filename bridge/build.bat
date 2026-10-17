@echo off
rem ============================================================
rem  Build sl1_bridge.dll (Streamline 1.3.3 -> 2.14.16 bridge)
rem ============================================================
setlocal

set VCVARS=G:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
set SDK=C:\Users\admin\AppData\Local\Temp\opencode\sl2sdk\include
set PY=C:\Program Files\Python313\python.exe
set BRIDGE=%~dp0

call "%VCVARS%" >nul
if errorlevel 1 ( echo [build] vcvars64 failed & exit /b 1 )

pushd "%~dp0"

"%PY%" gen_def.py
if errorlevel 1 ( echo [build] gen_def failed & exit /b 1 )

cl /nologo /LD /MT /O2 /W3 /EHsc /std:c++17 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /I "%SDK%" ^
   sl1_bridge.cpp ^
   /Fe:sl1_bridge.dll ^
   /link /DEF:sl1_bridge.def /MACHINE:X64 /SUBSYSTEM:WINDOWS version.lib
if errorlevel 1 ( echo [build] FAILED & exit /b 1 )

echo [build] OK
del /q sl1_bridge.exp sl1_bridge.lib >nul 2>&1
dir /b sl1_bridge.dll
