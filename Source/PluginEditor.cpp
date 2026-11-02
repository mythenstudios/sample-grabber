// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PluginEditor.h"
#include "BinaryData.h"
#include <cmath>

namespace
{
    const juce::String kDot  = juce::String::fromUTF8 (" \xc2\xb7 ");
    const juce::String kDash = juce::String::fromUTF8 ("\xe2\x80\x94");
    const char* const kSourceNames[4] = { "YOUTUBE", "TWITCH", "INSTAGRAM", "TIKTOK" };
    const char* const kKeyNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    // Directly readable (JUCE formats) or to be converted via ffmpeg
    const juce::StringArray kNativeExts  { ".wav", ".aif", ".aiff", ".flac", ".ogg" };
    const juce::StringArray kConvertExts { ".mp3", ".mp4", ".m4a", ".m4v", ".mov", ".webm", ".mkv",
                                           ".aac", ".opus", ".wma", ".avi" };

    bool isNative (const juce::File& f)  { return kNativeExts.contains (f.getFileExtension().toLowerCase()); }
    bool isConvert (const juce::File& f) { return kConvertExts.contains (f.getFileExtension().toLowerCase()); }

    // Upper case without the C runtime locale (towupper in the "C" locale may map ASCII only, e.g. on MSVC).
    // Covers Latin-1, Latin Extended-A, basic Greek and Cyrillic; sharp s (U+00DF) stays as it is. Anything else
    // goes through JUCE's towupper.
    juce::juce_wchar upperChar (juce::juce_wchar c)
    {
        if (c < 0x80)
            return c >= 'a' && c <= 'z' ? c - 0x20 : c;
        if (c >= 0xE0 && c <= 0xFE && c != 0xF7)              return c - 0x20;   // a grave .. thorn
        if (c == 0xFF)                                         return 0x178;      // y diaeresis
        if (c >= 0x100 && c <= 0x137)                          return c == 0x131 ? (juce::juce_wchar) 'I' : (c & ~(juce::juce_wchar) 1);   // pairs upper even
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
            return (c & 1) == 0 ? c - 1 : c;                                          // pairs upper odd
        if (c >= 0x14A && c <= 0x177)                          return c & ~(juce::juce_wchar) 1;
        if (c == 0x17F)                                        return 'S';        // long s
        if (c >= 0x3B1 && c <= 0x3C9)                          return c == 0x3C2 ? (juce::juce_wchar) 0x3A3 : c - 0x20;   // Greek, final sigma
        if (c >= 0x430 && c <= 0x44F)                          return c - 0x20;   // Cyrillic a .. ya
        if (c >= 0x450 && c <= 0x45F)                          return c - 0x50;   // Cyrillic ie grave .. dzhe
        if (c < 0x250)
            return c;                                                             // other Latin up to Extended-B: left as is
        return juce::CharacterFunctions::toUpperCase (c);
    }

    juce::String upperDisplay (const juce::String& s)
    {
        std::vector<juce::juce_wchar> buf;
        buf.reserve ((size_t) s.length() + 1);
        for (auto p = s.getCharPointer(); ! p.isEmpty();)
            buf.push_back (upperChar (p.getAndAdvance()));
        buf.push_back (0);
        return juce::String (juce::CharPointer_UTF32 (buf.data()));
    }

    // Range tag of a cache file name ("_r<from>-<to>") -> " (1:02:00-1:05:30)", empty without a tag
    juce::String rangeSuffix (const sg::range::Range& r)
    {
        if (! r.set)
            return {};
        return " (" + sg::range::formatTime (r.from) + "-"
               + (r.hasEnd() ? sg::range::formatTime (r.to) : juce::String ("END")) + ")";
    }

    // yt-dlp appends "_<11-char-ID>" and replaces spaces with "_"; cache files of a section carry "_r<from>-<to>"
    // -> "TITLE (1:02:00-1:05:30)". displayTitle (real title of a link) replaces the name taken from the file.
    juce::String prettyName (const juce::File& f, const juce::String& displayTitle = {})
    {
        sg::range::Range r;
        auto n = sg::storage::stripRangeTag (f, &r);
        if (sg::storage::hasVideoIdTail (n))
            n = n.substring (0, n.length() - 12);
        const auto shown = displayTitle.trim();
        return upperDisplay (shown.isNotEmpty() ? shown : n.replaceCharacter ('_', ' ')) + rangeSuffix (r);
    }

    // Length and bit depth from the header of a JUCE-readable file (cheap, no audio read)
    bool readNativeInfo (const juce::File& f, double& seconds, int& bits, int& channels)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr || r->sampleRate <= 0.0 || r->lengthInSamples <= 0)
            return false;
        seconds = (double) r->lengthInSamples / r->sampleRate;
        bits = (int) r->bitsPerSample;
        channels = (int) r->numChannels;
        return true;
    }

    juce::String bpmString (float v)
    {
        return std::abs (v - std::round (v)) < 0.25f ? juce::String ((int) std::lround (v)) : juce::String (v, 1);
    }

    // Display for LEN/SEL: under 60 s seconds with 2 decimals, from 60 s m:ss.d (like Windows/YouTube, but with tenths)
    juce::String timeString (double seconds)
    {
        if (std::llround (seconds * 100.0) < 6000)
            return juce::String (seconds, 2) + "S";

        const long long tenths = std::llround (seconds * 10.0);   // round to tenths first so "1:60.0" never appears
        const int m = (int) (tenths / 600);
        const int rest = (int) (tenths % 600);
        const int sec = rest / 10;
        return juce::String (m) + ":" + (sec < 10 ? "0" : "") + juce::String (sec) + "." + juce::String (rest % 10);
    }

    // Always m:ss.d (header), rounded to tenths before splitting
    juce::String clockString (double seconds)
    {
        const long long tenths = std::llround (juce::jmax (0.0, seconds) * 10.0);
        const int m = (int) (tenths / 600);
        const int rest = (int) (tenths % 600);
        const int sec = rest / 10;
        return juce::String (m) + ":" + (sec < 10 ? "0" : "") + juce::String (sec) + "." + juce::String (rest % 10);
    }
}

SampleGrabberEditor::SampleGrabberEditor (SampleGrabberProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      loopAttachment (p.apvts, "loop", loopButton),
      waveform (p), pads (p), settings (p),
      gainCell    (p.apvts, "gain",    "GAIN",     " DB", 1, true,  0.0),
      pitchCell   (p.apvts, "pitch",   "PITCH",    " ST", 1, true,  0.0),
      speedCell   (p.apvts, "speed",   "SPEED",    " %",  1, false, 100.0),
      fadeInCell  (p.apvts, "fadeInPct",  "FADE IN",  " %", 1, false, 0.0),
      fadeOutCell (p.apvts, "fadeOutPct", "FADE OUT", " %", 1, false, 0.0),
      shapeCell   (p.apvts, "fadeShape",  "SHAPE",    "",   0, true,  0.0)
{
    ui::loadThemeSetting();   // user's theme choice (settings.xml), before the first paint
    laf.applyTheme();         // laf was already constructed before loading
    setLookAndFeel (&laf);
    setOpaque (true);   // editor paints its background completely itself -> less repaint work

    // --- URL ---
    urlField.setReturnKeyStartsNewLine (false);
    urlField.setJustification (juce::Justification::centredLeft);
    urlField.setBorder (juce::BorderSize<int> (0));
    urlField.onReturnKey = [this] { grabOrConfirm(); };
    urlField.onEscapeKey = [this] { if (downloader.isBusy()) cancelJob ("CANCELLED", false); };
    urlField.onTextChange = [this]
    {
        const int s = detectSource (urlField.getText());
        if (s >= 0 && s != activeSource)
        {
            activeSource = s;
            repaint (tabsR);
        }
        prefillRangeFromUrl();
        onInputChanged();
    };
    addAndMakeVisible (urlField);

    // --- Range: "FROM-TO" / "FROM-", empty = whole source (digits, ':', '.', '-' and spaces only) ---
    rangeField.setReturnKeyStartsNewLine (false);
    rangeField.setJustification (juce::Justification::centredLeft);
    rangeField.setBorder (juce::BorderSize<int> (0));
    rangeField.setInputRestrictions (24, "0123456789:.- ");
    rangeField.onReturnKey = [this] { grabOrConfirm(); };
    rangeField.onEscapeKey = [this] { if (downloader.isBusy()) cancelJob ("CANCELLED", false); };
    rangeField.onTextChange = [this] { onInputChanged(); };
    rangeHover.onHover = [this] (bool in)
    {
        setHoverHint (in ? "RANGE" + kDot + "FROM-TO OR FROM-, E.G. 1:02:00-1:05:30" + kDot + "EMPTY = FULL SOURCE" : "");
    };
    rangeField.addMouseListener (&rangeHover, true);
    addAndMakeVisible (rangeField);

    fetchButton.getProperties().set ("primary", true);
    fetchButton.onClick = [this] { onFetchClicked(); };
    addAndMakeVisible (fetchButton);

    // --- Gear: settings overlay (theme, tools, audio/MIDI only in the standalone app) ---
    gearButton.onClick = [this] { showSettings (! settings.isVisible()); };
    gearButton.onHover = [this] (bool in) { setHoverHint (in ? "SETTINGS" : ""); };
    addAndMakeVisible (gearButton);

    // --- Slice mode ---
    for (auto* b : { &gridButton, &beatButton, &transientButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (1001);
        b->getProperties().set ("outline", true);
        addAndMakeVisible (*b);
    }
    gridButton.onClick = [this] { processor.setSliceMode (SampleGrabberProcessor::SliceMode::grid); waveform.repaint(); };
    transientButton.onClick = [this] { processor.setSliceMode (SampleGrabberProcessor::SliceMode::transient); waveform.repaint(); };
    beatButton.onClick = [this] { processor.setSliceMode (SampleGrabberProcessor::SliceMode::beat); waveform.repaint(); };
    syncSliceModeButtons();

    // --- BACK: return to the state before the last load (e.g. after dragging a pad back into the plugin) ---
    backButton.setEnabled (false);   // only enabled once there is a predecessor
    backButton.onClick = [this] { goBack(); };
    addAndMakeVisible (backButton);

    // --- Tempo octave (correct half/double tempo) ---
    for (auto* b : { &bpmHalfButton, &bpmDoubleButton })
    {
        b->getProperties().set ("outline", true);
        b->setEnabled (false);   // only enabled once a tempo has been detected
        addAndMakeVisible (*b);
    }
    bpmHalfButton.onClick   = [this] { processor.shiftBpmOctave (-1); repaintInfo(); waveform.repaint(); };
    bpmDoubleButton.onClick = [this] { processor.shiftBpmOctave (+1); repaintInfo(); waveform.repaint(); };

    // Editor was recreated, analysis already exists: do not start again
    if (processor.hasSample() && processor.getBpm() > 0.0f)
        lastAnalysisVersion = processor.getSelectionVersion();

    // --- Transport (icons instead of text) ---
    // Play and pause share one button: the timer sets icon and active state
    playButton.setClickingTogglesState (false);
    playButton.onClick = [this]
    {
        if (processor.getPlayheadNorm() >= 0.0f && ! processor.isPaused())
            processor.pause();
        else
            processor.triggerPad (SampleGrabberProcessor::fullRegionPad);
    };
    addAndMakeVisible (playButton);

    stopButton.onClick = [this] { processor.stop(); };
    addAndMakeVisible (stopButton);

    loopButton.setClickingTogglesState (true);
    addAndMakeVisible (loopButton);

    // Hints instead of tooltips: text appears in the footer (infoR)
    playButton.onHover = [this] (bool in)
    {
        setHoverHint (in ? (playButton.getIcon() == IconButton::Icon::pause ? "PAUSE" : "PLAY") : "");
    };
    stopButton.onHover = [this] (bool in) { setHoverHint (in ? "STOP" : ""); };
    loopButton.onHover = [this] (bool in)
    {
        setHoverHint (in ? (loopButton.getToggleState() ? "LOOP: ON" : "LOOP: OFF") : "");
    };
    backButton.onHover = [this] (bool in)
    {
        setHoverHint (in ? "BACK" + kDot + (backButton.isEnabled() ? "UNDO LAST LOAD" : "NOTHING TO UNDO") : "");
    };

    dragHandle.onDragStart = [this] { dragOut (SampleGrabberProcessor::fullRegionPad); };
    addAndMakeVisible (dragHandle);

    pads.onDragPad = [this] (int pad) { dragOut (pad); };

    waveform.onTrimChanged = [this] { repaintInfo(); };

    addAndMakeVisible (waveform);
    addAndMakeVisible (pads);
    // Gain: left stop = mute; red when selection peak x gain exceeds 0 dBFS (export clips then)
    gainCell.setFormatter ([] (double v)
    {
        return v <= (double) SampleGrabberProcessor::gainMinDb + 0.05 ? juce::String ("-INF DB") : juce::String();
    });
    addAndMakeVisible (gainCell);
    addAndMakeVisible (pitchCell);
    addAndMakeVisible (speedCell);
    addAndMakeVisible (fadeInCell);
    addAndMakeVisible (fadeOutCell);
    addAndMakeVisible (shapeCell);

    // Add the settings overlay last (on top of everything), closed at start
    settings.onClose = [this] { showSettings (false); };
    settings.onSetTheme = [this] (ui::ThemeId id) { setThemeFromSettings (id); };
    settings.isGrabBusy = [this] { return downloader.isBusy(); };
    addChildComponent (settings);

    setSize (720, 600);

    if (processor.hasSample())
    {
        waveform.resetView();
        pads.refreshThumbs();
    }

    applyTheme();

    // External tools: GRAB/INSTALL TOOLS button, hint when tools are missing, silent update check (at most once per 24 h)
    lastToolRun = processor.tools->getRunCounter();   // do not show old results again
    processor.tools->addChangeListener (this);
    processor.tools->refresh();
    syncTools();
    if (processor.tools->needsInstall() && ! busy && ! processor.hasSample())
        statusText = "TOOLS MISSING" + kDot + "CLICK INSTALL TOOLS (YT-DLP, DENO, FFMPEG ~220 MB)";
    processor.tools->requestAutoCheck();

    // State restored before the editor existed: show its note (source missing) and URL
    lastEmbedWarn = processor.getEmbedWarnVersion();
    lastStateVersion = processor.getStateVersion();
    if (processor.getRestoreNote().isNotEmpty())
        applyRestoredState();
    else
        fillSourceFields();   // editor reopened: link + range of the loaded sample (RE-GRAB fetches the same section)

    // Developer aid (SG_SNAPSHOT only): preset the status line for the image, e.g. to check long tool messages
    if (juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT", {}).isNotEmpty())
    {
        const auto snapStatus = juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_STATUS", {});
        if (snapStatus.isNotEmpty())
        {
            statusText = snapStatus.replace ("|", kDot);   // "|" = middle dot (environment variable stays ASCII)
            isError = snapStatus.contains ("FAILED");
        }

        // SG_SNAPSHOT_TOOLS_CHECK=1: start a real version check (result text in status line and overlay);
        // with SG_SNAPSHOT_FILE the snapshot waits 4 s, enough for the three queries
        if (juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_TOOLS_CHECK", {}).isNotEmpty())
            processor.tools->checkForUpdates();

        // SG_SNAPSHOT_URL / SG_SNAPSHOT_RANGE: fill the URL row (the URL goes through onTextChange, i.e. t= prefill);
        // SG_SNAPSHOT_GRABALL=1: show the long-source confirmation (button GRAB ALL)
        const auto snapUrl = juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_URL", {});
        if (snapUrl.isNotEmpty())
            urlField.setText (snapUrl, true);
        const auto snapRange = juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_RANGE", {});
        if (snapRange.isNotEmpty())
            rangeField.setText (snapRange, false);
        if (juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_GRABALL", {}).isNotEmpty())
        {
            GrabSpec spec;
            spec.isUrl = true;
            spec.url = snapUrl;
            spec.duration = 4350.0;
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SampleGrabberEditor> (this), spec]
            {
                if (safe != nullptr)   // after the URL text-change message, which would clear it again
                    safe->setConfirm (spec, "LONG SOURCE " + sg::range::formatDuration (spec.duration) + kDot
                                                + "SET A RANGE (RECOMMENDED) OR GRAB ALL");
            });
        }

        // SG_SNAPSHOT_GRAB=1: press GRAB (e.g. with SG_SNAPSHOT_URL: the image shows the running check and CANCEL)
        if (juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_GRAB", {}).isNotEmpty())
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SampleGrabberEditor> (this)]
            {
                if (safe != nullptr)
                    safe->grabOrConfirm();
            });

        // SG_SNAPSHOT_TITLE=<UTF-8>: display title for the file loaded via SG_SNAPSHOT_FILE (like the title of a link)
        snapshotTitle = juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_TITLE", {});

        // SG_SNAPSHOT_SETTINGS=1: show the settings overlay open (=cache|exports: with that folder row in CHANGE mode)
        const auto snapSettings = juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_SETTINGS", {});
        if (snapSettings.isNotEmpty())
        {
            showSettings (true);
            if (snapSettings == "cache" || snapSettings == "exports")
                settings.showFolderEditForSnapshot (snapSettings == "cache");
        }
    }

    startTimerHz (30);
}

// Theme-dependent colours of individual components (step 3 calls this after laf.applyTheme() when switching)
void SampleGrabberEditor::applyTheme()
{
    urlField.setColour (juce::TextEditor::backgroundColourId, ui::c().surface1);
    urlField.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Paste a link or file path \xe2\x80\x94 YouTube, TikTok, Twitch"),
                                     ui::c().textTertiary);
    urlField.applyFontToAllText (ui::mono (12.0f));   // also sets the font for new text
    urlField.applyColourToAllText (ui::c().textPrimary);
    rangeField.setColour (juce::TextEditor::backgroundColourId, ui::c().surface1);
    rangeField.setTextToShowWhenEmpty ("FULL", ui::c().textTertiary);
    rangeField.applyFontToAllText (ui::mono (12.0f));
    rangeField.applyColourToAllText (ui::c().textPrimary);
    repaint (urlR);
    shownTheme = ui::theme();
    rebuildLockup();
}

// Logo per theme: Dark = light text, Classic = dark text
void SampleGrabberEditor::rebuildLockup()
{
    if (ui::theme() == ui::ThemeId::dark)
        lockup = juce::Drawable::createFromImageData (BinaryData::lockuphorizontaldark_svg, (size_t) BinaryData::lockuphorizontaldark_svgSize);
    else
        lockup = juce::Drawable::createFromImageData (BinaryData::lockuphorizontallight_svg, (size_t) BinaryData::lockuphorizontallight_svgSize);
}

// Redraw detail panel and header meta (BPM, key, SEL) together
void SampleGrabberEditor::repaintInfo()
{
    repaint (detR);
    repaint (headerR);
}

void SampleGrabberEditor::setThemeFromSettings (ui::ThemeId id)
{
    if (ui::theme() == id)
        return;
    ui::setTheme (id);   // saves to settings.xml; other instances follow via timer
    refreshTheme();
}

// Open/close the overlay. The gear stays on top (click closes again), active = red.
void SampleGrabberEditor::showSettings (bool show)
{
    if (show)
    {
        settings.prepareToShow();
        settings.setVisible (true);
        settings.toFront (false);
        gearButton.toFront (false);
        if (settings.isShowing())
            settings.grabKeyboardFocus();
        else
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SettingsOverlay> (&settings)]
            {
                if (safe != nullptr && safe->isShowing())
                    safe->grabKeyboardFocus();
            });
    }
    else
    {
        settings.setVisible (false);
        if (urlField.isShowing())
            urlField.grabKeyboardFocus();
    }
    gearButton.setToggleState (show, juce::dontSendNotification);
    gearButton.setBadge (processor.tools->needsAttention());   // prepareToShow has just re-read the state
}

// Rebuild everything theme-dependent (also when another editor switched the theme)
void SampleGrabberEditor::refreshTheme()
{
    laf.applyTheme();
    applyTheme();
    waveform.themeChanged();
    settings.themeChanged();

    // Standalone: also update window frame and its LookAndFeel
    // Only touch our own app window, never the window of a (JUCE) host
    if (processor.onOpenAudioSettings != nullptr)
        if (auto* win = findParentComponentOfClass<juce::DocumentWindow>())
            if (auto* wl = dynamic_cast<SGLookAndFeel*> (&win->getLookAndFeel()))
            {
                win->setBackgroundColour (ui::c().canvas);
                wl->applyTheme();
                win->repaint();   // repaints all children (editor, audio panel, window buttons)
            }

    pads.refreshThumbs();
    repaint();
    waveform.repaint();
    pads.repaint();
    gainCell.repaint(); pitchCell.repaint(); speedCell.repaint(); fadeInCell.repaint(); fadeOutCell.repaint(); shapeCell.repaint();
    playButton.repaint(); stopButton.repaint(); loopButton.repaint(); dragHandle.repaint();
}

void SampleGrabberEditor::setHoverHint (const juce::String& t)
{
    if (hoverHint != t)
    {
        hoverHint = t;
        repaint (infoR);
    }
}

SampleGrabberEditor::~SampleGrabberEditor()
{
    processor.tools->removeChangeListener (this);
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
int SampleGrabberEditor::detectSource (const juce::String& url)
{
    const auto u = url.toLowerCase();
    if (u.contains ("youtube.") || u.contains ("youtu.be")) return 0;
    if (u.contains ("twitch.tv"))                           return 1;
    if (u.contains ("instagram.com"))                       return 2;
    if (u.contains ("tiktok.com"))                          return 3;
    return -1;
}

std::function<void (float, juce::String)> SampleGrabberEditor::progressCallback()
{
    juce::Component::SafePointer<SampleGrabberEditor> safe (this);

    return [safe, token = jobSerial] (float p, juce::String phase)
    {
        if (safe == nullptr || token != safe->jobSerial)
            return;

        safe->progress = p;
        safe->statusText = safe->grabNote.isEmpty() ? phase : phase + kDot + safe->grabNote;
        safe->repaint (safe->progR);
    };
}

std::function<void (Downloader::Result)> SampleGrabberEditor::doneCallback()
{
    juce::Component::SafePointer<SampleGrabberEditor> safe (this);

    return [safe, token = jobSerial] (Downloader::Result r)
    {
        if (safe == nullptr || token != safe->jobSerial)
            return;   // cancelled meanwhile: a finished file stays in the cache (auto-clean)

        safe->busy = false;
        safe->fetchButton.setEnabled (true);
        safe->grabNote.clear();

        if (r.ok)
        {
            const auto url = std::exchange (safe->pendingGrabUrl, juce::String());
            const auto range = std::exchange (safe->pendingGrabRange, juce::String());
            const auto title = std::exchange (safe->pendingGrabTitle, juce::String());
            // Already used by another instance or a BACK entry here (same link grabbed again): never delete it
            const bool shared = sg::storage::isInUse (r.file);
            const bool loaded = safe->loadFromFile (r.file, url, range, title);

            // KEEP DOWNLOADS off: the sample is in RAM and nothing reads the file again (BACK keeps buffers,
            // staging/state/waveform work from RAM). Deleted by the timer once the embedded FLAC is cached
            // (or skipped as too long - then the existing note shows), so a project saved right away keeps the audio.
            if (loaded && ! sg::storage::getKeepDownloads())
            {
                if (shared)
                    std::fprintf (stderr, "[SG] keep downloads off: %s is in use elsewhere - kept\n", r.file.getFullPathName().toRawUTF8());
                else
                    safe->pendingDelete = r.file;
            }
            safe->processor.cache->requestAutoClean();   // after each finished grab/import (new file is registered as in use)
        }
        else
        {
            safe->pendingGrabUrl.clear();
            safe->pendingGrabRange.clear();
            safe->pendingGrabTitle.clear();
            safe->showToolError (r.message);
        }
    };
}

// Result of a length check (yt-dlp probe or ffprobe): drop it if the input changed meanwhile, else apply the guard
std::function<void (Downloader::Result)> SampleGrabberEditor::probeCallback (const GrabSpec& specIn)
{
    juce::Component::SafePointer<SampleGrabberEditor> safe (this);

    return [safe, spec = specIn, token = jobSerial] (Downloader::Result r) mutable
    {
        if (safe == nullptr || token != safe->jobSerial)
            return;

        safe->probing = false;
        safe->busy = false;
        safe->fetchButton.setEnabled (true);

        if (spec.inputSerial != safe->inputSerial)
        {
            safe->statusText = "INPUT CHANGED" + kDot + "CLICK GRAB AGAIN";
            safe->isError = false;
            safe->progress = 0.0f;
            safe->repaint();
            return;
        }
        if (! r.ok)
        {
            safe->showToolError (r.message);
            return;
        }

        spec.duration = r.duration;
        if (spec.isUrl)
            spec.title = r.title;
        if (r.title.isNotEmpty())
            std::fprintf (stderr, "[SG] probe: %s -> %s, '%s'\n", (spec.isUrl ? spec.url : spec.file.getFullPathName()).toRawUTF8(),
                          r.duration >= 0.0 ? sg::range::formatTime (r.duration).toRawUTF8() : "unknown", r.title.toRawUTF8());
        else
            std::fprintf (stderr, "[SG] probe: %s -> %s\n", (spec.isUrl ? spec.url : spec.file.getFullPathName()).toRawUTF8(),
                          r.duration >= 0.0 ? sg::range::formatTime (r.duration).toRawUTF8() : "unknown");
        safe->checkGuard (spec);
    };
}

void SampleGrabberEditor::showStatusError (const juce::String& text)
{
    statusText = text;
    isError = true;
    progress = 0.0f;
    repaint();
}

// Error text of yt-dlp/ffmpeg/ffprobe for the status line
void SampleGrabberEditor::showToolError (const juce::String& message)
{
    const auto up = message.toUpperCase();
    showStatusError (up.contains ("TIKTOK")
        ? juce::String ("ERROR") + kDot + "TIKTOK BLOCKS DOWNLOADS - SAVE THE VIDEO AND DROP THE FILE HERE"
        : "ERROR" + kDot + up.substring (0, 70));
}

void SampleGrabberEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    syncTools();
}

// Apply ToolManager state to button and status line (message thread)
void SampleGrabberEditor::syncTools()
{
    auto& tm = *processor.tools;

    // If a grab/import is running (e.g. installation from another plugin instance), the status line belongs to the grab
    if (! downloader.isBusy())
    {
        if (tm.showsProgress())
        {
            toolsBusy = true;
            busy = true;
            isError = false;
            progress = juce::jmax (0.0f, tm.getProgress());   // < 0 = indeterminate -> running bar stripe
            statusText = tm.getStatusText();
            repaint (progR);
        }
        else if (toolsBusy)
        {
            toolsBusy = false;
            busy = false;
            progress = 0.0f;
            repaint (progR);
        }

        const int run = tm.getRunCounter();
        if (run != lastToolRun && ! tm.isWorking())
        {
            lastToolRun = run;
            const auto note = tm.getLastNote();
            if (note.isNotEmpty())
            {
                isError = tm.lastNoteIsError();
                statusText = note;   // ToolManager supplies the finished line (what worked, what not, what to do)
                progress = 0.0f;
                repaint (progR);
            }
        }
    }

    updateFetchLabel();
    gearButton.setBadge (tm.needsAttention());
}

void SampleGrabberEditor::updateFetchLabel()
{
    const juce::String label (downloader.isBusy() ? "CANCEL"
                              : processor.tools->needsInstall() ? "INSTALL TOOLS" : confirmPending ? "GRAB ALL" : "GRAB");
    if (fetchButton.getButtonText() != label)
    {
        fetchButton.setButtonText (label);
        resized();
    }
    const bool canClick = ! busy || downloader.isBusy();   // CANCEL stays clickable while a job runs
    if (fetchButton.isEnabled() != canClick)
        fetchButton.setEnabled (canClick);
}

// GRAB or INSTALL TOOLS (yt-dlp or ffmpeg missing): one click = consent to the download (~220 MB)
void SampleGrabberEditor::onFetchClicked()
{
    if (downloader.isBusy())
    {
        cancelJob ("CANCELLED", false);
        return;
    }
    if (processor.tools->needsInstall())
    {
        if (! busy && ! processor.tools->isWorking())
            processor.tools->installMissing();
        syncTools();
        return;
    }
    grabOrConfirm();
}

void SampleGrabberEditor::grabOrConfirm()
{
    if (busy || downloader.isBusy())
        return;

    if (confirmPending)
    {
        const auto spec = confirmSpec;   // second click on the same input: grab everything
        clearConfirm();
        proceedGrab (spec);
        return;
    }
    startGrab();
}

// Stops probe/download/conversion: process tree killed, partial files of the job removed, late results ignored
void SampleGrabberEditor::cancelJob (const juce::String& status, bool error)
{
    ++jobSerial;
    downloader.cancel();
    probing = false;
    busy = false;
    pendingGrabUrl.clear();
    pendingGrabRange.clear();
    pendingGrabTitle.clear();
    grabNote.clear();
    statusText = status;
    isError = error;
    progress = 0.0f;
    updateFetchLabel();
    fetchButton.setEnabled (true);
    repaint();
}

void SampleGrabberEditor::beginBusy (const juce::String& status)
{
    busy = true;
    isError = false;
    progress = 0.0f;
    statusText = grabNote.isEmpty() ? status : status + kDot + grabNote;
    fetchButton.setEnabled (false);
    repaint (progR);
}

void SampleGrabberEditor::onInputChanged()
{
    ++inputSerial;
    if (confirmPending)
    {
        clearConfirm();
        if (! busy)
        {
            statusText = "READY";
            isError = false;
            repaint (progR);
        }
    }
}

void SampleGrabberEditor::setConfirm (const GrabSpec& spec, const juce::String& status)
{
    confirmSpec = spec;
    confirmPending = true;
    statusText = status;
    isError = false;
    progress = 0.0f;
    updateFetchLabel();
    repaint();
}

void SampleGrabberEditor::clearConfirm()
{
    confirmPending = false;
    confirmSpec = {};
    updateFetchLabel();
}

// Link with a start time (t=90, t=1h2m3s, start=30): put "FROM-" into an empty range field. A range filled this way
// follows later URL edits; one the user typed is never touched. String parsing only, no network.
void SampleGrabberEditor::prefillRangeFromUrl()
{
    const auto url = urlField.getText().trim();
    juce::String want;
    double s = 0.0;
    if ((url.startsWithIgnoreCase ("http://") || url.startsWithIgnoreCase ("https://")) && sg::range::startFromUrl (url, s))
        want = sg::range::formatTime (s) + "-";

    if (want == lastAutoWant)
        return;   // same start time as before: a range the user cleared or edited stays as it is
    lastAutoWant = want;

    const auto cur = rangeField.getText().trim();
    if (cur.isEmpty() || cur == autoRangeText)
    {
        if (cur != want)
            rangeField.setText (want, false);
        autoRangeText = want;
    }
}

void SampleGrabberEditor::fillSourceFields()
{
    const auto url = processor.getSourceUrl();
    if (url.isEmpty())
        return;
    urlField.setText (url, false);
    rangeField.setText (processor.getSourceRange(), false);
    autoRangeText.clear();
    double s = 0.0;   // the restored range wins over the link's t= on the next URL edit
    lastAutoWant = sg::range::startFromUrl (url, s) ? sg::range::formatTime (s) + "-" : juce::String();
    onInputChanged();
}

bool SampleGrabberEditor::readRangeField (sg::range::Range& out)
{
    const auto p = sg::range::parse (rangeField.getText());
    if (! p.ok)
    {
        showStatusError (p.error);
        return false;
    }
    out = p.range;
    return true;
}

// Length rules: see sg::range::guard (grab, GRAB ALL confirmation, blocked; unknown length of a link -> confirmation)
void SampleGrabberEditor::checkGuard (const GrabSpec& spec)
{
    const auto v = sg::range::guard (spec.range, spec.duration, spec.isUrl);
    switch (v.kind)
    {
        case sg::range::Verdict::Kind::blocked: showStatusError (v.status); return;
        case sg::range::Verdict::Kind::confirm: setConfirm (spec, v.status); return;
        case sg::range::Verdict::Kind::grab:    proceedGrab (spec); return;
    }
}

void SampleGrabberEditor::proceedGrab (const GrabSpec& spec)
{
    if (busy || downloader.isBusy())
        return;

    clearConfirm();
    grabNote.clear();
    if (spec.duration < 0.0 && ! (spec.range.set && spec.range.hasEnd()))
        grabNote = "LENGTH UNKNOWN";
    const auto rangeText = sg::range::toText (spec.range);

    if (spec.isUrl)
    {
        pendingGrabUrl = spec.url;
        pendingGrabRange = rangeText;
        pendingGrabTitle = spec.title;
        beginBusy (spec.range.set ? "DOWNLOADING RANGE" : "STARTING");
        downloader.grab (spec.url, spec.range, spec.duration, ! sg::storage::getKeepDownloads(), progressCallback(), doneCallback());
        updateFetchLabel();
        return;
    }

    if (spec.native && ! spec.range.set && spec.channels <= 2)
    {
        grabNote.clear();
        loadFromFile (spec.file);   // directly, as before (no copy, no ffmpeg)
        return;
    }

    // Conversion and/or cut via ffmpeg into the cache; the original path is kept as the source (RE-GRAB, state)
    pendingGrabUrl = spec.file.getFullPathName();
    pendingGrabRange = rangeText;
    pendingGrabTitle.clear();
    beginBusy ("CONVERTING");
    downloader.importFile (spec.file, spec.range, spec.native ? spec.bits : 16, progressCallback(), doneCallback());
    updateFetchLabel();
}

namespace
{
    // Interprets input text as a local path. Empty = no path (no link, no supported format).
    juce::String toLocalPath (juce::String t)
    {
        if (t.startsWithIgnoreCase ("file://localhost/"))
            t = "file:///" + t.substring (17);   // otherwise "localhost" becomes the first folder
        if (t.startsWithIgnoreCase ("file://"))
            return juce::URL (t).getLocalFile().getFullPathName();

       #if JUCE_LINUX
        // Explorer path into the WSL distribution: \\wsl$\<Distro>\... or \\wsl.localhost\<Distro>\... -> /...
        for (auto prefix : { "\\\\wsl$\\", "\\\\wsl.localhost\\" })
        {
            if (t.startsWithIgnoreCase (prefix))
            {
                const auto rest = t.substring ((int) std::strlen (prefix));
                const int slash = rest.indexOfChar ('\\');
                return "/" + (slash < 0 ? juce::String() : rest.substring (slash + 1).replaceCharacter ('\\', '/'));
            }
        }
       #endif

        // Windows drive path X:\... or X:/...
        if (t.length() >= 3 && juce::CharacterFunctions::isLetter (t[0]) && t[1] == ':'
            && (t[2] == '\\' || t[2] == '/'))
        {
           #if JUCE_LINUX
            return "/mnt/" + t.substring (0, 1).toLowerCase() + "/" + t.substring (3).replaceCharacter ('\\', '/');
           #else
            return t;
           #endif
        }

        if (t.startsWith ("~/"))
            return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName() + t.substring (1);

        if (t.startsWithChar ('/'))
            return t;

        return {};
    }
}

void SampleGrabberEditor::startGrab()
{
    if (busy || downloader.isBusy() || urlField.isEmpty())
        return;

    auto text = urlField.getText().trim();

    // Strip enclosing quotes (Windows "Copy as path")
    if (text.length() >= 2 && ((text.startsWithChar ('"') && text.endsWithChar ('"'))
                               || (text.startsWithChar ('\'') && text.endsWithChar ('\''))))
        text = text.substring (1, text.length() - 1).trim();

    if (text.startsWithIgnoreCase ("http://") || text.startsWithIgnoreCase ("https://"))
    {
        GrabSpec spec;
        spec.isUrl = true;
        spec.url = text;
        spec.inputSerial = inputSerial;
        if (! readRangeField (spec.range))
            return;
        grabNote.clear();
        beginBusy ("CHECKING LINK");   // progress 0 -> running stripe
        downloader.probe (text, probeCallback (spec));
        probing = true;
        probeStartMs = juce::Time::getMillisecondCounter();
        updateFetchLabel();
        return;
    }

    const auto path = toLocalPath (text);
    juce::String err;

    if (path.isEmpty())
    {
        err = "PASTE AN HTTP(S) LINK OR A FILE PATH";
    }
    else
    {
        const juce::File f (path);

        if (! f.existsAsFile())
            err = "FILE NOT FOUND";
        else if (! openLocalFile (f))
            err = "UNSUPPORTED FILE TYPE";
    }

    if (err.isNotEmpty())
    {
        statusText = "ERROR" + kDot + err;
        isError = true;
        progress = 0.0f;
        repaint();
    }
}

bool SampleGrabberEditor::loadFromFile (const juce::File& f, const juce::String& sourceUrl, const juce::String& sourceRange,
                                        const juce::String& displayTitle)
{
    const bool ok = processor.loadSample (f, true, sourceUrl, sourceRange,
                                          displayTitle.isNotEmpty() ? displayTitle : snapshotTitle);
    if (ok)
    {
        waveform.resetView();
        pads.refreshThumbs();
        statusText = "READY" + kDot + loadedName();
        isError = false;
        progress = 1.0f;
    }
    else
    {
        statusText = "ERROR" + kDot + "COULD NOT READ FILE";
        isError = true;
        progress = 0.0f;
    }
    repaint();
    return ok;
}

// Message thread: display title under the processor's metaLock (getDisplayTitle), else the name from the file
juce::String SampleGrabberEditor::loadedName() const
{
    return prettyName (processor.getLoadedFile(), processor.getDisplayTitle());
}

void SampleGrabberEditor::syncSliceModeButtons()
{
    using Mode = SampleGrabberProcessor::SliceMode;
    const auto m = processor.getSliceMode();
    (m == Mode::grid ? gridButton : m == Mode::beat ? beatButton : transientButton)
        .setToggleState (true, juce::dontSendNotification);   // radio group switches the others off
}

void SampleGrabberEditor::applyRestoredState()
{
    lastStateVersion = processor.getStateVersion();
    if (processor.hasSample())
    {
        waveform.resetView();
        pads.refreshThumbs();
    }
    syncSliceModeButtons();
    waveform.repaint();
    pads.repaint();
    repaintInfo();

    if (busy)
        return;   // a running grab owns the status line

    const auto note = processor.getRestoreNote();
    const auto url = processor.getSourceUrl();
    if (note.isNotEmpty())
    {
        statusText = note;
        isError = ! processor.hasSample();
        progress = 0.0f;
        if (url.isNotEmpty())
            fillSourceFields();   // GRAB fetches the same source (and range) again
    }
    else if (processor.hasSample())
    {
        statusText = "READY" + kDot + loadedName();
        isError = false;
        progress = 1.0f;
        fillSourceFields();
    }
    repaint();
}

void SampleGrabberEditor::goBack()
{
    if (busy)
        return;   // during download/import: the result would overwrite the state again right away

    if (! processor.goBack())
        return;

    waveform.resetView();
    pads.refreshThumbs();
    syncSliceModeButtons();
    fillSourceFields();   // link + range of the sample BACK returned to (RE-GRAB fetches that one)
    statusText = "READY" + kDot + loadedName();
    isError = false;
    progress = 1.0f;
    repaint();
}

void SampleGrabberEditor::dragOut (int pad)
{
    if (! processor.hasSample())
        return;

    // Normally a hard link of the pre-rendered file; synchronous render only if staging is not ready
    const auto f = processor.prepareDragFile (pad);
    if (f.existsAsFile())
    {
       #if JUCE_LINUX
        // JUCE writes plain paths into the XDND uri-list as "file://" + path without percent-encoding;
        // strict targets (Reaper) reject paths with spaces. Strings that already look like URLs are passed through as-is.
        const auto dragItem = juce::URL (f).toString (false);
       #else
        const auto dragItem = f.getFullPathName();
       #endif
        juce::DragAndDropContainer::performExternalDragDropOfFiles (juce::StringArray (dragItem), false, this);
    }
    else
    {
        statusText = "EXPORT FAILED" + kDot + "CHECK FOLDER";   // details in sg.log ([SG] drag: ...)
        isError = true;
        progress = 0.0f;
        repaint (progR);
    }
}

//==============================================================================
bool SampleGrabberEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (settings.isVisible())
        return false;   // overlay open: no drop underneath

    bool ok = false;
    for (const auto& path : files)
    {
        const juce::File f (path);
        if (isNative (f) || isConvert (f))
            ok = true;
    }

    // Called on every drag move: log only when the offered list changes
    static juce::String lastLogged;
    const auto joined = files.joinIntoString ("|");
    if (joined != lastLogged)
    {
        lastLogged = joined;
        std::fprintf (stderr, "[SG] drop in: %d file(s), %s, first '%s'\n", files.size(), ok ? "accepted" : "rejected",
                      files[0].toRawUTF8());
    }
    return ok;
}

void SampleGrabberEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    dragOver = true;
    repaint();
}

void SampleGrabberEditor::fileDragExit (const juce::StringArray&)
{
    dragOver = false;
    repaint();
}

void SampleGrabberEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    repaint();

    if (busy)
    {
        statusText = "BUSY" + kDot + "WAIT FOR THE CURRENT DOWNLOAD";
        repaint (progR);
        return;
    }

    for (const auto& path : files)
        if (openLocalFile (juce::File (path)))
            break;
}

// Some XDND sources (e.g. XWayland bridges) offer text/plain before text/uri-list; JUCE then delivers
// the uri-list as text. Turn "file://" lines and absolute paths back into file paths.
static juce::StringArray filesFromDragText (const juce::String& text)
{
    juce::StringArray out;
    for (auto line : juce::StringArray::fromLines (text))
    {
        line = line.trim();
        if (line.startsWithIgnoreCase ("file://"))
            out.add (juce::URL::removeEscapeChars (line.replace ("+", "%2B").substring (7)));
        else if (line.startsWithChar ('/'))
            out.add (line);
    }
    return out;
}

bool SampleGrabberEditor::isInterestedInTextDrag (const juce::String& text)
{
    const auto files = filesFromDragText (text);
    static juce::String lastLogged;
    if (text != lastLogged)
    {
        lastLogged = text;
        std::fprintf (stderr, "[SG] drop in as text: '%s' -> %d path(s)\n", text.substring (0, 200).toRawUTF8(), files.size());
    }
    return ! files.isEmpty() && isInterestedInFileDrag (files);
}

void SampleGrabberEditor::textDragEnter (const juce::String& text, int x, int y)  { fileDragEnter (filesFromDragText (text), x, y); }
void SampleGrabberEditor::textDragExit (const juce::String& text)                 { fileDragExit (filesFromDragText (text)); }
void SampleGrabberEditor::textDropped (const juce::String& text, int x, int y)    { filesDropped (filesFromDragText (text), x, y); }

// Local path or dropped file: the range field applies here too. Length first (header or ffprobe), then the guard.
// The user's file is only read; conversions and cuts go to the cache.
bool SampleGrabberEditor::openLocalFile (const juce::File& f)
{
    const bool native = isNative (f);
    if (! native && ! isConvert (f))
        return false;
    if (busy || downloader.isBusy())
        return true;

    GrabSpec spec;
    spec.file = f;
    spec.native = native;
    spec.inputSerial = inputSerial;
    if (! readRangeField (spec.range))
        return true;

    grabNote.clear();
    if (native)
    {
        if (! readNativeInfo (f, spec.duration, spec.bits, spec.channels))
        {
            showToolError ("COULD NOT READ FILE");
            return true;
        }
        checkGuard (spec);
        return true;
    }

    beginBusy ("CHECKING FILE");
    downloader.probeFile (f, probeCallback (spec));
    probing = true;
    probeStartMs = juce::Time::getMillisecondCounter();
    updateFetchLabel();
    return true;
}

//==============================================================================
void SampleGrabberEditor::timerCallback()
{
    // Theme switched in another plugin instance: follow
    if (ui::theme() != shownTheme)
        refreshTheme();

    // Host restored a plugin state while the editor is open
    if (processor.getStateVersion() != lastStateVersion)
        applyRestoredState();

    // Selection too long/large to embed in the project: say so once per sample
    if (processor.getEmbedWarnVersion() != lastEmbedWarn)
    {
        lastEmbedWarn = processor.getEmbedWarnVersion();
        if (! busy)
        {
            statusText = "SELECTION TOO LONG TO SAVE IN PROJECT" + kDot + "LINK KEPT";
            isError = false;
            repaint (progR);
        }
    }

    // Indeterminate progress: only the bar stripe, only while busy
    if (busy)
        repaint (progR.withHeight (2));

    // Length check hangs (network): yt-dlp has --socket-timeout 15, this is the hard limit
    if (probing && juce::Time::getMillisecondCounter() - probeStartMs > 45000)
        cancelJob ("LINK CHECK TIMED OUT - TRY AGAIN", true);

    // GRAB / GRAB ALL / CANCEL / INSTALL TOOLS (job state can change without a callback, e.g. thread winding down)
    updateFetchLabel();

    // KEEP DOWNLOADS off: delete the cache file once the project can carry the selection without it
    if (pendingDelete != juce::File() && processor.isEmbedSettled())
    {
        const auto f = std::exchange (pendingDelete, juce::File());
        if (sg::storage::deleteCacheSource (f))
        {
            processor.noteSourceNotKept (f);
            if (! busy && ! isError && statusText.startsWith ("READY"))
            {
                statusText << kDot << "DOWNLOAD NOT KEPT";
                repaint (progR);
            }
        }
    }

    // Redraw pads only when the active pad changes (not continuously at 30 Hz)
    const int activePadNow = processor.getActivePad();
    if (activePadNow != lastActivePad)
    {
        lastActivePad = activePadNow;
        pads.repaint();
    }

    // Expensive work (mini waveforms, BPM/key) only once no mouse button is held
    if ((++tick % 5) == 0 && ! juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
    {
        if (processor.getSliceVersion() != pads.knownVersion())
        {
            pads.refreshThumbs();
            waveform.repaint();   // redraw markers immediately after a slice change (x2, 1/2, tempo analysis in BEAT mode)
        }

        if (processor.getSelectionVersion() != lastAnalysisVersion)
        {
            lastAnalysisVersion = processor.getSelectionVersion();
            processor.analyse();
            repaintInfo();
        }

        // Rescan the selection peak only when selection, sample, render or pitch/speed-neutral changed
        const auto peakKey = processor.getPeakKey();
        if (peakKey != lastPeakKey)
        {
            lastPeakKey = peakKey;
            selectionPeak = processor.computeSelectionPeak();
        }
    }

    // Overload: cheap (one multiplication), so every tick, so the gain knob reacts immediately
    gainCell.setWarning (selectionPeak * SampleGrabberProcessor::gainFromDb (processor.getGainDb()) > 1.0f);

    const bool canHalf = processor.canShiftBpmOctave (-1);
    const bool canDouble = processor.canShiftBpmOctave (+1);
    if (bpmHalfButton.isEnabled() != canHalf)
        bpmHalfButton.setEnabled (canHalf);
    if (bpmDoubleButton.isEnabled() != canDouble)
        bpmDoubleButton.setEnabled (canDouble);

    if (backButton.isEnabled() != processor.canGoBack())
        backButton.setEnabled (processor.canGoBack());

    const bool playing = processor.getPlayheadNorm() >= 0.0f;
    const float fi = processor.getFadeInPct(), fo = processor.getFadeOutPct(), fs = processor.getFadeShape();
    if (playing || wasPlaying || fi != lastFadeIn || fo != lastFadeOut || fs != lastFadeShape)
        waveform.repaint();
    wasPlaying = playing;
    {
        // Play/pause button: pause icon (red) while playing, otherwise play icon
        const bool running = playing && ! processor.isPaused();
        playButton.setIcon (running ? IconButton::Icon::pause : IconButton::Icon::play);
        if (playButton.getToggleState() != running)
            playButton.setToggleState (running, juce::dontSendNotification);
    }

    // Keep the hint current when the mouse stays on the button after a click
    if (playButton.isMouseOver() && playButton.onHover != nullptr)
        playButton.onHover (true);
    else if (loopButton.isMouseOver() && loopButton.onHover != nullptr)
        loopButton.onHover (true);
    else if (backButton.isMouseOver() && backButton.onHover != nullptr)
        backButton.onHover (true);
    lastFadeIn = fi;
    lastFadeOut = fo;
    lastFadeShape = fs;

    const bool rendering = processor.isRendering();
    if (rendering != lastRendering)
    {
        lastRendering = rendering;
        repaint (infoR);
        repaintInfo();
    }

    if (processor.audioInfo != lastAudioInfo)
    {
        lastAudioInfo = processor.audioInfo;
        repaint (headerR);
    }
}

void SampleGrabberEditor::mouseDown (const juce::MouseEvent& e)
{
    if (tabsR.contains (e.getPosition()))
    {
        const int tw = tabsR.getWidth() / 4;
        activeSource = juce::jlimit (0, 3, (e.x - tabsR.getX()) / juce::jmax (1, tw));
        repaint (tabsR);
    }
}

//==============================================================================
// Focus change in the URL field: repaint the border
void SampleGrabberEditor::focusOfChildComponentChanged (FocusChangeType)
{
    repaint (urlR);
}

// Drop state over the waveform (sits above the child component)
void SampleGrabberEditor::paintOverChildren (juce::Graphics& g)
{
    if (! dragOver)
        return;

    const auto r = waveform.getBounds();
    g.setColour (ui::c().red.withAlpha (0.12f));
    g.fillRect (r);
    g.setColour (ui::c().red);
    g.drawRect (r, 1);
    // Backing plate in background colour so the text stays readable over the waveform
    const auto font = ui::mono (12.0f);
    const juce::String text ("DROP AUDIO");
    const int tw = (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, text));
    const auto plate = r.withSizeKeepingCentre (tw + 24, 28);
    g.setColour (ui::c().canvas);
    g.fillRect (plate);
    g.setColour (ui::c().red);
    g.drawRect (plate, 1);
    g.setFont (font);
    g.drawText (text, plate, juce::Justification::centred);
}

void SampleGrabberEditor::resized()
{
    auto area = getLocalBounds().reduced (24);

    headerR = area.removeFromTop (38);
    area.removeFromTop (8);
    tabsR = area.removeFromTop (30);
    area.removeFromTop (12);
    urlR = area.removeFromTop (36);
    area.removeFromTop (10);
    progR = area.removeFromTop (26);
    area.removeFromTop (8);

    auto waveRow = area.removeFromTop (146);
    auto left = waveRow.removeFromLeft (waveRow.getWidth() * 2 / 3 - 8);
    waveRow.removeFromLeft (16);
    waveLabelR = left.removeFromTop (16);
    waveform.setBounds (left);
    detLabelR = waveRow.removeFromTop (16);
    detR = waveRow;

    area.removeFromTop (16);
    slicesLabelR = area.removeFromTop (20);
    area.removeFromTop (4);
    pads.setBounds (area.removeFromTop (40));
    area.removeFromTop (14);

    paramsR = area.removeFromTop (92);
    KnobCell* cells[] = { &gainCell, &pitchCell, &speedCell, &fadeInCell, &fadeOutCell, &shapeCell };
    static_assert (std::size (cells) == (size_t) numKnobCells, "divider lines in paint() would not match otherwise");
    constexpr int numCells = numKnobCells;
    const int cw = paramsR.getWidth() / numCells;
    for (int i = 0; i < numCells; ++i)
    {
        const int w = (i == numCells - 1) ? paramsR.getRight() - (paramsR.getX() + (numCells - 1) * cw) : cw;
        cells[i]->setBounds (paramsR.getX() + i * cw, paramsR.getY(), w, paramsR.getHeight());
    }

    area.removeFromTop (16);
    footerR = area.removeFromTop (36);

    auto fr = footerR;
    dragHandle.setBounds (fr.removeFromRight (150).withSizeKeepingCentre (150, 32));

    // Transport: [Play/Pause][Stop][Loop]  12 px  [Back], each 32x32, 4 px gap
    IconButton* transport[] = { &playButton, &stopButton, &loopButton };
    for (auto* b : transport)
    {
        b->setBounds (fr.removeFromLeft (32).withSizeKeepingCentre (32, 32));
        fr.removeFromLeft (4);
    }
    fr.removeFromLeft (8);   // 4 + 8 = 12 px up to the BACK group
    backButton.setBounds (fr.removeFromLeft (32).withSizeKeepingCentre (32, 32));
    infoR = fr.withTrimmedLeft (10).withTrimmedRight (8);

    auto hr = headerR;
    gearButton.setBounds (hr.removeFromRight (32).withSizeKeepingCentre (32, 32));
    settings.setBounds (getLocalBounds());

    auto sl = slicesLabelR;
    transientButton.setBounds (sl.removeFromRight (84).withSizeKeepingCentre (84, 18));
    sl.removeFromRight (4);
    beatButton.setBounds (sl.removeFromRight (50).withSizeKeepingCentre (50, 18));
    sl.removeFromRight (4);
    gridButton.setBounds (sl.removeFromRight (50).withSizeKeepingCentre (50, 18));

    // Tempo-octave buttons sit in the BPM row of the Detected panel (same geometry as in paint())
    {
        auto inner = detR.reduced (10, 8);
        inner.removeFromTop (34);
        const int rowH = inner.getHeight() / 4;
        const int rowY = inner.getY() + 2 * rowH;
        bpmHalfButton.setBounds (inner.getX() + 34, rowY + (rowH - 16) / 2, 28, 16);
        bpmDoubleButton.setBounds (inner.getX() + 66, rowY + (rowH - 16) / 2, 28, 16);
    }

    auto ur = urlR.reduced (1);
    fetchButton.setBounds (ur.removeFromRight (fetchButton.getButtonText() == "INSTALL TOOLS" ? 124 : 90));   // INSTALL TOOLS needs more room (Classic font)
    rangeR = ur.removeFromRight (176);
    rangeField.setBounds (rangeR.withTrimmedLeft (12 + 44).withTrimmedRight (10).reduced (0, 6));
    urlField.setBounds (ur.reduced (12, 6));
}

void SampleGrabberEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::c().canvas);

    // --- Header ---
    {
        // Lockup on the left (30 px high, aspect ratio 901x192 from the drawable)
        if (lockup != nullptr)
        {
            const float lh = 30.0f;   // 18 px per UI_BRAND.md was too small (user)
            const float lw = lh * 901.0f / 192.0f;
            lockup->drawWithin (g, juce::Rectangle<float> ((float) headerR.getX(), (float) headerR.getCentreY() - lh * 0.5f, lw, lh),
                                juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yMid, 1.0f);
        }
        else
        {
            g.setColour (ui::c().textPrimary);
            g.setFont (ui::strong (13.0f));
            g.drawText ("SAMPLE GRABBER", headerR, juce::Justification::centredLeft);
        }

        // Right: space left of the theme and audio buttons
        auto mr = headerR.withTrimmedRight (gearButton.getWidth() + 8);
        mr.setLeft (headerR.getX() + 240);

        if (processor.onOpenAudioSettings != nullptr && ! processor.audioOk)
        {
            g.setColour (ui::c().red);
            g.setFont (ui::label (10.0f));
            g.drawText (processor.audioInfo, mr, juce::Justification::centredRight);
        }
        else
        {
            const bool has = processor.hasSample();
            const double sr = juce::jmax (1.0, processor.getFileSampleRate());
            const double total = has ? (double) processor.getNumSamples() / sr : 0.0;
            const double sel = total * (double) (processor.getTrimEnd() - processor.getTrimStart());
            const float bpm = processor.getBpm();
            const int key = processor.getKey();

            const juce::String bpmPart = ((has && bpm > 0.0f) ? bpmString (bpm) : kDash) + " BPM";
            juce::String keyPart = kDash;
            if (has && key >= 0)
                keyPart = juce::String (kKeyNames[key % 12]) + (key >= 12 ? " MIN" : " MAJ");
            const juce::String selPart = has ? clockString (sel) : kDash;

            const bool keyDim = has && key >= 0 && processor.getKeyConfidence() < 0.5f;
            const auto font = ui::mono (12.0f);
            g.setFont (font);

            // draw right to left, parts separately so only the key can be dimmed
            auto drawPart = [&] (const juce::String& txt, juce::Colour col)
            {
                const int w = (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, txt)) + 2;
                g.setColour (col);
                g.drawText (txt, mr.removeFromRight (w), juce::Justification::centredRight);
            };
            drawPart (selPart, ui::c().textPrimary);
            drawPart (kDot, ui::c().textPrimary);
            drawPart (keyPart, keyDim ? ui::c().textSecondary : ui::c().textPrimary);
            drawPart (kDot, ui::c().textPrimary);
            drawPart (bpmPart, ui::c().textPrimary);
        }
    }

    g.setColour (ui::c().hairline);
    g.fillRect (headerR.getX(), headerR.getBottom() - 1, headerR.getWidth(), 1);

    // --- Source tabs ---
    {
        const int tw = tabsR.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
        {
            const int w = (i == 3) ? tabsR.getRight() - (tabsR.getX() + 3 * tw) : tw;
            const juce::Rectangle<int> t (tabsR.getX() + i * tw, tabsR.getY(), w, tabsR.getHeight());

            if (i == activeSource)
            {
                g.setColour (ui::c().hairline.withAlpha (0.35f));
                g.fillRect (t);
                g.setColour (ui::c().red);
                g.fillRect (t.getX(), t.getBottom() - 2, t.getWidth(), 2);
            }

            g.setColour (i == activeSource ? ui::c().textPrimary : ui::c().textSecondary);
            g.setFont (ui::label (10.0f));
            g.drawText (kSourceNames[i], t, juce::Justification::centred);

            if (i < 3)
            {
                g.setColour (ui::c().hairline);
                g.fillRect (t.getRight() - 1, t.getY(), 1, t.getHeight());
            }
        }
        g.setColour (ui::c().hairline);
        g.drawRect (tabsR, 1);
    }

    // --- URL field: fill surface1, border hairline, hairlineStrong on focus ---
    g.setColour (ui::c().surface1);
    g.fillRect (urlR);
    g.setColour (urlField.hasKeyboardFocus (true) || rangeField.hasKeyboardFocus (true) ? ui::c().hairlineStrong : ui::c().hairline);
    g.drawRect (urlR, 1);
    // Range box: divider on the left, small label in front of the field
    g.setColour (ui::c().hairline);
    g.fillRect (rangeR.getX(), urlR.getY() + 1, 1, urlR.getHeight() - 2);
    g.setColour (ui::c().textSecondary);
    g.setFont (ui::label (10.0f));
    g.drawText ("RANGE", rangeR.withTrimmedLeft (12).withWidth (44), juce::Justification::centredLeft);

    // --- Status / progress ---
    {
        const auto bar = progR.withHeight (2);
        const bool indeterminate = busy && (progress <= 0.001f || statusText.startsWith ("CONVERTING"));

        if (busy)
        {
            g.setColour (ui::c().hairline);
            g.fillRect (bar);
        }

        if (indeterminate)
        {
            // 25 % wide segment, about 1.2 s per pass
            const int sw = bar.getWidth() / 4;
            const float t = (float) (juce::Time::getMillisecondCounter() % 1200u) / 1200.0f;
            const int x = bar.getX() + (int) std::lround (t * (float) (bar.getWidth() + sw)) - sw;
            g.setColour (ui::c().red);
            g.fillRect (juce::Rectangle<int> (x, bar.getY(), sw, bar.getHeight()).getIntersection (bar));
        }
        else if (busy)
        {
            g.setColour (ui::c().red);
            g.fillRect (bar.withWidth ((int) std::lround ((float) bar.getWidth() * juce::jlimit (0.0f, 1.0f, progress))));
        }

        const auto textR = progR.withTrimmedTop (6);
        // "Tools missing" hint is a call to action: primary colour instead of dimmed
        const bool callToAction = ! busy && ! isError && (processor.tools->needsInstall() || confirmPending);
        g.setFont (ui::monoUi (10.0f));
        g.setColour (isError ? ui::c().red : (callToAction ? ui::c().textPrimary : ui::c().textSecondary));
        g.drawText (statusText, textR.withTrimmedRight (50), juce::Justification::centredLeft);

        if (busy && ! indeterminate)
        {
            g.setColour (ui::c().textSecondary);
            g.drawText (juce::String ((int) std::lround (progress * 100.0f)) + "%", textR, juce::Justification::centredRight);
        }
    }

    // --- Labels ---
    g.setColour (ui::c().textSecondary);
    g.setFont (ui::label (10.0f));
    g.drawText ("WAVEFORM", waveLabelR, juce::Justification::centredLeft);
    g.drawText ("DETECTED", detLabelR, juce::Justification::centredLeft);
    g.drawText ("SLICES", slicesLabelR, juce::Justification::centredLeft);

    // --- Detected panel ---
    {
        g.setColour (ui::c().hairline);
        g.drawRect (detR, 1);

        const bool has = processor.hasSample();
        const double sr = juce::jmax (1.0, processor.getFileSampleRate());
        const double total = has ? (double) processor.getNumSamples() / sr : 0.0;
        const double sel = total * (double) (processor.getTrimEnd() - processor.getTrimStart());

        // BPM (incl. result after speed change) and key
        juce::String bpmText = kDash, keyText = kDash;
        bool bpmDim = true, keyDim = true;

        const float bpm = processor.getBpm();
        if (has && bpm > 0.0f)
        {
            bpmText = bpmString (bpm);
            bpmDim = false;
            const float sp = processor.getSpeedFactor();
            if (std::abs (sp - 1.0f) > 0.002f)
                bpmText += " > " + bpmString (bpm * sp);
        }

        const int key = processor.getKey();
        if (has && key >= 0)
        {
            const int tonic = key % 12;
            const bool isMinor = key >= 12;
            const int rel = isMinor ? (tonic + 3) % 12 : (tonic + 9) % 12;   // relative major/minor key (same pitch set)
            keyText = juce::String (kKeyNames[tonic]) + (isMinor ? " MIN" : " MAJ")
                      + " / " + kKeyNames[rel] + (isMinor ? " MAJ" : " MIN");
            keyDim = processor.getKeyConfidence() < 0.5f;   // show uncertain detection dimmed
        }

        auto inner = detR.reduced (10, 8);
        const auto nameR = inner.removeFromTop (34);

        g.setColour (has ? ui::c().textPrimary : ui::c().textSecondary);
        g.setFont (ui::strong (11.0f));
        g.drawFittedText (has ? loadedName() : kDash, nameR, juce::Justification::topLeft, 2);

        const int rowH = inner.getHeight() / 4;
        auto row = [&] (int idx, const juce::String& k, const juce::String& v, bool dim)
        {
            const auto r = inner.withY (inner.getY() + idx * rowH).withHeight (rowH);
            g.setColour (ui::c().textSecondary);
            g.setFont (ui::label (10.0f));
            g.drawText (k, r, juce::Justification::centredLeft);
            g.setColour (dim ? ui::c().textSecondary : ui::c().textPrimary);
            g.setFont (ui::strong (11.0f));
            g.drawText (v, r.withTrimmedLeft (40), juce::Justification::centredRight);
        };

        row (0, "LEN", has ? timeString (total) : kDash, ! has);
        row (1, "SEL", has ? timeString (sel) : kDash, ! has);
        row (2, "BPM", bpmText, bpmDim);
        row (3, "KEY", keyText, keyDim);
    }

    // --- Knob bar: border + divider lines ---
    {
        g.setColour (ui::c().hairline);
        g.drawRect (paramsR, 1);
        const int cw = paramsR.getWidth() / numKnobCells;
        for (int i = 1; i < numKnobCells; ++i)
            g.fillRect (paramsR.getX() + i * cw, paramsR.getY(), 1, paramsR.getHeight());
    }

    // --- Footer info ---
    {
        juce::String info = "NO SAMPLE";
        if (processor.hasSample())
            info = juce::String (processor.getFileSampleRate() / 1000.0, 1) + "KHZ" + kDot
                   + juce::String (processor.getFileBitDepth()) + "BIT" + kDot + "WAV";

        g.setColour (ui::c().textSecondary);
        if (hoverHint.isNotEmpty())
        {
            g.setFont (ui::mono (10.0f));
            g.drawText (hoverHint, infoR, juce::Justification::centredLeft);
        }
        else
        {
            g.setFont (ui::label (10.0f));
            g.drawText (info, infoR, juce::Justification::centredLeft);
        }

        if (processor.isRendering())
        {
            g.setColour (ui::c().red);
            g.drawText ("PROCESSING", infoR, juce::Justification::centredRight);
        }
    }
}
