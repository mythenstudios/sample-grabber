#!/usr/bin/env bash
# Build the Linux release tarball: dist/sample-grabber-<version>-<commit>-linux-x86_64.tar.gz
#   bash scripts/package-linux.sh
# Build on the oldest supported glibc (currently Ubuntu 22.04 / glibc 2.35); the result runs there and on newer systems.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=build
ART="$BUILD/SampleGrabberApp_artefacts/Release/Sample Grabber"
VST="$BUILD/SampleGrabber_artefacts/Release/VST3/Sample Grabber.vst3"

cmake --build "$BUILD" --target SampleGrabber_VST3 SampleGrabberApp

VERSION=$(sed -n 's/^project(SampleGrabber VERSION \([0-9.]*\)).*/\1/p' CMakeLists.txt)
COMMIT=$(git rev-parse --short HEAD)
[ -z "$(git status --porcelain -- Source CMakeLists.txt packaging scripts/package-linux.sh)" ] || COMMIT="$COMMIT-dirty"
ARCH=$(uname -m)
NAME="sample-grabber-$VERSION-$COMMIT-linux-$ARCH"
STAGE="dist/$NAME"

rm -rf "$STAGE" "dist/$NAME.tar.gz"
mkdir -p "$STAGE"

cp "$ART" "$STAGE/Sample Grabber"
cp -r "$VST" "$STAGE/"
strip --strip-unneeded "$STAGE/Sample Grabber" "$STAGE/Sample Grabber.vst3/Contents/$ARCH-linux/Sample Grabber.so"

cp packaging/linux/install.sh packaging/linux/uninstall.sh packaging/linux/README.txt packaging/linux/sample-grabber.desktop "$STAGE/"
chmod +x "$STAGE/install.sh" "$STAGE/uninstall.sh"
cp brand/icon/icon-256.png "$STAGE/sample-grabber.png"
cp LICENSE "$STAGE/LICENSE"
mkdir -p "$STAGE/licenses"
cp resources/fonts/*-LICENSE.txt "$STAGE/licenses/"

GLIBC=$(objdump -T "$STAGE/Sample Grabber" "$STAGE/Sample Grabber.vst3/Contents/$ARCH-linux/Sample Grabber.so" \
        | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)

tar -C dist -czf "dist/$NAME.tar.gz" "$NAME"

echo "Package: dist/$NAME.tar.gz ($(du -h "dist/$NAME.tar.gz" | cut -f1)), needs $GLIBC or newer"
