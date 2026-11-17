#!/usr/bin/env bash
# Sample Grabber - https://github.com/mythenstudios/sample-grabber
# Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

# CI only: build, packaging and self test dependencies inside the ubuntu:22.04 container (runs as root, no sudo).
# Build part matches scripts/setup-wsl.sh; on top: binutils (strip/objdump), python3 (test wav), xvfb + xauth
# + a fallback font (snapshots), curl + xz-utils (tool installer at runtime).
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build git pkg-config ca-certificates curl xz-utils binutils \
  libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev \
  libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev \
  python3 xvfb xauth fonts-dejavu-core
