#!/bin/bash
# ============================================================
# build_web_linux.sh - Build Script for Procmon Web (Linux/Kali)
# ============================================================

set -e

# Output executable
OUT="procmon_web"

# Compiler
CXX=g++
CC=gcc

# Compile WebUI Dependencies
echo "[*] Compiling WebUI Dependencies..."
$CC -O2 -c webui_repo/src/webui.c -o webui.o -I webui_repo/include
$CC -O2 -DNO_SSL -DWEBUI_BUILD -DUSE_WEBSOCKET -c webui_repo/src/civetweb/civetweb.c -o civetweb.o -I webui_repo/include

# Build backend
echo "[*] Building $OUT with $CXX..."
$CXX -std=c++17 -O2 -DWEBUI_BUILD -DNO_SSL -o $OUT procmon_web.cpp webui.o civetweb.o -I webui_repo/include -lpthread -lm

if [ $? -eq 0 ]; then
    echo
    echo "[SUCCESS] Build complete! Run with: ./$OUT"
else
    echo
    echo "[FAIL] Build failed. Make sure you have build-essential installed (sudo apt install build-essential)."
fi
