#!/usr/bin/env bash
# Sample Grabber - https://github.com/mythenstudios/sample-grabber
# Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

# Install, update and remove macOS TEST builds from GitHub Actions (no installer yet).
#   bash mac-test.sh install [branch|run-id]   latest successful build of the branch (default: main), replaces the old one
#   bash mac-test.sh status                    what is installed, which build
#   bash mac-test.sh uninstall [--all]         removes app + plugins; --all also settings, cache and tools (not your exports)
# Needs the GitHub CLI once: brew install gh && gh auth login
# Installs for the current user only (no admin): ~/Applications and ~/Library/Audio/Plug-Ins.
set -euo pipefail

REPO="mythenstudios/sample-grabber"
ARTIFACT="sample-grabber-macos-arm64"
NAME="Sample Grabber"

APP_DIR="$HOME/Applications"
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
DATA_DIR="$HOME/Library/Application Support/SampleGrabber"
MARKER="$DATA_DIR/test-build.txt"

say() { printf '%s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

warn_running() {
    # Replacing bundles under a running host works, but the host keeps the old code loaded - confusing when testing
    local p running=""
    for p in "Sample Grabber" "Live" "Logic Pro X" "Logic Pro" "REAPER" "Bitwig Studio" "FL Studio" "GarageBand" "Cubase"; do
        pgrep -x "$p" >/dev/null 2>&1 && running="$running \"$p\""
    done
    if [ -n "$running" ]; then
        say "Still running:$running"
        say "Quit them first (they keep the old version loaded), then run this again."
        exit 1
    fi
}

refresh_au_cache() {
    # Logic/GarageBand re-read the AU list after the registrar restarts
    killall -9 AudioComponentRegistrar 2>/dev/null || true
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
    [ "$removed" = 1 ] || say "no installed bundles found"
    refresh_au_cache
}

cmd_status() {
    local p
    for p in "$APP_DIR/$NAME.app" "$VST3_DIR/$NAME.vst3" "$AU_DIR/$NAME.component"; do
        if [ -e "$p" ]; then say "installed  $p"; else say "missing    $p"; fi
    done
    if [ -f "$MARKER" ]; then say ""; cat "$MARKER"; fi
    [ -d "$DATA_DIR" ] && say "" && say "data: $DATA_DIR ($(du -sh "$DATA_DIR" 2>/dev/null | cut -f1))"
    return 0
}

cmd_install() {
    command -v gh >/dev/null 2>&1 || die "GitHub CLI missing. Once: brew install gh && gh auth login"
    gh auth status >/dev/null 2>&1 || die "GitHub CLI not logged in. Once: gh auth login"

    local sel="${1:-main}" run="" sha id
    if printf '%s' "$sel" | grep -Eq '^[0-9]+$'; then
        run="$sel"
    else
        # Newest run of the branch that has a macOS package (other jobs may have failed, that is fine here)
        local ids
        ids="$(gh run list -R "$REPO" --workflow build --branch "$sel" --limit 15 --json databaseId --jq '.[].databaseId')" \
            || die "could not list builds (network? gh auth login?)"
        for id in $ids; do
            if [ -n "$(gh api "repos/$REPO/actions/runs/$id/artifacts" --jq ".artifacts[] | select(.name == \"$ARTIFACT\" and .expired == false) | .id" 2>/dev/null || true)" ]; then
                run="$id"; break
            fi
        done
        [ -n "$run" ] || die "no macOS test build found for branch '$sel' (still building? try: bash mac-test.sh install mac/port)"
    fi
    sha="$(gh run view "$run" -R "$REPO" --json headSha --jq '.headSha')" || die "could not read run $run"
    say "build: run $run, commit ${sha:0:7}"

    TMP_DL="$(mktemp -d)"
    trap 'rm -rf "$TMP_DL"' EXIT
    local tmp="$TMP_DL"
    gh run download "$run" -R "$REPO" -n "$ARTIFACT" -D "$tmp/dl" || die "download failed (artifact expired? builds are kept 14 days)"
    local zip="" src="" f
    for f in "$tmp"/dl/*.zip; do [ -f "$f" ] && zip="$f" && break; done
    [ -n "$zip" ] || die "no zip inside the artifact"
    ditto -x -k "$zip" "$tmp/x" || die "could not unpack $zip"
    for f in "$tmp"/x/*/; do [ -d "$f" ] && src="${f%/}" && break; done
    [ -n "$src" ] && [ -d "$src/$NAME.app" ] || die "app missing in the package"

    warn_running
    say "removing the previous test build..."
    remove_bundles

    mkdir -p "$APP_DIR" "$VST3_DIR" "$AU_DIR" "$DATA_DIR"
    ditto "$src/$NAME.app" "$APP_DIR/$NAME.app"
    ditto "$src/$NAME.vst3" "$VST3_DIR/$NAME.vst3"
    ditto "$src/$NAME.component" "$AU_DIR/$NAME.component"
    # Files from gh carry no quarantine flag; clear it anyway in case the zip came through a browser
    xattr -dr com.apple.quarantine "$APP_DIR/$NAME.app" "$VST3_DIR/$NAME.vst3" "$AU_DIR/$NAME.component" 2>/dev/null || true
    refresh_au_cache

    printf 'test build: run %s, commit %s, branch/selector %s, installed %s\n' "$run" "$sha" "$sel" "$(date '+%Y-%m-%d %H:%M')" > "$MARKER"
    say ""
    say "installed:"
    say "  app   $APP_DIR/$NAME.app"
    say "  VST3  $VST3_DIR/$NAME.vst3"
    say "  AU    $AU_DIR/$NAME.component"
    say "Rescan plugins in your DAW (Ableton: Preferences > Plug-Ins > Rescan; Logic picks the AU up on start)."
}

cmd_uninstall() {
    warn_running
    remove_bundles
    if [ "${1:-}" = "--all" ]; then
        local p
        for p in "$DATA_DIR" "$HOME/Library/SampleGrabber" \
                 "$HOME/Library/Saved Application State/com.mythenstudios.samplegrabberapp.savedState" \
                 "$HOME/Library/Caches/com.mythenstudios.samplegrabberapp" \
                 "$HOME/Library/HTTPStorages/com.mythenstudios.samplegrabberapp"; do
            if [ -e "$p" ]; then rm -rf "$p"; say "removed  $p"; fi
        done
        [ -d "$HOME/Music/$NAME" ] && say "kept your exports: $HOME/Music/$NAME (delete it yourself if you want)"
    else
        rm -f "$MARKER"
        say "settings, cache and tools kept in $DATA_DIR (use 'uninstall --all' to remove them)"
    fi
    return 0
}

case "${1:-}" in
    install)   shift; cmd_install "$@" ;;
    status)    cmd_status ;;
    uninstall) shift; cmd_uninstall "$@" ;;
    *) sed -n '6,11p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
