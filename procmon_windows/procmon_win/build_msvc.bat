@echo off
REM ============================================================
REM  build_msvc.bat  -  Build with MSVC (Visual Studio)
REM  Run from a "Developer Command Prompt for VS"
REM ============================================================

set SRC=procmon.cpp
set OUT=procmon.exe
set LIBS=pdh.lib psapi.lib ntdll.lib user32.lib kernel32.lib

echo [*] Building %OUT% with MSVC...

cl /MT /std:c++17 /O2 /EHsc /W3 ^
   /D_UNICODE /DUNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   %SRC% /Fe:%OUT% /link %LIBS%

if %ERRORLEVEL% == 0 (
    echo.
    echo [OK] Build successful: %OUT%
    echo.
    echo      Run:   procmon.exe
    echo.
) else (
    echo.
    echo [FAIL] Build failed. Check error messages above.
)
