#!/bin/sh
# Sample Grabber - per-user install (no root needed)
#   VST3        -> ~/.vst3/Sample Grabber.vst3
#   Standalone  -> ~/.local/bin/sample-grabber
#   Menu entry  -> ~/.local/share/applications/sample-grabber.desktop (+ icon)
set -e
cd "$(dirname "$0")"

BIN_DIR="$HOME/.local/bin"
VST3_DIR="$HOME/.vst3"
APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICON_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"

mkdir -p "$BIN_DIR" "$VST3_DIR" "$APP_DIR" "$ICON_DIR"

rm -rf "$VST3_DIR/Sample Grabber.vst3"
cp -r "Sample Grabber.vst3" "$VST3_DIR/"

cp "Sample Grabber" "$BIN_DIR/sample-grabber.new"
chmod +x "$BIN_DIR/sample-grabber.new"
mv -f "$BIN_DIR/sample-grabber.new" "$BIN_DIR/sample-grabber"

cp sample-grabber.png "$ICON_DIR/sample-grabber.png"
# Absolute Exec path: ~/.local/bin is not on PATH for every desktop session
sed "s|@BIN@|$BIN_DIR/sample-grabber|" sample-grabber.desktop > "$APP_DIR/sample-grabber.desktop"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$APP_DIR" || true

echo "Installed:"
echo "  VST3        $VST3_DIR/Sample Grabber.vst3"
echo "  Standalone  $BIN_DIR/sample-grabber (also in the app menu)"
echo "Rescan plugins in your DAW if it does not show up (Ardour: Plugin Manager)."
