#!/bin/sh
# Sample Grabber - remove the per-user install.
# Settings, downloaded tools and cache (~/.config/SampleGrabber) and your exports (~/Music/Sample Grabber) are kept;
# pass --purge to also delete ~/.config/SampleGrabber.
set -e

DATA="${XDG_DATA_HOME:-$HOME/.local/share}"

rm -rf "$HOME/.vst3/Sample Grabber.vst3"
rm -f "$HOME/.local/bin/sample-grabber"
rm -f "$DATA/applications/sample-grabber.desktop"
rm -f "$DATA/icons/hicolor/256x256/apps/sample-grabber.png"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$DATA/applications" || true
echo "Note: the app menu may show a stale icon until you log out and in again (GNOME caches the list)."

if [ "${1:-}" = "--purge" ]; then
    rm -rf "$HOME/.config/SampleGrabber"
    echo "Sample Grabber is uninstalled, including settings, tools and cache. Your exports in ~/Music/Sample Grabber are kept."
else
    echo "Sample Grabber is uninstalled (plugin, app, menu entry)."
    echo "Kept on purpose: your exports in ~/Music/Sample Grabber and settings/tools/cache in ~/.config/SampleGrabber."
    echo "Optional: run './uninstall.sh --purge' to also delete ~/.config/SampleGrabber."
fi
