<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/assets/banner-dark.svg">
    <source media="(prefers-color-scheme: light)" srcset=".github/assets/banner-light.svg">
    <img alt="Sample Grabber - Stream it. Cut it. Drop it in your DAW." src=".github/assets/banner-light.svg" width="100%">
  </picture>
</p>

<p align="center">
  <b>Pull audio from a link or a file, cut exactly the part you need,<br>slice it across 16 pads and drag it straight into your DAW.</b>
</p>

<p align="center">
  <sub>VST3 &middot; Standalone &nbsp;&mdash;&nbsp; C++ / JUCE 8 &nbsp;&mdash;&nbsp; by Mythen Studios</sub>
</p>

<p align="center">
  <img src="docs/media/demo.gif" width="760" alt="Sample Grabber: pads, fades and settings">
</p>

## What it does

| | |
|---|---|
| **Grab** | Paste a YouTube, TikTok, Instagram or Twitch link, or a local file path (WAV, MP3, M4A, Opus, video ...). |
| **Trim** | Waveform with in/out handles, zoom, loop and instant playback. |
| **Slice** | Auto-slice the selection across 16 pads: **Grid**, **Beat** or **Transient**. Pads respond to clicks and MIDI notes 36-51. |
| **Analyse** | BPM and key detection on the whole track or the current selection, with tempo octave `/2` `x2`. |
| **Shape** | Gain, pitch and speed (independent, applied live), fade in/out with adjustable curve, drawn right in the waveform. |
| **Drop** | Drag a pad or the whole selection into your DAW as a WAV file. |
| **Tools** | yt-dlp, deno and ffmpeg are installed and kept up to date by the app itself - from their official GitHub releases, SHA-256 verified. No Python, no PATH setup. |

<p align="center">
  <img src="docs/media/hero-dark.png" width="49%" alt="Dark theme">
  <img src="docs/media/classic.png" width="49%" alt="Classic theme">
</p>
<p align="center"><sub>Two themes: <b>Dark</b> and <b>Classic</b>.</sub></p>

## Status

Early development. Expect rough edges and breaking changes between commits.

| Platform | State |
|---|---|
| Linux x86_64 | Builds and runs (developed on Ubuntu 22.04 / WSL2) |
| Windows x64 / ARM64 | Planned |
| macOS (Apple Silicon) | Planned |

## Build (Linux)

Requirements: CMake 3.22+, a C++17 compiler, Ninja, and the usual JUCE dependencies.
JUCE 8.0.4 and [signalsmith-stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) are fetched automatically by CMake.

```bash
sudo apt install build-essential cmake ninja-build git pkg-config curl xz-utils \
  libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev \
  libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Artefacts:

- Standalone: `build/SampleGrabberApp_artefacts/Release/Sample Grabber`
- VST3: `build/SampleGrabber_artefacts/Release/VST3/Sample Grabber.vst3`

On first start the app offers **INSTALL TOOLS** (about 220 MB download: yt-dlp, deno, ffmpeg).
They go to `~/.config/SampleGrabber/bin` and are managed from the settings (gear icon, top right).
At runtime Linux needs `curl`, `tar` and `xz`.

Developing in WSL2? `scripts/setup-wsl.sh` installs the build dependencies, `scripts/wsl-audio.sh` routes ALSA to WSLg's PulseAudio.

<p align="center">
  <img src="docs/media/settings.png" width="70%" alt="Settings: theme, tools, audio">
</p>

## Third-party software

| Component | Use | License |
|---|---|---|
| [JUCE](https://juce.com) | Framework | AGPLv3 / commercial |
| [signalsmith-stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) | Pitch / time stretching | MIT |
| [Inter](https://rsms.me/inter/), [JetBrains Mono](https://www.jetbrains.com/lp/mono/) | UI fonts (`resources/fonts`) | SIL OFL 1.1 |
| [yt-dlp](https://github.com/yt-dlp/yt-dlp) | Downloading (separate process, downloaded at runtime) | Unlicense; release binaries include GPLv3+ code |
| [deno](https://github.com/denoland/deno) | JavaScript runtime for yt-dlp (YouTube) | MIT |
| [FFmpeg](https://ffmpeg.org) ([BtbN builds](https://github.com/BtbN/FFmpeg-Builds), LGPL variant) | Decoding / conversion (separate process) | LGPL 2.1+ |

The external tools are never bundled with Sample Grabber. They are downloaded by the user's machine from their official releases and run as separate programs.

## Responsible use

Sample Grabber is a tool. You are responsible for having the rights to the audio you download, sample and release, and for following each platform's terms of service.

## License

All rights reserved. This repository is private; licensing will be decided before any public release.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset=".github/assets/divider-dark.svg">
  <img alt="" src=".github/assets/divider-light.svg" width="100%">
</picture>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="brand/mark/mark-dark.svg">
    <img alt="Sample Grabber mark" src="brand/mark/mark-light.svg" width="56">
  </picture>
  <br>
  <sub>Mythen Studios &mdash; made in Switzerland</sub>
</p>
