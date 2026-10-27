#pragma once
#include "PluginProcessor.h"
#include "Downloader.h"
#include "ui/SGLookAndFeel.h"
#include "WaveformView.h"
#include "Widgets.h"
#include "KnobCell.h"
#include "IconButton.h"
#include "SettingsOverlay.h"

class SampleGrabberEditor : public juce::AudioProcessorEditor,
                            public juce::FileDragAndDropTarget,
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

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;   // ToolManager: progress/result
    void syncTools();
    void onFetchClicked();
    void startGrab();
    void beginBusy (const juce::String& status);
    void loadFromFile (const juce::File&);
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleGrabberEditor)
};
