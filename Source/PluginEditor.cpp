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

    // yt-dlp appends "_<11-char-ID>" and replaces spaces with "_"
    juce::String prettyName (const juce::File& f)
    {
        auto n = f.getFileNameWithoutExtension();
        const int us = n.lastIndexOfChar ('_');
        if (us > 0 && n.length() - us - 1 == 11)
            n = n.substring (0, us);
        return n.replaceCharacter ('_', ' ').toUpperCase();
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
    urlField.onReturnKey = [this] { startGrab(); };
    urlField.onTextChange = [this]
    {
        const int s = detectSource (urlField.getText());
        if (s >= 0 && s != activeSource)
        {
            activeSource = s;
            repaint (tabsR);
        }
    };
    addAndMakeVisible (urlField);

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

        // SG_SNAPSHOT_SETTINGS=1: show the settings overlay open
        if (juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_SETTINGS", {}).isNotEmpty())
            showSettings (true);
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

    return [safe] (float p, juce::String phase)
    {
        if (safe == nullptr)
            return;

        safe->progress = p;
        safe->statusText = phase;
        safe->repaint (safe->progR);
    };
}

std::function<void (Downloader::Result)> SampleGrabberEditor::doneCallback()
{
    juce::Component::SafePointer<SampleGrabberEditor> safe (this);

    return [safe] (Downloader::Result r)
    {
        if (safe == nullptr)
            return;

        safe->busy = false;
        safe->fetchButton.setEnabled (true);

        if (r.ok)
        {
            safe->loadFromFile (r.file);
        }
        else
        {
            const auto up = r.message.toUpperCase();
            safe->statusText = up.contains ("TIKTOK")
                ? juce::String ("ERROR") + kDot + "TIKTOK BLOCKS DOWNLOADS - SAVE THE VIDEO AND DROP THE FILE HERE"
                : "ERROR" + kDot + up.substring (0, 70);
            safe->isError = true;
            safe->progress = 0.0f;
            safe->repaint();
        }
    };
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

    const juce::String label (tm.needsInstall() ? "INSTALL TOOLS" : "GRAB");
    if (fetchButton.getButtonText() != label)
    {
        fetchButton.setButtonText (label);
        resized();
    }
    fetchButton.setEnabled (! busy);
    gearButton.setBadge (tm.needsAttention());
}

// GRAB or INSTALL TOOLS (yt-dlp or ffmpeg missing): one click = consent to the download (~220 MB)
void SampleGrabberEditor::onFetchClicked()
{
    if (processor.tools->needsInstall())
    {
        if (! busy && ! processor.tools->isWorking())
            processor.tools->installMissing();
        syncTools();
        return;
    }
    startGrab();
}

void SampleGrabberEditor::beginBusy (const juce::String& status)
{
    busy = true;
    isError = false;
    progress = 0.0f;
    statusText = status;
    fetchButton.setEnabled (false);
    repaint (progR);
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
        beginBusy ("STARTING");
        downloader.grab (text, progressCallback(), doneCallback());
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

void SampleGrabberEditor::loadFromFile (const juce::File& f)
{
    if (processor.loadSample (f))
    {
        waveform.resetView();
        pads.refreshThumbs();
        statusText = "READY" + kDot + prettyName (f);
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
}

void SampleGrabberEditor::syncSliceModeButtons()
{
    using Mode = SampleGrabberProcessor::SliceMode;
    const auto m = processor.getSliceMode();
    (m == Mode::grid ? gridButton : m == Mode::beat ? beatButton : transientButton)
        .setToggleState (true, juce::dontSendNotification);   // radio group switches the others off
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
    statusText = "READY" + kDot + prettyName (processor.getLoadedFile());
    isError = false;
    progress = 1.0f;
    repaint();
}

void SampleGrabberEditor::dragOut (int pad)
{
    if (! processor.hasSample())
        return;

    const auto f = processor.exportRegion (pad);
    if (f.existsAsFile())
        juce::DragAndDropContainer::performExternalDragDropOfFiles (juce::StringArray (f.getFullPathName()), false, this);
}

//==============================================================================
bool SampleGrabberEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (settings.isVisible())
        return false;   // overlay open: no drop underneath

    for (const auto& path : files)
    {
        const juce::File f (path);
        if (isNative (f) || isConvert (f))
            return true;
    }
    return false;
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

bool SampleGrabberEditor::openLocalFile (const juce::File& f)
{
    if (isNative (f))
    {
        loadFromFile (f);
        return true;
    }

    if (isConvert (f))
    {
        if (! downloader.isBusy())
        {
            beginBusy ("CONVERTING");
            downloader.importFile (f, progressCallback(), doneCallback());
        }
        return true;
    }

    return false;
}

//==============================================================================
void SampleGrabberEditor::timerCallback()
{
    // Theme switched in another plugin instance: follow
    if (ui::theme() != shownTheme)
        refreshTheme();

    // Indeterminate progress: only the bar stripe, only while busy
    if (busy)
        repaint (progR.withHeight (2));

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
    fetchButton.setBounds (ur.removeFromRight (fetchButton.getButtonText() == "GRAB" ? 90 : 124));   // INSTALL TOOLS needs more room (Classic font)
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
    g.setColour (urlField.hasKeyboardFocus (true) ? ui::c().hairlineStrong : ui::c().hairline);
    g.drawRect (urlR, 1);

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
        const bool callToAction = ! busy && ! isError && processor.tools->needsInstall();
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
        g.drawFittedText (has ? prettyName (processor.getLoadedFile()) : kDash, nameR, juce::Justification::topLeft, 2);

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
