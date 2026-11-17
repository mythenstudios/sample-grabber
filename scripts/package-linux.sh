#!/usr/bin/env bash
# Build the Linux release tarball: dist/sample-grabber-<version>-<commit>-linux-x86_64.tar.gz
#   bash scripts/package-linux.sh
# Configures build/ (Ninja, Release) if it is not configured yet, so it also works on a fresh checkout (CI).
# Build on the oldest supported glibc (Ubuntu 22.04 / glibc 2.35); the result runs there and on newer systems.
# The script fails if a binary needs a newer glibc/libstdc++ than that baseline. Building on a newer distro for
# local use only: SG_PKG_ALLOW_NEWER_GLIBC=1 bash scripts/package-linux.sh (do not publish that package).
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=build
ART="$BUILD/SampleGrabberApp_artefacts/Release/Sample Grabber"
VST="$BUILD/SampleGrabber_artefacts/Release/VST3/Sample Grabber.vst3"
MAX_GLIBC=2.35        # Ubuntu 22.04
MAX_GLIBCXX=3.4.30    # GCC 12 libstdc++ as shipped with Ubuntu 22.04

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$BUILD" --target SampleGrabber_VST3 SampleGrabberApp

VERSION=$(sed -n 's/^project(SampleGrabber VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$VERSION" ] || { echo "ERROR: no version in CMakeLists.txt" >&2; exit 1; }
COMMIT=$(git rev-parse --short=7 HEAD)
[ -z "$(git status --porcelain -- Source CMakeLists.txt packaging scripts/package-linux.sh)" ] || COMMIT="$COMMIT-dirty"
ARCH=$(uname -m)
NAME="sample-grabber-$VERSION-$COMMIT-linux-$ARCH"
STAGE="dist/$NAME"
SO="$STAGE/Sample Grabber.vst3/Contents/$ARCH-linux/Sample Grabber.so"

rm -rf "$STAGE" "dist/$NAME.tar.gz"
mkdir -p "$STAGE"

cp "$ART" "$STAGE/Sample Grabber"
cp -r "$VST" "$STAGE/"
[ -f "$SO" ] || { echo "ERROR: VST3 binary missing: $SO" >&2; exit 1; }
strip --strip-unneeded "$STAGE/Sample Grabber" "$SO"

cp packaging/linux/install.sh packaging/linux/uninstall.sh packaging/linux/README.txt packaging/linux/sample-grabber.desktop "$STAGE/"
chmod +x "$STAGE/install.sh" "$STAGE/uninstall.sh" "$STAGE/Sample Grabber"
cp brand/icon/icon-256.png "$STAGE/sample-grabber.png"
cp LICENSE "$STAGE/LICENSE"
mkdir -p "$STAGE/licenses"
cp resources/fonts/*-LICENSE.txt "$STAGE/licenses/"

# Highest versioned symbol each binary needs from glibc / libstdc++ (empty if none)
maxsym() {
    objdump -T "$STAGE/Sample Grabber" "$SO" | { grep -o "$1_[0-9][0-9.]*" || true; } | sed "s/^$1_//" | sort -uV | tail -1
}
# true if version $1 is newer than $2
newer() { [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]; }

GLIBC=$(maxsym GLIBC)
GLIBCXX=$(maxsym GLIBCXX)
echo "Needs GLIBC_${GLIBC:-none} (max $MAX_GLIBC), GLIBCXX_${GLIBCXX:-none} (max $MAX_GLIBCXX)"
TOO_NEW=0
if [ -n "$GLIBC" ] && newer "$GLIBC" "$MAX_GLIBC"; then
    echo "ERROR: needs GLIBC_$GLIBC, newer than the supported baseline $MAX_GLIBC" >&2; TOO_NEW=1
fi
if [ -n "$GLIBCXX" ] && newer "$GLIBCXX" "$MAX_GLIBCXX"; then
    echo "ERROR: needs GLIBCXX_$GLIBCXX, newer than the supported baseline $MAX_GLIBCXX" >&2; TOO_NEW=1
fi
if [ "$TOO_NEW" = 1 ]; then
    if [ "${SG_PKG_ALLOW_NEWER_GLIBC:-}" = 1 ]; then
        echo "WARNING: SG_PKG_ALLOW_NEWER_GLIBC=1 - this package will not run on Ubuntu 22.04, do not publish it" >&2
    else
        echo "Build on Ubuntu 22.04 (or set SG_PKG_ALLOW_NEWER_GLIBC=1 for a local-only package)" >&2
        exit 1
    fi
fi

# Neutral owner in the archive (CI builds as root inside a container)
tar -C dist --owner=0 --group=0 --numeric-owner -czf "dist/$NAME.tar.gz" "$NAME"

echo "Package: dist/$NAME.tar.gz ($(du -h "dist/$NAME.tar.gz" | cut -f1))"
