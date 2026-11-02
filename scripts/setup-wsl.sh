#!/usr/bin/env bash
# One-time setup in WSL (Ubuntu 22.04): build tools and JUCE dependencies.
# yt-dlp, deno and ffmpeg are not needed here: the app installs them itself (INSTALL TOOLS on first start).
set -euo pipefail

sudo apt update
sudo apt install -y build-essential cmake ninja-build git pkg-config curl xz-utils \
  libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev \
  libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev

echo "OK. Build with: cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build"
