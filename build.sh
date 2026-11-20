#!/usr/bin/env sh
# Cross-compiles relais.exe from Linux or macOS with Zig (pip install ziglang).
# On Windows, use CMake with Visual Studio instead (see README).
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-python3 -m ziglang}"
mkdir -p build
$ZIG c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode -Wl,--subsystem,windows \
  -Wall -Wno-unknown-pragmas -Wno-missing-braces \
  src/main.cpp src/video.cpp src/audio.cpp src/render.cpp res/relais.rc \
  -o build/relais.exe \
  -ld3d11 -ldxgi -lmfplat -lmf -lmfreadwrite -lole32 -luuid -lcomctl32 -lgdi32 -luser32 -lshell32
echo "build/relais.exe"
