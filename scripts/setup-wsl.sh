#!/usr/bin/env bash
# One-time setup in WSL (Ubuntu 22.04): build tools, JUCE dependencies, ffmpeg, yt-dlp
set -euo pipefail

sudo apt update
sudo apt install -y build-essential cmake ninja-build git pkg-config ffmpeg \
  libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev \
  libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev

mkdir -p "$HOME/.local/bin"
# Standalone build (bundles its own Python + impersonation; the pure-Python variant lacks it -> TikTok fails)
curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux -o "$HOME/.local/bin/yt-dlp"
chmod +x "$HOME/.local/bin/yt-dlp"

echo "OK. If needed: export PATH=\"\$HOME/.local/bin:\$PATH\""
yt-dlp --version
echo "--- impersonation targets (TikTok needs an entry here, e.g. chrome) ---"
yt-dlp --list-impersonate-targets || true
