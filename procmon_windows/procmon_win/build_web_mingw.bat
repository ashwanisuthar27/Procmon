@echo off
REM ============================================================
REM  build_web_mingw.bat  -  Build WebUI Modern Frontend with MinGW
REM  Run from an MSYS2 MinGW64 terminal or standard Command Prompt
REM ============================================================

set SRC=procmon_web.cpp webui_repo\src\webui.c webui_repo\src\civetweb\civetweb.c webui_repo\src\webview\win32_wv2.cpp
set OUT=procmon_web.exe
set LIBS=-lpdh -lpsapi -lntdll -luser32 -lkernel32 -ladvapi32 -lole32 -lshell32 -lws2_32 -luuid
set INCS=-I webui_repo\include

echo [*] Compiling WebUI Dependencies...
gcc -O2 -c webui_repo\src\webui.c -o webui.o -I webui_repo\include
gcc -O2 -c webui_repo\src\civetweb\civetweb.c -o civetweb.o -DNO_SSL -DWEBUI_BUILD -DUSE_WEBSOCKET -I webui_repo\include
g++ -O2 -c webui_repo\src\webview\win32_wv2.cpp -o win32_wv2.o -DWEBUI_BUILD -DNO_SSL -I webui_repo\include

echo [*] Building %OUT% with MinGW (g++)...
g++ -std=c++17 -O2 -static -mwindows -DWEBUI_BUILD -o %OUT% procmon_web.cpp webui.o civetweb.o win32_wv2.o %INCS% %LIBS%

if "%ERRORLEVEL%" == "0" (
    echo.
    echo [OK] Build successful: %OUT%
    echo.
    echo      Run:   procmon_web.exe
    echo.
) else (
    echo.
    echo [FAIL] Build failed. Check error messages above.
)
