#!/usr/bin/env bash
# Sample Grabber - https://github.com/mythenstudios/sample-grabber
# Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

# Sample Grabber installer for macOS on Apple Silicon. Current user only, no admin rights, no sudo.
#   curl -fsSL https://github.com/mythenstudios/sample-grabber/releases/latest/download/install-mac.sh | bash
#   curl -fsSL https://github.com/mythenstudios/sample-grabber/releases/latest/download/install-mac.sh | bash -s -- uninstall
#   bash install-mac.sh                 install (or update) the release this script belongs to
#   bash install-mac.sh --zip <file>    install from a local SampleGrabber-<version>-macos-arm64.zip
#   bash install-mac.sh uninstall       remove app and plugins (settings, tools and cache stay)
#   bash install-mac.sh uninstall --all also remove settings, cache and the downloaded tools (never your exports)
# Installs to ~/Applications, ~/Library/Audio/Plug-Ins/VST3 and ~/Library/Audio/Plug-Ins/Components.
# The copy attached to a GitHub release knows its version and the SHA-256 of its zip and checks the download.
# Everything runs inside main(), called on the last line: a cut-off download does nothing.

REPO="mythenstudios/sample-grabber"
NAME="Sample Grabber"
# Filled in by the release workflow (the repository copy keeps the placeholders and only supports --zip)
SG_VERSION="@SG_VERSION@"
SG_ZIP="@SG_ZIP@"
SG_SHA256="@SG_SHA256@"
SG_TAGS="@SG_TAGS@"

APP_DIR="$HOME/Applications"
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
DATA_DIR="$HOME/Library/Application Support/SampleGrabber"
TMP_DIR=""

say() { printf '%s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
    # Not read from $0: when piped into bash there is no script file
    say "Usage: install-mac.sh [install] [--zip <file>]    install or update (current user, no admin)"
    say "       install-mac.sh uninstall [--all]            remove app + plugins; --all also settings, cache, tools"
}

# True if the release workflow has replaced the placeholders
is_baked() {
    case "$SG_SHA256" in
        *@*|"") return 1 ;;
    esac
    return 0
}

cleanup() {
    if [ -n "$TMP_DIR" ] && [ -d "$TMP_DIR" ]; then rm -rf "$TMP_DIR"; fi
}

check_system() {
    [ "$(uname -s)" = "Darwin" ] || die "this installer is for macOS. Windows and Linux: see https://github.com/$REPO/releases"
    # hw.optional.arm64 is 1 on Apple Silicon, also in a terminal running under Rosetta (where uname -m says x86_64)
    if [ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" != "1" ]; then
        die "Sample Grabber for macOS needs a Mac with Apple Silicon (M1 or newer). Intel Macs are not supported yet."
    fi
    [ "$(id -u)" != "0" ] || die "do not run this with sudo: it installs for your user only"
    local ver major
    ver="$(sw_vers -productVersion 2>/dev/null || echo 0)"
    major="${ver%%.*}"
    case "$major" in ''|*[!0-9]*) major=0 ;; esac
    [ "$major" -ge 11 ] || die "macOS 11 or newer needed (this Mac: $ver)"
    if [ "$major" -lt 12 ]; then
        say "NOTE: macOS $ver - the app runs, but the ffmpeg that INSTALL TOOLS downloads needs macOS 12 or newer."
    fi
}

warn_running() {
    # Replacing bundles under a running host works, but the host keeps the old code loaded
    local p running=""
    for p in "$NAME" "Live" "Logic Pro X" "Logic Pro" "REAPER" "Bitwig Studio" "FL Studio" "GarageBand" "Cubase" "Studio One"; do
        if pgrep -x "$p" >/dev/null 2>&1; then running="$running \"$p\""; fi
    done
    if [ -n "$running" ]; then
        say "Still running:$running"
        say "Quit them first (they keep the old version loaded), then run the installer again."
        exit 1
    fi
}

refresh_au_cache() {
    # Logic/GarageBand re-read the AU list after the registrar restarts
    killall AudioComponentRegistrar 2>/dev/null || true
}

remove_bundles() {
    local removed=0 p
    for p in "$APP_DIR/$NAME.app" "$VST3_DIR/$NAME.vst3" "$AU_DIR/$NAME.component"; do
        if [ -e "$p" ]; then rm -rf "$p"; say "removed  $p"; removed=1; fi
    done
    # System-wide copies are not ours to delete without admin rights - only report them
    for p in "/Applications/$NAME.app" "/Library/Audio/Plug-Ins/VST3/$NAME.vst3" "/Library/Audio/Plug-Ins/Components/$NAME.component"; do
        if [ -e "$p" ]; then say "NOTE: also found $p (system-wide) - remove with: sudo rm -rf \"$p\""; fi
    done
    [ "$removed" = 1 ] || say "no installed bundles found in your user folders"
    refresh_au_cache
}

sha256_of() {
    shasum -a 256 "$1" | awk '{print $1}'
}

# Downloads this release's zip to $1. Tries the final tag first, then the release candidate tag it was built for
# (a release candidate promoted to final changes its tag; the zip and its checksum stay the same).
download_zip() {
    local dest="$1" tag url
    for tag in $SG_TAGS; do
        url="https://github.com/$REPO/releases/download/$tag/$SG_ZIP"
        say "downloading $url"
        # curl sets no quarantine flag, so Gatekeeper does not block the ad-hoc signed app
        if curl -fL --proto '=https' --proto-redir '=https' --retry 3 --connect-timeout 20 --progress-bar -o "$dest" "$url"; then
            return 0
        fi
    done
    die "download failed (network? release removed?). Releases: https://github.com/$REPO/releases"
}

cmd_install() {
    local zip_arg="$1" zip sum src f
    check_system
    command -v ditto >/dev/null 2>&1 || die "ditto missing (not macOS?)"

    TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sg-install.XXXXXX")" || die "could not create a temp folder"
    trap cleanup EXIT

    if [ -n "$zip_arg" ]; then
        [ -f "$zip_arg" ] || die "zip not found: $zip_arg"
        zip="$zip_arg"
    else
        is_baked || die "this copy of install-mac.sh is not from a release. Use the command from the README, or: bash install-mac.sh --zip <file>"
        zip="$TMP_DIR/$SG_ZIP"
        download_zip "$zip"
    fi

    sum="$(sha256_of "$zip")"
    if is_baked; then
        if [ "$sum" != "$SG_SHA256" ]; then
            die "SHA-256 mismatch for $(basename "$zip"): got $sum, expected $SG_SHA256 (wrong or damaged file - nothing was installed)"
        fi
        say "SHA-256 ok ($sum)"
    else
        say "NOTE: no release checksum in this copy of the script; SHA-256 of the zip: $sum"
        say "      compare it with SHA256SUMS of the release before you trust the file."
    fi

    ditto -x -k "$zip" "$TMP_DIR/x" || die "could not unpack $(basename "$zip")"
    src=""
    for f in "$TMP_DIR"/x/*/; do
        if [ -d "$f$NAME.app" ]; then src="${f%/}"; break; fi
    done
    [ -n "$src" ] || die "app missing in the zip"
    [ -d "$src/$NAME.vst3" ] && [ -d "$src/$NAME.component" ] || die "plugins missing in the zip"

    warn_running

    # Copy the new version next to the targets first (same volume, hidden names hosts do not scan), and only
    # then replace the old one: a failed copy (disk full) leaves the previous install untouched
    mkdir -p "$APP_DIR" "$VST3_DIR" "$AU_DIR"
    rm -rf "$APP_DIR/.$NAME.app.sg-new" "$VST3_DIR/.$NAME.vst3.sg-new" "$AU_DIR/.$NAME.component.sg-new"
    if ! { ditto "$src/$NAME.app" "$APP_DIR/.$NAME.app.sg-new" \
           && ditto "$src/$NAME.vst3" "$VST3_DIR/.$NAME.vst3.sg-new" \
           && ditto "$src/$NAME.component" "$AU_DIR/.$NAME.component.sg-new"; }; then
        rm -rf "$APP_DIR/.$NAME.app.sg-new" "$VST3_DIR/.$NAME.vst3.sg-new" "$AU_DIR/.$NAME.component.sg-new"
        die "copying failed (disk full?) - the previous version was left as it was"
    fi
    say "replacing the previous version (if any)..."
    remove_bundles
    mv "$APP_DIR/.$NAME.app.sg-new" "$APP_DIR/$NAME.app"
    mv "$VST3_DIR/.$NAME.vst3.sg-new" "$VST3_DIR/$NAME.vst3"
    mv "$AU_DIR/.$NAME.component.sg-new" "$AU_DIR/$NAME.component"
    # Downloads by curl/gh carry no quarantine flag; clear it anyway in case the zip came through a browser
    xattr -dr com.apple.quarantine "$APP_DIR/$NAME.app" "$VST3_DIR/$NAME.vst3" "$AU_DIR/$NAME.component" 2>/dev/null || true
    # Ad-hoc signed (no Apple developer ID yet): only check that the copies are intact, Gatekeeper is not asked
    for f in "$APP_DIR/$NAME.app" "$VST3_DIR/$NAME.vst3" "$AU_DIR/$NAME.component"; do
        codesign --verify --deep --strict "$f" 2>/dev/null || say "WARNING: signature check failed for $f"
    done
    refresh_au_cache
    # Marker of the old test-build script, if it was used before
    rm -f "$DATA_DIR/test-build.txt"

    say ""
    if is_baked; then say "Sample Grabber $SG_VERSION installed:"; else say "Sample Grabber installed:"; fi
    say "  app   $APP_DIR/$NAME.app"
    say "  VST3  $VST3_DIR/$NAME.vst3"
    say "  AU    $AU_DIR/$NAME.component"
    say ""
    say "Next: open the app (Spotlight, or Finder > Go > Home > Applications) and press INSTALL TOOLS"
    say "(downloads yt-dlp, deno and ffmpeg from their official releases). Rescan plugins in your DAW"
    say "(Ableton: Settings > Plug-Ins > Rescan; Logic picks the AU up on start)."
    say "Remove it again: curl -fsSL https://github.com/$REPO/releases/latest/download/install-mac.sh | bash -s -- uninstall"
}

cmd_uninstall() {
    local all="$1" p
    [ "$(uname -s)" = "Darwin" ] || die "this script is for macOS"
    [ "$(id -u)" != "0" ] || die "do not run this with sudo: it only removes what was installed for your user"
    warn_running
    remove_bundles
    if [ "$all" = 1 ]; then
        for p in "$DATA_DIR" "$HOME/Library/SampleGrabber" \
                 "$HOME/Library/Saved Application State/com.mythenstudios.samplegrabberapp.savedState" \
                 "$HOME/Library/Caches/com.mythenstudios.samplegrabberapp" \
                 "$HOME/Library/HTTPStorages/com.mythenstudios.samplegrabberapp"; do
            if [ -e "$p" ]; then rm -rf "$p"; say "removed  $p"; fi
        done
        if [ -d "$HOME/Music/$NAME" ]; then say "kept your exports: $HOME/Music/$NAME (delete it yourself if you want)"; fi
    else
        say "settings, cache and downloaded tools kept in $DATA_DIR (use 'uninstall --all' to remove them)"
    fi
    say "Sample Grabber is uninstalled."
}

main() {
    set -euo pipefail
    # All install paths are built from HOME; never let an empty HOME turn them into system folders
    case "${HOME:-}" in /?*) ;; *) die "HOME is not set" ;; esac
    [ -d "$HOME" ] || die "HOME ($HOME) does not exist"
    local cmd="install" zip="" all=0
    while [ $# -gt 0 ]; do
        case "$1" in
            install)    cmd="install"; shift ;;
            uninstall)  cmd="uninstall"; shift ;;
            --zip)      [ $# -ge 2 ] || die "--zip needs a file"; zip="$2"; shift 2 ;;
            --all)      all=1; shift ;;
            -h|--help|help) usage; return 0 ;;
            *) usage; die "unknown argument: $1" ;;
        esac
    done
    if [ "$cmd" = "install" ]; then
        [ "$all" = 0 ] || die "--all only goes with uninstall"
        cmd_install "$zip"
    else
        [ -z "$zip" ] || die "--zip only goes with install"
        cmd_uninstall "$all"
    fi
}

main "$@"
