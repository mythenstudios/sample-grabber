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
  <sub>by Mythen Studios</sub>
</p>

<p align="center">
  <a href="https://isocpp.org"><img src="https://img.shields.io/badge/C%2B%2B17-18181A?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++17"></a>
  <a href="https://juce.com"><img src="https://img.shields.io/badge/JUCE%208-18181A?style=for-the-badge&logo=juce&logoColor=white" alt="JUCE 8"></a>
  <a href="#install-windows"><img src="https://img.shields.io/badge/VST3-18181A?style=for-the-badge&logo=steinberg&logoColor=white" alt="VST3"></a>
  <a href="#install-windows"><img src="https://img.shields.io/badge/Standalone-18181A?style=for-the-badge" alt="Standalone"></a>
  <a href="#status"><img src="https://img.shields.io/badge/Platform-Windows%20%7C%20macOS%20%7C%20Linux-18181A?style=for-the-badge&labelColor=2A2A2C&logo=linux&logoColor=white" alt="Platform: Windows | macOS | Linux"></a>
</p>
<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-AGPL%20v3-FF3B21?style=for-the-badge&labelColor=2A2A2C" alt="License: AGPL v3"></a>
  <a href="https://github.com/mythenstudios/sample-grabber/stargazers"><img src="https://img.shields.io/github/stars/mythenstudios/sample-grabber?style=for-the-badge&color=18181A&labelColor=2A2A2C&logo=github&logoColor=white" alt="GitHub stars"></a>
  <a href="https://ko-fi.com/mythenstudios"><img src="https://img.shields.io/badge/Ko--fi-Support-18181A?style=for-the-badge&labelColor=2A2A2C&logo=kofi&logoColor=FF3B21" alt="Support on Ko-fi"></a>
</p>
<p align="center">
  <a href="https://mythenstudios.com/"><img src="https://img.shields.io/badge/mythenstudios.com-18181A?style=for-the-badge" alt="mythenstudios.com"></a>
  <a href="https://www.instagram.com/mythenstudios"><img src="https://img.shields.io/badge/Instagram-18181A?style=for-the-badge&logo=instagram&logoColor=white" alt="Instagram"></a>
  <a href="https://www.youtube.com/@mythen-studios"><img src="https://img.shields.io/badge/YouTube-18181A?style=for-the-badge&logo=youtube&logoColor=white" alt="YouTube"></a>
</p>

<p align="center">
  <img src="docs/media/demo.gif" width="760" alt="Sample Grabber: pads, fades and settings">
</p>

## What it does

| | |
|---|---|
| **Grab** | Paste a YouTube, TikTok, Instagram or Twitch link, a local file path, or drop a file onto the window (WAV, MP3, M4A, Opus, video ...). Grab only a part with a time range (`1:02-1:30`, or `t=` in the link); long downloads can be cancelled. |
| **Trim** | Waveform with in/out handles, zoom, loop and instant playback. |
| **Slice** | Auto-slice the selection across 16 pads: **Grid**, **Beat** or **Transient**. Pads respond to clicks and MIDI notes 36-51. |
| **Analyse** | BPM and key estimate on the whole track or the current selection, with tempo octave `/2` `x2`. A starting point, not a verdict - see [Accuracy](#accuracy). |
| **Shape** | Gain, pitch and speed (independent, applied live), fade in/out with adjustable curve, drawn right in the waveform. |
| **Drop** | Drag a pad or the whole selection into your DAW as a WAV file (saved under `Music/Sample Grabber`, folder changeable in the settings). |
| **Recall** | The plugin state is saved with your DAW project, including the selected audio - it still plays if the download is gone. |
| **Tools** | yt-dlp, deno and ffmpeg are installed and kept up to date by the app itself, from their official releases. No Python, no PATH setup. One click removes them again. Each download is checked against the SHA-256 checksum published in the same upstream release - that catches broken or incomplete downloads, not a compromised upstream release. On macOS the ffmpeg build must also carry its builder's Developer ID signature. |

<p align="center">
  <img src="docs/media/hero-dark.png" width="49%" alt="Dark theme">
  <img src="docs/media/classic.png" width="49%" alt="Classic theme">
</p>
<p align="center"><sub>Two themes: <b>Dark</b> and <b>Classic</b>.</sub></p>

## Accuracy

BPM, key and the **Beat** / **Transient** slicing are automatic estimates. They are right for most
straight-ahead tracks, but they will be wrong sometimes - trust your ears over the numbers:

- **Tempo** can come out at half or double speed (e.g. 87 instead of 174), especially with half-time drums,
  swing or very sparse intros. Use `/2` and `x2`, or analyse a busier part of the track by selecting it.
- **Key** is shown as the minor key and its relative major (`A MIN / C MAJ`) because both share the same notes.
  Modal music, strong key changes, atonal parts or heavy detuning can give the wrong result.
- **Live recordings** with drifting tempo, rubato or tempo changes only get an average tempo.
- **Slices** follow the detected beats or transients; check the pads and nudge the selection if a cut lands early or late.

Detection runs only on your machine and uses openly documented methods (no proprietary or GPL analysis libraries).

## Status

Early development. Expect rough edges and breaking changes between commits.

| Platform | State |
|---|---|
| Windows 10/11 x64 | Works. Installer, tested in FL Studio and Ableton Live on Windows 11 |
| Linux x86_64 | Works. Built on Ubuntu 22.04 (glibc 2.35), tested on Ubuntu 26.04 in Ardour, Reaper and Bitwig |
| macOS, Apple Silicon (M1 and newer) | Works. VST3 + Audio Unit + app, tested in Logic Pro, Ableton Live and FL Studio. Ad-hoc signed, not notarized |
| Windows ARM64, Linux ARM64, Intel Macs | Planned |

Known limitations on Linux: in Bitwig, dropping a file *onto the plugin window* does not reach the plugin
(Bitwig keeps the drop) - paste the file path into the link field instead. Dragging pads *out* works everywhere.

## Install (Windows)

Download `SampleGrabber-<version>-windows-x64-setup.exe` from the [latest release](https://github.com/mythenstudios/sample-grabber/releases/latest) and run it.

- The installer is not code-signed yet, so Windows SmartScreen warns on first start:
  **More info** -> **Run anyway**.
- **All users** puts the plugin into `C:\Program Files\Common Files\VST3`, the folder every DAW scans (asks for admin rights).
  **Only me** needs no admin rights, but you may have to add `%LOCALAPPDATA%\Programs\Common\VST3` to your DAW's plugin folders.
- **Recommended** uses the standard folders; **Advanced** lets you choose components and the plugin, export and cache folders.
  The summary page shows what goes where, and `INSTALLED-FILES.txt` next to the app lists it again.
- Uninstall via *Settings -> Apps*. It removes plugin, app, tools, cache and settings; your exported samples stay unless you choose to delete them.

Rescan plugins in your DAW if it does not show up (close the DAW while installing).
Without installer: `SampleGrabber-<version>-windows-x64.zip` holds the app and the `.vst3` folder to copy by hand.

## Install (macOS)

Apple Silicon only (M1 to M5), macOS 12 or newer recommended (macOS 11 runs the app, but the ffmpeg that
INSTALL TOOLS downloads needs 12). Open *Terminal* and paste:

```bash
curl -fsSL https://github.com/mythenstudios/sample-grabber/releases/latest/download/install-mac.sh | bash
```

The script downloads the zip of that release, checks its SHA-256 and installs for your user only (no admin password):
the app to `~/Applications`, the plugins to `~/Library/Audio/Plug-Ins/VST3` and `~/Library/Audio/Plug-Ins/Components` (AU).
Quit your DAW first, rescan plugins afterwards (Logic picks up the AU on start). Run the same command again to update.

```bash
# remove app and plugins (add --all to also delete settings, cache and the downloaded tools; exports stay)
curl -fsSL https://github.com/mythenstudios/sample-grabber/releases/latest/download/install-mac.sh | bash -s -- uninstall
```

The app is ad-hoc signed but not notarized yet (that needs a paid Apple developer account). If you download
`SampleGrabber-<version>-macos-arm64.zip` with a browser instead, Gatekeeper reports the app as "damaged" -
use the command above. To install a zip you already have: `bash install-mac.sh --zip SampleGrabber-<version>-macos-arm64.zip`.

## Install (Linux)

Download `SampleGrabber-<version>-linux-x64.tar.gz` from the [latest release](https://github.com/mythenstudios/sample-grabber/releases/latest),
unpack it and run in the unpacked folder:

```bash
./install.sh      # VST3 to ~/.vst3, app to ~/.local/bin + app menu entry, no root needed
./uninstall.sh    # removes it again (add --purge to also delete settings, tools and cache)
```

Needs x86_64 with glibc 2.35 or newer (Ubuntu 22.04+, Debian 12+, Fedora 36+). Rescan plugins in your DAW if it does not show up.
More notes in [`packaging/linux/README.txt`](packaging/linux/README.txt).

Every release also lists `SHA256SUMS` for all files (`sha256sum -c SHA256SUMS`, on macOS `shasum -a 256 -c SHA256SUMS`).

## Install tools manually

Use this if **INSTALL TOOLS** fails (blocked network, proxy, upstream changed its files) or on an unsupported system.
The app needs four programs. Download them from the official sources, rename them exactly as shown and put them into
the tools folder of your system (create the folder if it does not exist):

| System | Tools folder |
|---|---|
| Windows | `%LOCALAPPDATA%\SampleGrabber\bin` |
| macOS | `~/Library/Application Support/SampleGrabber/bin` |
| Linux | `~/.config/SampleGrabber/bin` |

| File in the tools folder | Windows x64 | macOS Apple Silicon | Linux x86_64 |
|---|---|---|---|
| `yt-dlp` (Windows `yt-dlp.exe`) | `yt-dlp.exe` | `yt-dlp_macos` -> rename to `yt-dlp` | `yt-dlp_linux` -> rename to `yt-dlp` |
| `deno` (Windows `deno.exe`) | from `deno-x86_64-pc-windows-msvc.zip` | from `deno-aarch64-apple-darwin.zip` | from `deno-x86_64-unknown-linux-gnu.zip` |
| `ffmpeg` + `ffprobe` (Windows `.exe`) | from `bin\` in `ffmpeg-n<version>-latest-win64-lgpl-<version>.zip` | `ffmpeg.zip` and `ffprobe.zip` (release, macOS arm64) | from `bin/` in `ffmpeg-n<version>-latest-linux64-lgpl-<version>.tar.xz` |

Sources: [yt-dlp releases](https://github.com/yt-dlp/yt-dlp/releases), [deno releases](https://github.com/denoland/deno/releases),
ffmpeg for Windows/Linux from [BtbN FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases) (tag `latest`),
ffmpeg for macOS from [ffmpeg.martin-riedl.de](https://ffmpeg.martin-riedl.de). Compare each file with the checksum
published next to it.

- `ffmpeg` and `ffprobe` always belong together (same build, same folder). deno is only used by yt-dlp (YouTube).
- Linux and macOS: make them executable, `chmod +x yt-dlp deno ffmpeg ffprobe` in the tools folder.
- macOS: files downloaded with a browser are quarantined. Clear the flag in the tools folder:
  `xattr -d com.apple.quarantine yt-dlp deno ffmpeg ffprobe`.
- Restart Sample Grabber. The settings then list the tools with version `UNKNOWN`. When you press **UPDATE** (or the
  app updates yt-dlp automatically), it replaces your files with the current official release.

Alternative: leave the tools folder empty and install the tools system-wide - the app then uses them from `PATH`.
macOS with [Homebrew](https://brew.sh): `brew install yt-dlp ffmpeg deno` (found in `/opt/homebrew/bin` even when
the app is started from the Finder). On Linux prefer the official yt-dlp binary: distribution packages are often too old for YouTube.

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

On first start the app offers **INSTALL TOOLS** (yt-dlp, deno, ffmpeg - about 220 MB on Linux, 385 MB on Windows).
They go to `~/.config/SampleGrabber/bin` (Windows: `%LOCALAPPDATA%\SampleGrabber\bin`) and are managed
from the settings (gear icon, top right), together with the cache and export folders.
At runtime Linux needs `curl`, `tar` and `xz`.

`bash scripts/package-linux.sh` builds the release tarball into `dist/` (configures `build/` if needed). It refuses
binaries that need a newer glibc than 2.35 or libstdc++ than `GLIBCXX_3.4.30`, so build releases on Ubuntu 22.04
(CI does this in an `ubuntu:22.04` container).

Developing in WSL2? `scripts/setup-wsl.sh` installs the build dependencies, `scripts/wsl-audio.sh` routes ALSA to WSLg's PulseAudio.

## Build (Windows)

Requirements: Visual Studio 2022 (Desktop development with C++), CMake 3.22+, Git.
Optional: [Inno Setup 6.7+](https://jrsoftware.org/isinfo.php) for the installer.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target SampleGrabber_VST3 SampleGrabberApp
```

The installer is built from `installer/SampleGrabber.iss` (see the comment at its top); CI does this on every push.

<p align="center">
  <img src="docs/media/settings.png" width="70%" alt="Settings: theme, tools, storage folders, audio">
</p>

## Releases (maintainers)

Releases come from CI (`.github/workflows/build.yml`), never from a local build. Pushing a tag
`v<version>-rc<N>` or `v<version>` (version = `project(... VERSION ...)` in `CMakeLists.txt`, otherwise the run fails at once)
builds and tests all three platforms and creates a **draft** release with exactly those files:
`SampleGrabber-<version>-windows-x64-setup.exe`, `-windows-x64.zip`, `-linux-x64.tar.gz`, `-macos-arm64.zip`,
`install-mac.sh` (with the version and the zip's SHA-256 filled in) and `SHA256SUMS`. File names carry no `-rc` suffix.

```bash
git tag v0.1.0-rc1 && git push origin v0.1.0-rc1        # build + draft pre-release (about 30 min)
gh release download v0.1.0-rc1 -D rc1                   # test these files on each system
bash rc1/install-mac.sh --zip rc1/SampleGrabber-0.1.0-macos-arm64.zip   # macOS: same checks as the curl install
# all good: promote the tested files unchanged (creates tag v0.1.0 on the same commit, no rebuild)
gh release edit v0.1.0-rc1 --tag v0.1.0 --target "$(git rev-parse 'v0.1.0-rc1^{commit}')" \
  --title "Sample Grabber 0.1.0" --prerelease=false --draft=false
```

A failed candidate: `gh release delete v0.1.0-rc1 --yes`, fix, tag `v0.1.0-rc2`. A daily canary
(`.github/workflows/canary.yml`) checks that INSTALL TOOLS still works against the current upstream releases.

## Third-party software

| Component | Use | License |
|---|---|---|
| [JUCE](https://juce.com) | Framework | AGPLv3 / commercial |
| [signalsmith-stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) | Pitch / time stretching | MIT |
| [Inter](https://rsms.me/inter/), [JetBrains Mono](https://www.jetbrains.com/lp/mono/) | UI fonts (`resources/fonts`) | SIL OFL 1.1 |
| [yt-dlp](https://github.com/yt-dlp/yt-dlp) | Downloading (separate process, downloaded at runtime) | Unlicense; release binaries include GPLv3+ code |
| [deno](https://github.com/denoland/deno) | JavaScript runtime for yt-dlp (YouTube) | MIT |
| [Inno Setup](https://jrsoftware.org/isinfo.php) | Windows installer (build tool, not part of the app) | Inno Setup License |
| [FFmpeg](https://ffmpeg.org) ([BtbN builds](https://github.com/BtbN/FFmpeg-Builds), LGPL variant) | Decoding / conversion (separate process) | LGPL 2.1+ |
| [FFmpeg](https://ffmpeg.martin-riedl.de) (Martin Riedl builds, macOS Apple Silicon: ffmpeg + ffprobe, Developer ID signed; the app verifies SHA-256 and signature Team ID) | Decoding / conversion on macOS (separate process) | GPLv3 (built with `--enable-gpl --enable-version3`) |

The external tools are never bundled with Sample Grabber. They are downloaded by the user's machine from their official releases and run as separate programs.

## Responsible use

Sample Grabber is a tool. You are responsible for having the rights to the audio you download, sample and release, and for following each platform's terms of service.

## Support

Sample Grabber is free and stays free. If it saves you time, you can buy us a coffee - it helps keep the project maintained.

<a href="https://ko-fi.com/mythenstudios">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/assets/kofi-dark.svg">
    <img alt="Support on Ko-fi" src=".github/assets/kofi-light.svg" height="40">
  </picture>
</a>

## License

Sample Grabber is free software, licensed under the [GNU Affero General Public License v3.0](LICENSE) (AGPL-3.0-or-later).

Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>

**What this means for forks and redistribution:** you may use, study, change and share Sample Grabber.
If you distribute a modified version, or offer it to users over a network, you must release its complete
source code under the same license (AGPL-3.0-or-later) and keep the copyright notices. Closed-source,
paywalled or subscription-only versions of this code are not allowed.

The names "Sample Grabber" and "Mythen Studios" and the logos are not covered by the code license.
Forks must use their own name and branding, see [TRADEMARKS.md](TRADEMARKS.md).

Third-party components keep their own licenses (see the table above).

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
