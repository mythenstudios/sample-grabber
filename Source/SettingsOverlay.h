// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "PluginProcessor.h"
#include "IconButton.h"
#include "ToolManager.h"
#include "Storage.h"
#include "ui/SGLookAndFeel.h"
#include <functional>

// Settings overlay (gear in the header): child component on top of the whole editor,
// dimmed background (scrim) and a panel in the style of the DETECTED panel. No popup/window (WSLg).
// Sections: APPEARANCE (theme), TOOLS (ToolManager), STORAGE (cache, exports, tools folder), AUDIO (standalone only).
// Folder CHANGE: inline text field in the row (no popup, WSLg); Windows/macOS also open the native folder chooser.
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

        for (auto* b : { &checkButton, &updateButton, &autoButton, &audioButton })
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

        // --- STORAGE ---
        for (auto* b : { &cacheOpenButton, &cacheClearButton, &exportOpenButton, &cacheChangeButton, &exportChangeButton,
                         &toolsOpenButton, &removeToolsButton, &editBrowseButton, &editDefaultButton, &editOkButton,
                         &editCancelButton })
        {
            b->getProperties().set ("outline", true);
            addAndMakeVisible (*b);
        }
        cacheOpenButton.onClick  = [this] { openFolder (sg::storage::cacheRoot(), storageNote); };
        exportOpenButton.onClick = [this] { openFolder (sg::storage::exportRoot(), storageNote); };
        toolsOpenButton.onClick  = [this] { openFolder (ToolManager::toolsDir(), storageNote); };
        cacheChangeButton.onClick  = [this] { beginEdit (sg::storage::Folder::cache); };
        exportChangeButton.onClick = [this] { beginEdit (sg::storage::Folder::exports); };
        editOkButton.onClick       = [this] { applyFolder (editField.getText()); };
        editCancelButton.onClick   = [this] { endEdit(); };
        editBrowseButton.onClick   = [this] { browseFolder(); };
        editDefaultButton.onClick  = [this] { resetFolder(); };

        editField.setMultiLine (false);
        editField.setReturnKeyStartsNewLine (false);
        editField.setPopupMenuEnabled (false);   // no popup menus (WSLg)
        editField.setScrollbarsShown (false);
        editField.setJustification (juce::Justification::centredLeft);
        editField.setBorder (juce::BorderSize<int> (0));
        editField.setIndents (6, 0);   // vertical centring by the justification, text level with the other rows
        editField.onReturnKey = [this] { applyFolder (editField.getText()); };
        editField.onEscapeKey = [this] { endEdit(); };
        editField.onTextChange = [this] { if (storageNote.isNotEmpty()) { storageNote.clear(); repaint (storageHintR); } };
        addChildComponent (editField);

        // REMOVE TOOLS: same two-step confirm as CLEAR NOW
        removeToolsButton.onClick = [this]
        {
            if (! removeArmed)
            {
                removeArmed = true;
                removeToolsButton.setButtonText ("CONFIRM REMOVE?");
                const int token = ++removeToken;
                const juce::Component::SafePointer<SettingsOverlay> safe (this);
                juce::Timer::callAfterDelay (3000, [safe, token]
                {
                    if (safe != nullptr && safe->removeToken == token)
                        safe->disarmRemove();
                });
                return;
            }
            disarmRemove();
            if (isGrabBusy != nullptr && isGrabBusy())
                storageNote = "A DOWNLOAD IS RUNNING - REMOVE THE TOOLS AFTER IT";
            else
            {
                storageNote.clear();
                tools->removeOwnTools();   // result in the TOOLS status line and the main status line
            }
            refreshTools();
        };
        // Two-step confirm inside the button (no popup): first click arms for 3 s, second click clears
        cacheClearButton.onClick = [this]
        {
            if (! clearArmed)
            {
                clearArmed = true;
                cacheClearButton.setButtonText ("CONFIRM CLEAR?");
                const int token = ++clearToken;
                // Built outside the capture list: MSVC rejects 'this' in an init-capture of a nested lambda
                const juce::Component::SafePointer<SettingsOverlay> safe (this);
                juce::Timer::callAfterDelay (3000, [safe, token]
                {
                    if (safe != nullptr && safe->clearToken == token)
                        safe->disarmClear();
                });
                return;
            }
            disarmClear();
            storageNote.clear();
            cache->requestClearNow();
            repaint();
        };

        for (auto* b : { &cleanOffButton, &clean7Button, &cleanMaxButton })
        {
            b->setClickingTogglesState (true);
            b->setRadioGroupId (2002);
            b->getProperties().set ("outline", true);
            addAndMakeVisible (*b);
        }
        auto setClean = [this] (sg::storage::AutoClean m)
        {
            sg::storage::setAutoClean (m);
            storageNote.clear();
            if (m != sg::storage::AutoClean::off)
                cache->requestAutoClean();   // a new limit applies right away
        };
        cleanOffButton.onClick = [setClean] { setClean (sg::storage::AutoClean::off); };
        clean7Button.onClick   = [setClean] { setClean (sg::storage::AutoClean::days7); };
        cleanMaxButton.onClick = [setClean] { setClean (sg::storage::AutoClean::maxSize); };

        // KEEP DOWNLOADS: ON = cache keeps grabbed/converted files; OFF = deleted once loaded into RAM
        keepButton.setClickingTogglesState (true);
        keepButton.getProperties().set ("outline", true);
        keepButton.onClick = [this]
        {
            sg::storage::setKeepDownloads (keepButton.getToggleState());
            keepButton.setButtonText (keepText());
            repaint (keepR);
        };
        addAndMakeVisible (keepButton);

        audioButton.onClick = [this]
        {
            if (onClose) onClose();
            if (proc.onOpenAudioSettings) proc.onOpenAudioSettings();
        };

        tools->addChangeListener (this);
        cache->addChangeListener (this);
        themeChanged();
    }

    ~SettingsOverlay() override
    {
        cache->removeChangeListener (this);
        tools->removeChangeListener (this);
    }

    std::function<void()> onClose;
    std::function<void (ui::ThemeId)> onSetTheme;
    std::function<bool()> isGrabBusy;               // editor: a grab/import of this editor runs (REMOVE TOOLS waits)

    // Before showing: fetch fresh state (disk state, setting, standalone yes/no)
    void prepareToShow()
    {
        hasAudio = proc.onOpenAudioSettings != nullptr;   // same decision as the former AUDIO button
        audioButton.setVisible (hasAudio);
        autoButton.setToggleState (ToolManager::isAutoUpdateYtDlp(), juce::dontSendNotification);
        autoButton.setButtonText (autoText());
        keepButton.setToggleState (sg::storage::getKeepDownloads(), juce::dontSendNotification);
        keepButton.setButtonText (keepText());
        localNote.clear();
        storageNote.clear();
        disarmClear();
        disarmRemove();
        endEdit();
        refreshPaths();
        tools->refresh();
        refreshTools();
        const auto mode = sg::storage::getAutoClean();
        (mode == sg::storage::AutoClean::off ? cleanOffButton : mode == sg::storage::AutoClean::days7 ? clean7Button : cleanMaxButton)
            .setToggleState (true, juce::dontSendNotification);
        cache->requestSize();   // background thread, result via change message
        resized();
    }

    // SG_SNAPSHOT_SETTINGS=cache|exports (developer aid): open the folder CHANGE row (no native chooser, no saving)
    void showFolderEditForSnapshot (bool cacheRow)
    {
        editing = cacheRow ? sg::storage::Folder::cache : sg::storage::Folder::exports;
        isEditing = true;
        editField.setText ((cacheRow ? cacheNext : exportShown).getFullPathName(), false);
        layoutEdit();
        repaint();
    }

    // Theme changed (also by another instance): update switch and colours
    void themeChanged()
    {
        (ui::theme() == ui::ThemeId::dark ? darkButton : classicButton).setToggleState (true, juce::dontSendNotification);
        editField.setColour (juce::TextEditor::backgroundColourId, ui::c().surface1);
        editField.applyFontToAllText (ui::monoUi (11.0f));   // also the font for new text
        editField.applyColourToAllText (ui::c().textPrimary);
        repaint();
    }

    void resized() override
    {
        const int w = 600;   // wider would run into the gear button (stays on top, top right of the editor)
        const int h = 16 + 24 + 12                         // top margin, title
                    + 16 + 6 + 22 + 20                     // APPEARANCE
                    + 16 + 6 + 3 * rowH + 8 + 22 + 6 + 16 + 20   // TOOLS
                    + 16 + 6 + 5 * 22 + 5 * 6 + 16 + 20    // STORAGE (5 rows + note line)
                    + (hasAudio ? 16 + 6 + 22 + 20 : 0)    // AUDIO
                    + 16 + 4 + 16 + 16;                    // licence note, about line, bottom margin
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
        r.removeFromTop (20);

        // STORAGE: [CACHE path size  OPEN CHANGE CLEAR NOW] [AUTO-CLEAN OFF 7 DAYS 1 GB MAX  note] [DOWNLOADS KEEP]
        //          [EXPORTS path  OPEN CHANGE] [TOOLS path  OPEN REMOVE TOOLS] [note line]
        // Button columns line up: OPEN 56 | CHANGE 64 | CLEAR NOW 112 (REMOVE TOOLS spans CHANGE + CLEAR NOW)
        storageLabelR = r.removeFromTop (16);
        r.removeFromTop (6);
        cacheR = r.removeFromTop (22);
        cacheClearButton.setBounds (cacheR.removeFromRight (112));   // room for "CONFIRM CLEAR?"
        cacheR.removeFromRight (4);
        cacheChangeButton.setBounds (cacheR.removeFromRight (64));
        cacheR.removeFromRight (4);
        cacheOpenButton.setBounds (cacheR.removeFromRight (56));
        cacheSizeR = cacheR.removeFromRight (64).withTrimmedRight (8);
        r.removeFromTop (6);
        cleanR = r.removeFromTop (22);
        {
            auto cr = cleanR.withTrimmedLeft (labelW);
            cleanOffButton.setBounds (cr.removeFromLeft (48));
            cr.removeFromLeft (4);
            clean7Button.setBounds (cr.removeFromLeft (64));
            cr.removeFromLeft (4);
            cleanMaxButton.setBounds (cr.removeFromLeft (80));
            cleanNoteR = cr.withTrimmedLeft (12);
        }
        r.removeFromTop (6);
        keepR = r.removeFromTop (22);
        {
            auto kr = keepR.withTrimmedLeft (labelW);
            keepButton.setBounds (kr.removeFromLeft (148));
            keepNoteR = kr.withTrimmedLeft (12);
        }
        r.removeFromTop (6);
        exportR = r.removeFromTop (22);
        exportR.removeFromRight (112 + 4);
        exportChangeButton.setBounds (exportR.removeFromRight (64));
        exportR.removeFromRight (4);
        exportOpenButton.setBounds (exportR.removeFromRight (56));
        r.removeFromTop (6);
        toolsR = r.removeFromTop (22);
        removeToolsButton.setBounds (toolsR.removeFromRight (112 + 4 + 64));
        toolsR.removeFromRight (4);
        toolsOpenButton.setBounds (toolsR.removeFromRight (56));
        r.removeFromTop (6);
        storageHintR = r.removeFromTop (16);
        r.removeFromTop (20);

        layoutEdit();

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
        r.removeFromTop (4);
        aboutR = r.removeFromTop (16);
        sourceR = aboutR.withX (aboutR.getX() + (int) std::ceil (aboutPrefixWidth())).withWidth ((int) std::ceil (sourceWidth()));
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
        g.drawText ("STORAGE", storageLabelR, juce::Justification::centredLeft);
        if (hasAudio)
            g.drawText ("AUDIO", audioLabelR, juce::Justification::centredLeft);

        paintTools (g);

        paintStorage (g);

        g.setColour (ui::c().textTertiary);
        g.setFont (ui::label (10.0f));
        g.drawText ("TOOLS FROM OFFICIAL GITHUB RELEASES: YT-DLP (UNLICENSE / GPLV3 BINARY), DENO (MIT), FFMPEG (LGPL)",
                    footerR, juce::Justification::centredLeft);

        // About line: version, copyright, licence, SOURCE link (hit area = only the word SOURCE)
        g.setFont (ui::label (10.0f));
        g.drawText (aboutPrefix(), aboutR, juce::Justification::centredLeft, false);
        g.setColour (sourceHover ? ui::c().red : ui::c().textSecondary);
        g.drawText ("SOURCE", sourceR, juce::Justification::centredLeft, false);
        g.fillRect (sourceR.getX(), sourceR.getBottom() - 3, sourceR.getWidth(), 1);   // underline = link
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool over = sourceR.contains (e.getPosition());
        if (over != sourceHover)
        {
            sourceHover = over;
            repaint (sourceR);
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (sourceHover)
        {
            sourceHover = false;
            repaint (sourceR);
        }
    }

    juce::MouseCursor getMouseCursor() override
    {
        return sourceR.contains (getMouseXYRelative()) && isVisible() ? juce::MouseCursor::PointingHandCursor
                                                                       : juce::MouseCursor::NormalCursor;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (sourceR.contains (e.getPosition()))
        {
            juce::URL ("https://github.com/mythenstudios/sample-grabber").launchInDefaultBrowser();
            return;
        }
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

    static constexpr int labelW = 84;   // label column of the STORAGE rows

    // Plugin target: JucePlugin_VersionString, standalone target: JUCE_APPLICATION_VERSION_STRING (both from CMake VERSION)
    static juce::String versionText()
    {
       #if defined (JucePlugin_VersionString)
        return JucePlugin_VersionString;
       #elif defined (JUCE_APPLICATION_VERSION_STRING)
        return JUCE_APPLICATION_VERSION_STRING;
       #else
        return {};
       #endif
    }

    // "SAMPLE GRABBER 0.1.0 · © MYTHEN STUDIOS · AGPL-3.0 · " (middle dot and copyright sign as UTF-8 bytes)
    static juce::String aboutPrefix()
    {
        const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");
        return "SAMPLE GRABBER " + versionText() + dot + juce::String::fromUTF8 ("\xc2\xa9") + " MYTHEN STUDIOS" + dot + "AGPL-3.0" + dot;
    }

    static float aboutPrefixWidth() { return juce::GlyphArrangement::getStringWidth (ui::label (10.0f), aboutPrefix()); }
    static float sourceWidth()      { return juce::GlyphArrangement::getStringWidth (ui::label (10.0f), "SOURCE"); }

    void disarmClear()
    {
        clearArmed = false;
        ++clearToken;
        cacheClearButton.setButtonText ("CLEAR NOW");
    }

    void disarmRemove()
    {
        removeArmed = false;
        ++removeToken;
        removeToolsButton.setButtonText ("REMOVE TOOLS");
    }

    // Paths shown in STORAGE: read when opening and after a change, not on every repaint (settings.xml/registry)
    void refreshPaths()
    {
        cacheShown = sg::storage::cacheRoot();
        cacheNext = sg::storage::folderFromEnv (sg::storage::Folder::cache) ? cacheShown
                                                                            : sg::storage::resolveFolder (sg::storage::Folder::cache);
        exportShown = sg::storage::exportRoot();
        cacheChangeButton.setEnabled (! sg::storage::folderFromEnv (sg::storage::Folder::cache));    // env var wins anyway
        exportChangeButton.setEnabled (! sg::storage::folderFromEnv (sg::storage::Folder::exports));
        repaint();
    }

    // --- Folder CHANGE: the row turns into [label][text field][BROWSE][DEFAULT][OK][CANCEL] ---
    void beginEdit (sg::storage::Folder f)
    {
        disarmClear();
        editing = f;
        isEditing = true;
        storageNote.clear();
        editField.setText ((f == sg::storage::Folder::cache ? cacheNext : exportShown).getFullPathName(), false);
        layoutEdit();
        editField.grabKeyboardFocus();
        editField.selectAll();
        repaint();
       #if JUCE_WINDOWS || JUCE_MAC
        browseFolder();   // native chooser right away; cancelled -> the text field stays for typing/DEFAULT
       #endif
    }

    void endEdit()
    {
        isEditing = false;
        chooser.reset();
        layoutEdit();
        if (isShowing())
            grabKeyboardFocus();   // Escape closes the overlay again
        repaint();
    }

    void layoutEdit()
    {
        const bool editCache = isEditing && editing == sg::storage::Folder::cache;
        const bool editExport = isEditing && editing == sg::storage::Folder::exports;
        for (auto* b : { &cacheOpenButton, &cacheChangeButton, &cacheClearButton })
            b->setVisible (! editCache);
        for (auto* b : { &exportOpenButton, &exportChangeButton })
            b->setVisible (! editExport);
        editField.setVisible (isEditing);
       #if JUCE_WINDOWS || JUCE_MAC
        editBrowseButton.setVisible (isEditing);
       #else
        editBrowseButton.setVisible (false);
       #endif
        for (auto* b : { &editDefaultButton, &editOkButton, &editCancelButton })
            b->setVisible (isEditing);
        if (! isEditing)
            return;

        // Whole row right of the label (the cache size column too)
        auto row = (editCache ? cacheR.withRight (cacheClearButton.getRight()) : exportR.withRight (cacheClearButton.getRight()))
                       .withTrimmedLeft (labelW);
        editCancelButton.setBounds (row.removeFromRight (64));
        row.removeFromRight (4);
        editOkButton.setBounds (row.removeFromRight (40));
        row.removeFromRight (4);
        editDefaultButton.setBounds (row.removeFromRight (68));
        row.removeFromRight (4);
        if (editBrowseButton.isVisible())
        {
            editBrowseButton.setBounds (row.removeFromRight (68));
            row.removeFromRight (4);
        }
        editR = row.withTrimmedRight (8);
        editField.setBounds (editR.reduced (1));
    }

    void applyFolder (const juce::String& typed)
    {
        if (! isEditing)
            return;
        const auto f = editing;
        juce::File dir;
        const auto r = sg::storage::checkChosenFolder (f, typed, dir);
        if (r.failed())
        {
            storageNote = r.getErrorMessage();
            repaint();
            return;
        }
        if (! sg::storage::setStoredFolder (f, dir.getFullPathName()))
        {
            storageNote = "COULD NOT SAVE THE SETTING";
            repaint();
            return;
        }
        endEdit();
        refreshPaths();
        if (f == sg::storage::Folder::exports)
            storageNote = "EXPORT FOLDER CHANGED - NEW DRAGS GO THERE";
        else if (dir != cacheShown)
            storageNote = dir.getParentDirectory() != juce::File (typed.trim().unquoted().trim())
                              ? "CACHE FOLDER SAVED - APPLIES AFTER A RESTART"
                              : "FOLDER NOT EMPTY - USING A SUBFOLDER, APPLIES AFTER A RESTART";
        repaint();
    }

    void resetFolder()
    {
        if (! isEditing)
            return;
        const auto f = editing;
        if (! sg::storage::setStoredFolder (f, {}))
        {
            storageNote = "COULD NOT SAVE THE SETTING";
            repaint();
            return;
        }
        endEdit();
        refreshPaths();
        storageNote = f == sg::storage::Folder::exports ? juce::String ("EXPORT FOLDER BACK TO DEFAULT")
                    : cacheNext != cacheShown ? juce::String ("CACHE BACK TO DEFAULT AFTER A RESTART")
                                              : juce::String ("CACHE FOLDER IS THE DEFAULT");
        repaint();
    }

    // Native folder chooser (Windows/macOS). Picking a folder applies it at once.
    void browseFolder()
    {
        if (! isEditing)
            return;
        auto start = juce::File::isAbsolutePath (editField.getText().trim()) ? juce::File (editField.getText().trim()) : juce::File();
        while (start != juce::File() && ! start.isDirectory() && start.getParentDirectory() != start)
            start = start.getParentDirectory();
        chooser = std::make_unique<juce::FileChooser> (editing == sg::storage::Folder::cache ? "Choose the cache folder"
                                                                                             : "Choose the export folder",
                                                       start);
        const juce::Component::SafePointer<SettingsOverlay> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr)
                                      return;
                                  const auto picked = fc.getResult();
                                  if (picked != juce::File())
                                  {
                                      safe->editField.setText (picked.getFullPathName(), false);
                                      safe->applyFolder (picked.getFullPathName());
                                  }
                              });
    }

    void changeListenerCallback (juce::ChangeBroadcaster* source) override
    {
        if (source == &cache.get())
        {
            if (isVisible())
                repaint();   // size or clean result
            return;
        }
        localNote.clear();   // a tool event replaces the local note (e.g. PATH COPIED)
        if (isVisible())
            refreshTools();
    }

    // Create the folder only on click, then open it in the file manager.
    // Linux: xdg-open as a separate process with an argument list (no shell); if missing (WSLg without desktop tools),
    // copy the path to the clipboard and show that (in the given note line).
    void openFolder (const juce::File& dir, juce::String& note)
    {
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

        // WSL: no Linux file manager, but Windows Explorer can open the folder via its \\wsl.localhost\... path
        if (juce::SystemStats::getEnvironmentVariable ("WSL_DISTRO_NAME", {}).isNotEmpty())
        {
            const juce::File wslpath ("/usr/bin/wslpath");
            juce::File explorer;
            for (auto d : juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), ":", {}))
                if (juce::File::isAbsolutePath (d) && juce::File (d).getChildFile ("explorer.exe").existsAsFile())
                {
                    explorer = juce::File (d).getChildFile ("explorer.exe");
                    break;
                }
            if (explorer == juce::File())
                explorer = juce::File ("/mnt/c/Windows/explorer.exe");

            if (wslpath.existsAsFile() && explorer.existsAsFile())
            {
                juce::ChildProcess conv;
                if (conv.start (juce::StringArray { wslpath.getFullPathName(), "-w", dir.getFullPathName() }))
                {
                    const auto winPath = conv.readAllProcessOutput().trim();
                    if (winPath.isNotEmpty())
                    {
                        opener = std::make_unique<juce::ChildProcess>();
                        started = opener->start (juce::StringArray { explorer.getFullPathName(), winPath }, 0);
                    }
                }
            }
        }

        if (! started && xdg != juce::File())
        {
            opener = std::make_unique<juce::ChildProcess>();
            started = opener->start (juce::StringArray { xdg.getFullPathName(), dir.getFullPathName() }, 0);
        }
        if (! started)
        {
            juce::SystemClipboard::copyTextToClipboard (dir.getFullPathName());
            note = "NO FILE MANAGER FOUND - PATH COPIED";
            repaint();
        }
       #else
        // Windows: ShellExecute with the default verb opens the folder itself in Explorer (revealToUser would select it
        // inside its parent). macOS: opens in Finder.
        if (! dir.startAsProcess())
        {
            juce::SystemClipboard::copyTextToClipboard (dir.getFullPathName());
            note = "COULD NOT OPEN FOLDER - PATH COPIED";
            repaint();
        }
       #endif
    }

    void refreshTools()
    {
        const bool working = tools->isWorking();
        checkButton.setEnabled (! working);
        updateButton.setEnabled (! working);
        removeToolsButton.setEnabled (! working && tools->hasOwnFiles());
        updateButton.setButtonText (tools->anyMissing() ? "INSTALL" : "UPDATE");
        repaint();
    }

    juce::String keepText() const
    {
        return juce::String ("KEEP: ") + (keepButton.getToggleState() ? "ON" : "OFF");
    }

    juce::String autoText() const
    {
        return juce::String ("AUTO-UPDATE YT-DLP: ") + (autoButton.getToggleState() ? "ON" : "OFF");
    }


    // Long paths lose their start, not their end (the last folders are the telling part): ".../SampleGrabber/cache"
    static juce::String elideStart (const juce::Font& font, const juce::String& text, float maxW)
    {
        if (juce::GlyphArrangement::getStringWidth (font, text) <= maxW)
            return text;
        int lo = 1, hi = text.length();   // number of leading characters to drop
        while (lo < hi)
        {
            const int mid = (lo + hi) / 2;
            if (juce::GlyphArrangement::getStringWidth (font, "..." + text.substring (mid)) <= maxW)
                hi = mid;
            else
                lo = mid + 1;
        }
        return "..." + text.substring (lo);
    }

    static juce::String shortPath (const juce::File& f)
    {
        auto p = f.getFullPathName();
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
       #if JUCE_WINDOWS
        // Case-insensitive, backslash separator
        if (home.length() > 1 && (p.equalsIgnoreCase (home) || p.startsWithIgnoreCase (home + "\\")))
            p = "~" + p.substring (home.length());
       #else
        if (home.length() > 1 && (p == home || p.startsWith (home + "/")))
            p = "~" + p.substring (home.length());
       #endif
        return p;
    }

    void paintEditRow (juce::Graphics& g, const char* label, juce::Rectangle<int> row)
    {
        g.setColour (ui::c().textSecondary);
        g.setFont (ui::label (10.0f));
        g.drawText (label, row.removeFromLeft (labelW), juce::Justification::centredLeft);
        g.setColour (editField.hasKeyboardFocus (true) ? ui::c().hairlineStrong : ui::c().hairline);
        g.drawRect (editR, 1);
    }

    void paintStorage (juce::Graphics& g)
    {
        auto pathRow = [&] (const char* label, juce::Rectangle<int> row, const juce::File& dir)
        {
            g.setColour (ui::c().textSecondary);
            g.setFont (ui::label (10.0f));
            g.drawText (label, row.removeFromLeft (labelW), juce::Justification::centredLeft);
            g.setColour (ui::c().textPrimary);
            g.setFont (ui::monoUi (11.0f));
            const auto pr = row.withTrimmedRight (8);
            g.drawText (elideStart (ui::monoUi (11.0f), shortPath (dir), (float) pr.getWidth()), pr,
                        juce::Justification::centredLeft, false);
        };

        const bool editCache = isEditing && editing == sg::storage::Folder::cache;
        const bool editExport = isEditing && editing == sg::storage::Folder::exports;
        if (editCache)
            paintEditRow (g, "CACHE", cacheR);
        else
            pathRow ("CACHE", cacheR, cacheShown);
        if (editExport)
            paintEditRow (g, "EXPORTS", exportR);
        else
            pathRow ("EXPORTS", exportR, exportShown);
        pathRow ("TOOLS", toolsR, ToolManager::toolsDir());

        // Note line: result of CHANGE/OPEN/REMOVE, otherwise a pending cache folder, otherwise where the folders come from
        juce::String hint = storageNote;
        bool hintIsError = false;
        if (hint.isNotEmpty())
            hintIsError = hint.startsWith ("NOT ") || hint.startsWith ("CANNOT") || hint.startsWith ("COULD NOT")
                       || hint.startsWith ("NEEDS") || hint.startsWith ("THIS IS") || hint.startsWith ("ENTER")
                       || hint.startsWith ("A DOWNLOAD");
        else if (cacheNext != cacheShown)
            hint = "AFTER A RESTART THE CACHE IS " + shortPath (cacheNext) + " (OLD FILES STAY)";
        else if (isEditing)
            hint = editing == sg::storage::Folder::cache ? "CACHE: NEW, EMPTY FOLDER - APPLIES AFTER A RESTART"
                                                         : "EXPORTS: DAW PROJECTS KEEP POINTING TO THE OLD FILES";
        g.setColour (hintIsError ? ui::c().red : ui::c().textSecondary);
        g.setFont (ui::monoUi (10.0f));
        g.drawFittedText (hint, storageHintR.withTrimmedLeft (labelW), juce::Justification::centredLeft, 1, 1.0f);

        const auto bytes = cache->getSize();
        if (! editCache)
        {
            g.setColour (cache->isBusy() ? ui::c().textSecondary : ui::c().textPrimary);
            g.setFont (ui::monoUi (11.0f));
            g.drawText (cache->isBusy() || bytes < 0 ? juce::String ("...") : sg::storage::formatBytes (bytes),
                        cacheSizeR, juce::Justification::centredRight);
        }

        g.setColour (ui::c().textSecondary);
        g.setFont (ui::label (10.0f));
        g.drawText ("AUTO-CLEAN", cleanR.withWidth (labelW), juce::Justification::centredLeft);

        // Note: last clean result, otherwise what the mode does
        juce::String note = cache->getLastNote();
        if (note.isEmpty())
            note = cleanMaxButton.getToggleState() ? "OLDEST FIRST, LOADED FILES KEPT"
                 : clean7Button.getToggleState()   ? "UNUSED FOR 7 DAYS, LOADED FILES KEPT"
                                                   : "NOTHING IS DELETED AUTOMATICALLY";
        g.setFont (ui::monoUi (10.0f));
        g.drawFittedText (note, cleanNoteR, juce::Justification::centredLeft, 1, 1.0f);

        g.setFont (ui::label (10.0f));
        g.drawText ("DOWNLOADS", keepR.withWidth (labelW), juce::Justification::centredLeft);
        g.setFont (ui::monoUi (10.0f));
        g.drawFittedText (keepButton.getToggleState() ? "GRABS AND CONVERSIONS STAY IN THE CACHE"
                                                      : "DELETED AFTER LOADING, PROJECTS KEEP SELECTION + LINK",
                          keepNoteR, juce::Justification::centredLeft, 1, 1.0f);
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
    juce::SharedResourcePointer<CacheManager> cache;

    IconButton closeButton { IconButton::Icon::close };
    juce::TextButton darkButton { "DARK" }, classicButton { "CLASSIC" };
    juce::TextButton checkButton { "CHECK NOW" }, updateButton { "UPDATE" };
    juce::TextButton autoButton { "AUTO-UPDATE YT-DLP: ON" };
    juce::TextButton audioButton { "AUDIO / MIDI SETUP" };
    juce::TextButton cacheOpenButton { "OPEN" }, cacheClearButton { "CLEAR NOW" }, exportOpenButton { "OPEN" };
    juce::TextButton cacheChangeButton { "CHANGE" }, exportChangeButton { "CHANGE" };
    juce::TextButton toolsOpenButton { "OPEN" }, removeToolsButton { "REMOVE TOOLS" };
    juce::TextButton editBrowseButton { "BROWSE" }, editDefaultButton { "DEFAULT" }, editOkButton { "OK" }, editCancelButton { "CANCEL" };
    juce::TextEditor editField;                     // folder path while CHANGE is open (one row at a time)
    std::unique_ptr<juce::FileChooser> chooser;     // native folder chooser (Windows/macOS), alive while open
    sg::storage::Folder editing = sg::storage::Folder::exports;
    bool isEditing = false;
    juce::File cacheShown, cacheNext, exportShown;  // active cache, cache after a restart, export folder
    bool removeArmed = false;                       // REMOVE TOOLS clicked once, waiting for the confirm click
    int removeToken = 0;
    juce::TextButton cleanOffButton { "OFF" }, clean7Button { "7 DAYS" }, cleanMaxButton { "1 GB MAX" };
    juce::TextButton keepButton { "KEEP: ON" };
    juce::String storageNote;                       // note line of the STORAGE section (OPEN fallback, CHANGE, REMOVE)
    bool clearArmed = false;                        // CLEAR NOW clicked once, waiting for the confirm click
    int clearToken = 0;                             // invalidates pending disarm timers

    bool hasAudio = false;
    juce::String localNote;                         // note from the overlay itself (OPEN), until the next tool event
    std::unique_ptr<juce::ChildProcess> opener;     // xdg-open; object stays until the next click (no kill)
    juce::Rectangle<int> panelR, titleR, appLabelR, toolsLabelR, rowsR, toolStatusR, lastCheckR, audioLabelR, footerR, aboutR, sourceR;
    bool sourceHover = false;
    juce::Rectangle<int> storageLabelR, cacheR, cacheSizeR, cleanR, cleanNoteR, keepR, keepNoteR, exportR, toolsR, storageHintR, editR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsOverlay)
};
