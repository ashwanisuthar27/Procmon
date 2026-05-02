@echo off
REM ============================================================
REM  build_web_msvc.bat  -  Build WebUI Modern Frontend with MSVC
REM  Run from a "Developer Command Prompt for VS"
REM ============================================================

set SRC_CPP=procmon_web.cpp webui_repo\src\webview\win32_wv2.cpp
set SRC_C=webui_repo\src\webui.c webui_repo\src\civetweb\civetweb.c
set OUT=procmon_web.exe
set LIBS=pdh.lib psapi.lib ntdll.lib user32.lib kernel32.lib advapi32.lib ole32.lib shell32.lib ws2_32.lib
set INCS=/I webui_repo\include

echo [*] Building %OUT% with MSVC and WebUI...

cl /MT /std:c++17 /O2 /EHsc /W3 /DNO_SSL /DUSE_WEBSOCKET ^
   /D_UNICODE /DUNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DWEBUI_BUILD ^
   %INCS% %SRC_CPP% /Tcwebui_repo\src\webui.c /Tcwebui_repo\src\civetweb\civetweb.c /Fe:%OUT% /link %LIBS%

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
