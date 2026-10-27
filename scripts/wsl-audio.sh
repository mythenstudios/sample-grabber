#!/usr/bin/env bash
# Sound in WSL2: JUCE uses ALSA, but WSLg only provides PulseAudio.
# This script bridges ALSA -> PulseAudio so the standalone app can output sound.
# Development in WSL only - on Windows audio works without these steps.
set -euo pipefail

sudo apt install -y libasound2-plugins pulseaudio-utils alsa-utils

cat > "$HOME/.asoundrc" <<'EOF'
pcm.!default { type pulse }
ctl.!default { type pulse }
EOF

echo
echo "PULSE_SERVER = ${PULSE_SERVER:-<not set>}"
if pactl info >/dev/null 2>&1; then
  echo "PulseAudio connection: OK"
  echo "Test (noise on both channels, Ctrl+C to stop):  speaker-test -t pink -c 2"
else
  echo "No PulseAudio connection. Check: ls /mnt/wslg/PulseServer"
  echo "If present:  export PULSE_SERVER=unix:/mnt/wslg/PulseServer"
  echo "Otherwise in PowerShell:  wsl --update   then   wsl --shutdown"
fi
