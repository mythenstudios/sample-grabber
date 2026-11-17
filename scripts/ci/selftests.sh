#!/usr/bin/env bash
# Sample Grabber - https://github.com/mythenstudios/sample-grabber
# Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
# SPDX-License-Identifier: AGPL-3.0-or-later

# Runs the app's built-in self tests in CI on Linux (bash twin of selftests.ps1). All app folders, HOME and TMPDIR
# point into a scratch directory. Needs a display for the snapshots: run it under xvfb-run.
# Usage: selftests.sh --app <app executable> --work <scratch dir> [--only name,name] [--skip name,name]
# Tests: range cache state export settings tools grab-file grab-range grab-cancel snapshot-dark snapshot-classic
# "tools" needs the network (GitHub releases) and installs yt-dlp/deno/ffmpeg into <work>/tools; the grab tests use them.
# Exit code 0 = all selected tests passed. Logs and snapshot PNGs end up in <work>/out.
set -uo pipefail

APP="" WORK="" ONLY="" SKIP=""
while [ $# -gt 0 ]; do
    case "$1" in
        --app)  APP="$2"; shift 2 ;;
        --work) WORK="$2"; shift 2 ;;
        --only) ONLY="$2"; shift 2 ;;
        --skip) SKIP="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -n "$APP" ] && [ -n "$WORK" ] || { echo "usage: selftests.sh --app <exe> --work <dir> [--only a,b] [--skip a,b]" >&2; exit 2; }
[ -x "$APP" ] || { echo "app not found or not executable: $APP" >&2; exit 2; }

HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
OUT="$WORK/out"
LOGS="$OUT/logs"
TOOLS="$WORK/tools"
mkdir -p "$LOGS"

in_list() { case ",$2," in *",$1,"*) return 0 ;; esac; return 1; }
selected() {
    if [ -n "$ONLY" ] && ! in_list "$1" "$ONLY"; then return 1; fi
    if [ -n "$SKIP" ] && in_list "$1" "$SKIP"; then return 1; fi
    return 0
}

WAV="$WORK/loop124.wav"
python3 "$HERE/make-test-wav.py" "$WAV" 20 || { echo "could not create the test wav" >&2; exit 1; }

RESULTS=""
FAILED=0
CHECK_PNG=""   # set before a snapshot test: the PNG that must exist afterwards

# run_test <name> <timeout seconds> [VAR=value ...]
run_test() {
    local name="$1" secs="$2" base code result note="" t0
    shift 2
    if ! selected "$name"; then echo "skipped: $name"; return 0; fi
    base="$WORK/$name"
    mkdir -p "$base/home" "$base/tmp"
    echo "::group::$name"
    t0=$SECONDS
    env HOME="$base/home" TMPDIR="$base/tmp" \
        SG_CACHE_DIR="$base/cache" SG_EXPORT_DIR="$base/export" SG_TOOLS_DIR="$TOOLS" "$@" \
        timeout -k 10 "$secs" "$APP" > "$LOGS/$name.stdout.log" 2> "$LOGS/$name.log"
    code=$?
    cat "$LOGS/$name.log"
    echo "::endgroup::"

    result=ok
    if [ "$code" = 124 ] || [ "$code" = 137 ]; then result=FAIL; note="timeout after ${secs}s"
    elif [ "$code" != 0 ]; then result=FAIL
    elif [ -n "$CHECK_PNG" ]; then
        if [ ! -f "$CHECK_PNG" ] || [ "$(wc -c < "$CHECK_PNG")" -lt 10000 ]; then result=FAIL; note="no or empty $(basename "$CHECK_PNG")"; fi
    fi
    [ "$result" = ok ] || FAILED=$((FAILED + 1))
    RESULTS="$RESULTS$(printf '%-18s %-5s exit=%-4s %5ss  %s' "$name" "$result" "$code" "$((SECONDS - t0))" "$note")
"
    return 0
}

run_test range    120 SG_RANGE_SELFTEST=1
run_test cache    120 SG_CACHE_SELFTEST=1
run_test state    120 SG_STATE_SELFTEST="$WAV" SG_SELFTEST_TMP="$WORK/state/tmp"
run_test export   120 SG_EXPORT_SELFTEST="$WAV"
# settings.xml lives under HOME (scratch); also checks the export folder priority env > stored > default
run_test settings 120 SG_SETTINGS_SELFTEST=1

# Needs the network (GitHub releases): installs yt-dlp/deno/ffmpeg into $TOOLS for the grab tests
run_test tools    900 SG_TOOLS_SELFTEST=1

# Local files only: video sites block CI addresses. ffmpeg cuts/converts the synthetic loop.
run_test grab-file  300 SG_GRAB_SELFTEST="$WAV"
run_test grab-range 300 SG_GRAB_SELFTEST="$WAV" SG_GRAB_RANGE="0:02-0:08"

# Cancel while ffmpeg cuts a long file: no process of the job and no partial file may survive
if selected grab-cancel; then
    LONG="$WORK/long120.wav"
    python3 "$HERE/make-test-wav.py" "$LONG" 120 || { echo "could not create the long test wav" >&2; exit 1; }
    run_test grab-cancel 120 SG_GRAB_SELFTEST="$LONG" SG_GRAB_RANGE="0:05-1:55" SG_GRAB_CANCEL_AFTER=40
fi

for theme in dark classic; do
    CHECK_PNG="$OUT/snapshots/$theme.png"
    run_test "snapshot-$theme" 60 SG_SNAPSHOT="$OUT/snapshots" SG_THEME="$theme" SG_SNAPSHOT_FILE="$WAV"
done
CHECK_PNG=""

echo
printf '%s' "$RESULTS"
if [ "$FAILED" -gt 0 ]; then echo "$FAILED self test(s) failed"; exit 1; fi
echo "all self tests passed"
exit 0
