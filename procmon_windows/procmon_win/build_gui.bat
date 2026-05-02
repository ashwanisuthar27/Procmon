@echo off
REM ============================================================
REM  build_gui.bat  -  Build GUI version with MinGW g++
REM  Requires: MSYS2 mingw64 in PATH
REM ============================================================

set SRC=procmon_gui.cpp
set OUT=procmon_gui.exe

echo [*] Building %OUT% with MinGW g++ (GUI / no console)...

g++ -std=c++17 -O2 -static -mwindows ^
    -DUNICODE -D_UNICODE ^
    -o %OUT% %SRC% ^
    -lpdh -lpsapi -lntdll -luser32 -lkernel32 ^
    -lgdi32 -lcomctl32 -lcomdlg32 -lshell32 -luxtheme -lshlwapi

if %ERRORLEVEL% == 0 (
    echo.
    echo [OK] Build successful: %OUT%
    echo.
    echo      Run:   procmon_gui.exe
    echo.
) else (
    echo.
    echo [FAIL] Build failed. Check errors above.
)
