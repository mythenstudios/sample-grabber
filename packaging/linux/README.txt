Sample Grabber for Linux (x86_64)
=================================

Link or file -> audio -> trim -> 16 slices/pads -> drag WAVs into your DAW.
VST3 plugin and standalone app. Source code: https://github.com/mythenstudios/sample-grabber
License: AGPL-3.0-or-later (see LICENSE).

Requirements
------------
- x86_64 Linux with glibc 2.35 or newer (Ubuntu 22.04+, Debian 12+, Fedora 36+, ...)
- X11 or Wayland with XWayland, ALSA (PipeWire/PulseAudio work through ALSA)
- For INSTALL TOOLS (downloads yt-dlp, deno, ffmpeg): curl, tar, xz

Install (per user, no root)
---------------------------
    ./install.sh

  VST3        ~/.vst3/Sample Grabber.vst3
  Standalone  ~/.local/bin/sample-grabber, plus an entry in the app menu

Then rescan plugins in your DAW if needed (Ardour: Window > Plugin Manager > Scan).
On first start open the gear menu and press INSTALL TOOLS, or use yt-dlp/ffmpeg from your PATH.

Uninstall
---------
    ./uninstall.sh            keeps settings, tools, cache and your exports
    ./uninstall.sh --purge    also deletes ~/.config/SampleGrabber

Where things go
---------------
- Exports (dragged pads):  ~/Music/Sample Grabber/<title>/
- Tools, cache, settings:  ~/.config/SampleGrabber/

DAW notes
---------
- Reaper: dragged files stay in ~/Music/Sample Grabber. To keep projects self-contained, enable copying
  imported media into the project folder (Preferences > Media), or use "Save as" with "Copy all media".
- Bitwig: dropped clips are time-stretched to the project tempo by default. Set the clip's stretch mode
  to Raw to hear the sample unchanged. Dropping a file ONTO the plugin window does not work in Bitwig
  (Bitwig keeps the drop); paste the file path into the link field instead (Ctrl+V).
- Ardour: plugin appears after a scan in the Plugin Manager.

Legal
-----
Only grab content you have the rights to use. yt-dlp, deno and ffmpeg are separate programs under their
own licenses; INSTALL TOOLS downloads them from their official releases, they are not bundled here.
