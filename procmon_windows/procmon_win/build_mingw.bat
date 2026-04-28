@echo off
REM ============================================================
REM  build_mingw.bat  -  Build with MinGW / MSYS2 g++
REM  Requires: MSYS2 ucrt64 toolchain in PATH
REM  Install:  pacman -S mingw-w64-ucrt-x86_64-gcc
REM ============================================================

set SRC=procmon.cpp
set OUT=procmon.exe

echo [*] Building %OUT% with MinGW g++...

g++ -std=c++17 -O2 -Wall -static ^
    -D_UNICODE -DUNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX ^
    -o %OUT% %SRC% ^
    -lpdh -lpsapi -lntdll -luser32 -lkernel32

if %ERRORLEVEL% == 0 (
    echo.
    echo [OK] Build successful: %OUT%
    echo.
    echo      Run:   procmon.exe
    echo.
) else (
    echo.
    echo [FAIL] Build failed. Check messages above.
    echo        Make sure MSYS2 ucrt64 bin is in your PATH.
)
