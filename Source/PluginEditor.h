// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "PluginProcessor.h"
#include "Downloader.h"
#include "ui/SGLookAndFeel.h"
#include "WaveformView.h"
#include "Widgets.h"
#include "KnobCell.h"
#include "IconButton.h"
#include "SettingsOverlay.h"
#include "TimeRange.h"

class SampleGrabberEditor : public juce::AudioProcessorEditor,
                            public juce::FileDragAndDropTarget,
                            public juce::TextDragAndDropTarget,
                            private juce::Timer,
                            private juce::ChangeListener
{
public:
    explicit SampleGrabberEditor (SampleGrabberProcessor&);
    ~SampleGrabberEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void focusOfChildComponentChanged (FocusChangeType) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void applyTheme();

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    bool isInterestedInTextDrag (const juce::String&) override;
    void textDragEnter (const juce::String&, int, int) override;
    void textDragExit (const juce::String&) override;
    void textDropped (const juce::String&, int, int) override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;   // ToolManager: progress/result
    void syncTools();
    void onFetchClicked();
    void startGrab();
    void beginBusy (const juce::String& status);
    bool loadFromFile (const juce::File&, const juce::String& sourceUrl = {}, const juce::String& sourceRange = {},
                       const juce::String& displayTitle = {});
    juce::String loadedName() const;               // name of the loaded sample for status line and DETECTED panel

    // One grab request: link or local file, optional range, length once known (probe or file header)
    struct GrabSpec
    {
        bool isUrl = false;
        juce::String url;                 // link (isUrl)
        juce::String title;               // link: real title from the probe (display only), may be empty
        juce::File file;                  // local file (! isUrl)
        bool native = false;              // JUCE reads it directly (no ffmpeg needed without a range)
        int bits = 16;                    // native: bit depth, kept when cutting a range
        int channels = 2;                 // native: > 2 goes through ffmpeg (stereo downmix)
        sg::range::Range range;
        double duration = -1.0;           // seconds, < 0 = unknown
        int inputSerial = 0;              // inputSerial when the request started (stale results are dropped)
    };

    // Footer hint for a child that is not an IconButton (mouse enter/exit of the component and its children)
    struct HoverRelay : public juce::MouseListener
    {
        std::function<void (bool)> onHover;
        void mouseEnter (const juce::MouseEvent&) override { if (onHover) onHover (true); }
        void mouseExit (const juce::MouseEvent&) override  { if (onHover) onHover (false); }
    };

    void grabOrConfirm();                          // GRAB / Return: GRAB ALL if a long source waits for confirmation
    bool readRangeField (sg::range::Range& out);   // false = invalid (error shown)
    void checkGuard (const GrabSpec&);             // range beyond end, long source (confirm), too long (blocked)
    void proceedGrab (const GrabSpec&);
    void setConfirm (const GrabSpec&, const juce::String& status);
    void clearConfirm();
    void updateFetchLabel();
    void onInputChanged();                         // URL or range edited: drop a pending confirmation
    void prefillRangeFromUrl();
    void fillSourceFields();                       // URL + range of the loaded/restored sample into the fields
    void showStatusError (const juce::String& text);
    void showToolError (const juce::String& message);
    std::function<void (Downloader::Result)> probeCallback (const GrabSpec&);
    void cancelJob (const juce::String& status, bool error);   // CANCEL button, Esc, probe watchdog
    void applyRestoredState();                     // after setStateInformation: refresh widgets, note, URL field
    bool openLocalFile (const juce::File&);   // true = file type supported
    void goBack();
    void repaintInfo();
    void rebuildLockup();
    void setThemeFromSettings (ui::ThemeId);
    void showSettings (bool show);
    void refreshTheme();
    void syncSliceModeButtons();
    void dragOut (int pad);
    std::function<void (float, juce::String)> progressCallback();
    std::function<void (Downloader::Result)> doneCallback();
    static int detectSource (const juce::String& url);

    SampleGrabberProcessor& processor;
    SGLookAndFeel laf;
    Downloader downloader;

    juce::TextEditor urlField;
    HoverRelay rangeHover;                         // declared before rangeField -> outlives it
    juce::TextEditor rangeField;                   // "FROM-TO" / "FROM-", empty = whole source
    juce::TextButton fetchButton { "GRAB" };
    IconButton gearButton { IconButton::Icon::gear };   // settings (theme, tools, audio); red dot = update/error
    std::unique_ptr<juce::Drawable> lockup;        // logo per theme, nullptr -> text
    IconButton playButton { IconButton::Icon::play };
    IconButton stopButton { IconButton::Icon::stop };
    IconButton loopButton { IconButton::Icon::loop };
    juce::AudioProcessorValueTreeState::ButtonAttachment loopAttachment;   // declared after loopButton -> destroyed before it
    juce::TextButton gridButton { "GRID" };
    juce::TextButton beatButton { "BEAT" };
    juce::TextButton transientButton { "TRANSIENT" };
    IconButton backButton { IconButton::Icon::back };     // back to the state before the last load (sample, trim, slice mode, tempo octave)
    juce::TextButton bpmHalfButton { "/2" };      // tempo octave: half tempo
    juce::TextButton bpmDoubleButton { "x2" };    // tempo octave: double tempo

    WaveformView waveform;
    SliceGrid pads;
    DragHandle dragHandle;

    SettingsOverlay settings;                       // on top of everything, visible only when open (added after the other children)

    KnobCell gainCell, pitchCell, speedCell, fadeInCell, fadeOutCell, shapeCell;
    static constexpr int numKnobCells = 6;   // knob row: layout (resized) and divider lines (paint)

    juce::Rectangle<int> rangeR;                   // range box inside the URL row (label + field)
    juce::Rectangle<int> headerR, tabsR, urlR, progR, waveLabelR, detLabelR, detR, slicesLabelR, paramsR, footerR, infoR;

    juce::String hoverHint;                        // hint for the transport button under the mouse (instead of a tooltip)
    void setHoverHint (const juce::String& t);
    int activeSource = 0;
    int tick = 0;
    int lastActivePad = -2;
    int lastAnalysisVersion = -1;
    float lastFadeIn = -1.0f, lastFadeOut = -1.0f, lastFadeShape = -1000.0f;
    float selectionPeak = 0.0f;                    // peak level of the selection (linear, without gain), for the overload indicator
    juce::int64 lastPeakKey = -1;                  // basis of selectionPeak (processor.getPeakKey())
    juce::String statusText { "READY" };
    juce::String lastAudioInfo;
    ui::ThemeId shownTheme = ui::theme();
    float progress = 0.0f;
    bool busy = false, isError = false, wasPlaying = false, dragOver = false, lastRendering = false;
    bool toolsBusy = false;                        // status line/progress currently belong to the ToolManager
    int lastToolRun = 0;                           // ToolManager::getRunCounter() at the last sync
    int lastStateVersion = 0;                      // processor.getStateVersion() already shown
    int lastEmbedWarn = 0;                         // processor.getEmbedWarnVersion() already shown
    juce::String pendingGrabUrl;                   // link of the running grab (saved with the sample for re-grabbing)
    juce::String pendingGrabRange;                 // its range text (sg::range::toText), empty = whole source
    juce::String pendingGrabTitle;                 // its real title from the probe (display only), empty = from the file name
    juce::String snapshotTitle;                    // SG_SNAPSHOT_TITLE (developer aid): display title for a loaded file
    juce::String grabNote;                         // appended to the progress phase (e.g. LENGTH UNKNOWN)
    juce::String autoRangeText;                    // range text last filled in from a link (t=...), replaced on URL change
    GrabSpec confirmSpec;                          // long source waiting for GRAB ALL
    bool confirmPending = false;
    int inputSerial = 0;                           // +1 on every URL/range edit
    int jobSerial = 0;                             // +1 per cancel: results of a cancelled job are ignored
    bool probing = false;                          // length check running (watchdog)
    juce::uint32 probeStartMs = 0;
    juce::String lastAutoWant;                     // start time last taken from a link (prefill once per value)
    juce::File pendingDelete;                      // KEEP DOWNLOADS off: delete once the embedded FLAC is settled

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleGrabberEditor)
};
