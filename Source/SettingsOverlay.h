#pragma once
#include "PluginProcessor.h"
#include "IconButton.h"
#include "ToolManager.h"
#include "ui/SGLookAndFeel.h"
#include <functional>

// Settings overlay (gear in the header): child component on top of the whole editor,
// dimmed background (scrim) and a panel in the style of the DETECTED panel. No popup/window (WSLg).
// Sections: APPEARANCE (theme), TOOLS (ToolManager), AUDIO (standalone only).
// When closed the component is invisible and intercepts nothing. Message thread only.
class SettingsOverlay : public juce::Component,
                        private juce::ChangeListener
{
public:
    explicit SettingsOverlay (SampleGrabberProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (true);
        setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);   // Tab stays inside the panel

        closeButton.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeButton);

        for (auto* b : { &darkButton, &classicButton })
        {
            b->setClickingTogglesState (true);
            b->setRadioGroupId (2001);
            b->getProperties().set ("outline", true);
            addAndMakeVisible (*b);
        }
        darkButton.onClick    = [this] { if (onSetTheme) onSetTheme (ui::ThemeId::dark); };
        classicButton.onClick = [this] { if (onSetTheme) onSetTheme (ui::ThemeId::classic); };

        for (auto* b : { &checkButton, &updateButton, &autoButton, &openButton, &audioButton })
        {
            b->getProperties().set ("outline", true);
            addAndMakeVisible (*b);
        }

        checkButton.onClick = [this] { tools->checkForUpdates(); refreshTools(); };   // without the 24 h limit
        updateButton.onClick = [this]
        {
            if (tools->anyMissing())
                tools->installMissing();   // also deno, if yt-dlp/ffmpeg come from PATH
            else
                tools->updateAll (false);
            refreshTools();
        };

        autoButton.setClickingTogglesState (true);
        autoButton.onClick = [this]
        {
            ToolManager::setAutoUpdateYtDlp (autoButton.getToggleState());
            autoButton.setButtonText (autoText());
        };

        openButton.onClick = [this] { openFolder(); };

        audioButton.onClick = [this]
        {
            if (onClose) onClose();
            if (proc.onOpenAudioSettings) proc.onOpenAudioSettings();
        };

        tools->addChangeListener (this);
        themeChanged();
    }

    ~SettingsOverlay() override
    {
        tools->removeChangeListener (this);
    }

    std::function<void()> onClose;
    std::function<void (ui::ThemeId)> onSetTheme;

    // Before showing: fetch fresh state (disk state, setting, standalone yes/no)
    void prepareToShow()
    {
        hasAudio = proc.onOpenAudioSettings != nullptr;   // same decision as the former AUDIO button
        audioButton.setVisible (hasAudio);
        autoButton.setToggleState (ToolManager::isAutoUpdateYtDlp(), juce::dontSendNotification);
        autoButton.setButtonText (autoText());
        localNote.clear();
        tools->refresh();
        refreshTools();
        resized();
    }

    // Theme changed (also by another instance): update switch and colours
    void themeChanged()
    {
        (ui::theme() == ui::ThemeId::dark ? darkButton : classicButton).setToggleState (true, juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        const int w = 600;
        const int h = 16 + 24 + 12                         // top margin, title
                    + 16 + 6 + 22 + 20                     // APPEARANCE
                    + 16 + 6 + 3 * rowH + 8 + 22 + 6 + 16 + 8 + 22 + 20   // TOOLS
                    + (hasAudio ? 16 + 6 + 22 + 20 : 0)    // AUDIO
                    + 16 + 16;                             // licence note, bottom margin
        panelR = getLocalBounds().withSizeKeepingCentre (w, h);

        auto r = panelR.reduced (16);
        titleR = r.removeFromTop (24);
        closeButton.setBounds (titleR.removeFromRight (24).withSizeKeepingCentre (24, 24));
        r.removeFromTop (12);

        appLabelR = r.removeFromTop (16);
        r.removeFromTop (6);
        auto ar = r.removeFromTop (22);
        darkButton.setBounds (ar.removeFromLeft (72));
        ar.removeFromLeft (4);
        classicButton.setBounds (ar.removeFromLeft (72));
        r.removeFromTop (20);

        toolsLabelR = r.removeFromTop (16);
        r.removeFromTop (6);
        rowsR = r.removeFromTop (3 * rowH);
        r.removeFromTop (8);
        auto br = r.removeFromTop (22);
        checkButton.setBounds (br.removeFromLeft (88));
        br.removeFromLeft (4);
        updateButton.setBounds (br.removeFromLeft (88));
        autoButton.setBounds (br.removeFromRight (176));
        lastCheckR = br.withTrimmedLeft (12).withTrimmedRight (12);
        r.removeFromTop (6);
        toolStatusR = r.removeFromTop (16);   // running operation or last result
        r.removeFromTop (8);
        folderR = r.removeFromTop (22);
        openButton.setBounds (folderR.removeFromRight (64));
        r.removeFromTop (20);

        if (hasAudio)
        {
            audioLabelR = r.removeFromTop (16);
            r.removeFromTop (6);
            audioButton.setBounds (r.removeFromTop (22).removeFromLeft (152));
            r.removeFromTop (20);
        }
        else
        {
            audioLabelR = {};
        }

        footerR = r.removeFromTop (16);
    }

    void paint (juce::Graphics& g) override
    {
        // Scrim: editor below dimmed; panel like DETECTED (surface1, hairline)
        g.setColour (ui::c().canvas.withAlpha (0.82f));
        g.fillAll();
        g.setColour (ui::c().surface1);
        g.fillRect (panelR);
        g.setColour (ui::c().hairline);
        g.drawRect (panelR, 1);

        g.setColour (ui::c().textPrimary);
        g.setFont (ui::strong (11.0f));
        g.drawText ("SETTINGS", titleR, juce::Justification::centredLeft);
        g.setColour (ui::c().hairline);
        g.fillRect (panelR.getX() + 16, titleR.getBottom() + 6, panelR.getWidth() - 32, 1);

        g.setColour (ui::c().textSecondary);
        g.setFont (ui::label (10.0f));
        g.drawText ("APPEARANCE", appLabelR, juce::Justification::centredLeft);
        g.drawText ("TOOLS", toolsLabelR, juce::Justification::centredLeft);
        if (hasAudio)
            g.drawText ("AUDIO", audioLabelR, juce::Justification::centredLeft);

        paintTools (g);

        // Folder
        g.setColour (ui::c().textSecondary);
        g.setFont (ui::label (10.0f));
        g.drawText ("FOLDER", folderR.withWidth (56), juce::Justification::centredLeft);
        g.setColour (ui::c().textPrimary);
        g.setFont (ui::monoUi (11.0f));
        g.drawFittedText (shortPath (ToolManager::toolsDir()), folderR.withTrimmedLeft (56).withTrimmedRight (8),
                          juce::Justification::centredLeft, 1, 1.0f);

        g.setColour (ui::c().textTertiary);
        g.setFont (ui::label (10.0f));
        g.drawText ("TOOLS FROM OFFICIAL GITHUB RELEASES: YT-DLP (UNLICENSE / GPLV3 BINARY), DENO (MIT), FFMPEG (LGPL)",
                    footerR, juce::Justification::centredLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! panelR.contains (e.getPosition()) && onClose)
            onClose();   // click on the scrim closes
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey)
        {
            if (onClose) onClose();
            return true;
        }
        return true;   // do not pass keys on to the editor below while open
    }

private:
    static constexpr int rowH = 24;

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        localNote.clear();   // a tool event replaces the local note (e.g. PATH COPIED)
        if (isVisible())
            refreshTools();
    }

    // Create the folder only on click, then open it in the file manager.
    // Linux: xdg-open as a separate process with an argument list (no shell); if missing (WSLg without desktop tools),
    // copy the path to the clipboard and show that.
    void openFolder()
    {
        const auto dir = ToolManager::toolsDir();
        dir.createDirectory();

       #if JUCE_LINUX
        juce::File xdg;
        for (auto d : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), ":", {}))
            if (juce::File::isAbsolutePath (d) && juce::File (d).getChildFile ("xdg-open").existsAsFile())
            {
                xdg = juce::File (d).getChildFile ("xdg-open");
                break;
            }

        bool started = false;
        if (xdg != juce::File())
        {
            opener = std::make_unique<juce::ChildProcess>();
            started = opener->start (juce::StringArray { xdg.getFullPathName(), dir.getFullPathName() }, 0);
        }
        if (! started)
        {
            juce::SystemClipboard::copyTextToClipboard (dir.getFullPathName());
            localNote = "NO FILE MANAGER FOUND - PATH COPIED";
            repaint();
        }
       #else
        dir.startAsProcess();
       #endif
    }

    void refreshTools()
    {
        const bool working = tools->isWorking();
        checkButton.setEnabled (! working);
        updateButton.setEnabled (! working);
        updateButton.setButtonText (tools->anyMissing() ? "INSTALL" : "UPDATE");
        repaint();
    }

    juce::String autoText() const
    {
        return juce::String ("AUTO-UPDATE YT-DLP: ") + (autoButton.getToggleState() ? "ON" : "OFF");
    }


    static juce::String shortPath (const juce::File& f)
    {
        auto p = f.getFullPathName();
       #if ! JUCE_WINDOWS
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
        if (home.length() > 1 && (p == home || p.startsWith (home + "/")))
            p = "~" + p.substring (home.length());
       #endif
        return p;
    }

    void paintTools (juce::Graphics& g)
    {
        const char* const repos[] = { "YT-DLP/YT-DLP", "DENOLAND/DENO", "BTBN/FFMPEG-BUILDS" };
        const bool working = tools->showsProgress();
        const float p = tools->getProgress();

        for (int i = 0; i < ToolManager::numTools; ++i)
        {
            const auto t = (ToolManager::Tool) i;
            const auto in = tools->getInfo (t);
            auto r = rowsR.withY (rowsR.getY() + i * rowH).withHeight (rowH);

            g.setColour (ui::c().hairline);
            g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
            if (i == ToolManager::numTools - 1)
                g.fillRect (r.getX(), r.getBottom() - 1, r.getWidth(), 1);

            const auto nameR = r.removeFromLeft (76);
            const auto verR = r.removeFromLeft (124);
            const auto srcR = r.removeFromLeft (148);
            const auto stateR = r;

            g.setColour (ui::c().textPrimary);
            g.setFont (ui::label (10.0f));
            g.drawText (ToolManager::toolName (t).toUpperCase(), nameR, juce::Justification::centredLeft);

            const bool fromPath = ! in.own && in.path != juce::File();
            const bool missing = in.path == juce::File();
            g.setColour (missing ? ui::c().textSecondary : ui::c().textPrimary);
            g.setFont (missing || fromPath ? ui::label (10.0f) : ui::monoUi (11.0f));
            g.drawText (in.own ? in.version : (fromPath ? juce::String ("FROM PATH") : juce::String ("NOT INSTALLED")),
                        verR, juce::Justification::centredLeft);

            // Source: own installation/missing -> GitHub repo, otherwise the PATH folder
            g.setColour (ui::c().textSecondary);
            if (fromPath)
            {
                g.setFont (ui::monoUi (10.0f));
                g.drawFittedText (shortPath (in.path.getParentDirectory()), srcR.withTrimmedRight (8),
                                  juce::Justification::centredLeft, 1, 1.0f);
            }
            else
            {
                g.setFont (ui::label (10.0f));
                g.drawText (repos[i], srcR, juce::Justification::centredLeft);
            }

            juce::String state;
            juce::Colour col = ui::c().textSecondary;
            if (in.state == ToolManager::State::working)
            {
                state = working && p >= 0.0f ? "INSTALLING " + juce::String ((int) std::lround (p * 100.0f)) + "%"
                                             : tools->getStatusText().upToFirstOccurrenceOf (" ", false, false);
                if (state.isEmpty())
                    state = "WORKING";
                col = ui::c().red;   // progress
            }
            else if (in.failed)
            {
                state = "ERROR: " + in.message.upToFirstOccurrenceOf (" - ", false, false);
                col = ui::c().red;
            }
            else if (in.own && in.updateAvailable)
            {
                state = "UPDATE AVAILABLE " + in.latest;
                col = ui::c().textPrimary;
            }
            else if (in.own)
            {
                state = in.latest.isNotEmpty() ? "UP TO DATE" : "INSTALLED";
            }
            else if (fromPath)
            {
                state = "NOT MANAGED";
            }
            else
            {
                state = "MISSING";
            }

            g.setColour (col);
            g.setFont (ui::monoUi (10.0f));
            g.drawFittedText (state, stateR, juce::Justification::centredRight, 1, 1.0f);
        }

        // Running operation or last result
        juce::String line;
        bool err = false;
        if (localNote.isNotEmpty())
            line = localNote;
        else if (working)
            line = tools->getStatusText() + (p >= 0.0f ? " " + juce::String ((int) std::lround (p * 100.0f)) + "%" : juce::String());
        else if (tools->isWorking())
            line = "CHECKING";
        else
        {
            line = tools->getLastNote();
            err = tools->lastNoteIsError();
        }
        g.setColour (err ? ui::c().red : ui::c().textSecondary);
        g.setFont (ui::monoUi (10.0f));
        g.drawFittedText (line, toolStatusR, juce::Justification::centredLeft, 1, 1.0f);

        // Last check
        const auto last = tools->getLastCheck();
        juce::String lc = "LAST CHECK ";
        if (last <= 0)
            lc << "NEVER";
        else
        {
            const juce::Time t (last);
            const auto now = juce::Time::getCurrentTime();
            const bool today = t.getYear() == now.getYear() && t.getDayOfYear() == now.getDayOfYear();
            lc << t.formatted (today ? "%H:%M" : "%d.%m. %H:%M");
        }
        g.setColour (ui::c().textSecondary);
        g.setFont (ui::monoUi (10.0f));
        g.drawText (lc, lastCheckR, juce::Justification::centredLeft);
    }

    SampleGrabberProcessor& proc;
    juce::SharedResourcePointer<ToolManager> tools;

    IconButton closeButton { IconButton::Icon::close };
    juce::TextButton darkButton { "DARK" }, classicButton { "CLASSIC" };
    juce::TextButton checkButton { "CHECK NOW" }, updateButton { "UPDATE" };
    juce::TextButton autoButton { "AUTO-UPDATE YT-DLP: ON" };
    juce::TextButton openButton { "OPEN" };
    juce::TextButton audioButton { "AUDIO / MIDI SETUP" };

    bool hasAudio = false;
    juce::String localNote;                         // note from the overlay itself (OPEN), until the next tool event
    std::unique_ptr<juce::ChildProcess> opener;     // xdg-open; object stays until the next click (no kill)
    juce::Rectangle<int> panelR, titleR, appLabelR, toolsLabelR, rowsR, toolStatusR, lastCheckR, folderR, audioLabelR, footerR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsOverlay)
};
