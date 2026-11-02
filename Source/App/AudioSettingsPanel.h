// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "ui/SGLookAndFeel.h"
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

// Audio settings as an overlay in the main window (no dialog window, no dropdowns).
// Section labels on the left, choices on the right; devices as a list, sample rate/buffer
// as buttons - only the common values the device actually supports.
class AudioSettingsPanel : public juce::Component,
                           private juce::ChangeListener,
                           private juce::AsyncUpdater
{
public:
    AudioSettingsPanel (juce::AudioDeviceManager& dm, std::function<void()> closeFn)
        : deviceManager (dm), onClose (std::move (closeFn))
    {
        setOpaque (true);

        closeButton.getProperties().set ("outline", true);
        closeButton.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeButton);

        testButton.getProperties().set ("outline", true);
        testButton.onClick = [this] { deviceManager.playTestSound(); };
        addAndMakeVisible (testButton);

        deviceManager.addChangeListener (this);
        rebuild();
    }

    ~AudioSettingsPanel() override
    {
        cancelPendingUpdate();
        deviceManager.removeChangeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (ui::c().canvas);

        auto area = getLocalBounds().reduced (24);
        const auto head = area.removeFromTop (30);

        g.setColour (ui::c().textPrimary);
        g.setFont (ui::strong (13.0f));
        g.drawText ("AUDIO", head, juce::Justification::centredLeft);
        g.fillRect (head.getX(), head.getBottom() - 1, head.getWidth(), 1);

        for (size_t i = 0; i < sections.size(); ++i)
        {
            const auto& s = sections[i];

            if (i > 0)
            {
                g.setColour (ui::c().hairline);
                g.fillRect (area.getX(), s.top - 10, area.getWidth(), 1);
            }

            g.setColour (ui::c().textSecondary);
            g.setFont (ui::label (10.0f));
            g.drawText (s.index, juce::Rectangle<int> (area.getX(), s.top, 24, 28), juce::Justification::centredLeft);
            g.setColour (ui::c().textPrimary);
            g.drawText (s.title, juce::Rectangle<int> (area.getX() + 28, s.top, labelW - 28, 28), juce::Justification::centredLeft);
        }

        const auto foot = juce::Rectangle<int> (area.getX(), area.getBottom() - 28, area.getWidth(), 28);
        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            const double ms = 1000.0 * (double) dev->getOutputLatencyInSamples()
                              / juce::jmax (1.0, dev->getCurrentSampleRate());
            g.setColour (ui::c().textSecondary);
            g.setFont (ui::label (10.0f));
            g.drawText ("OUTPUT LATENCY " + juce::String (ms, 1) + " MS", foot, juce::Justification::centredRight);
        }
        else
        {
            g.setColour (ui::c().red);
            g.setFont (ui::label (10.0f));
            g.drawText ("NO AUDIO DEVICE OPEN", foot, juce::Justification::centredRight);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (24);
        auto head = area.removeFromTop (30);
        closeButton.setBounds (head.removeFromRight (70).withSizeKeepingCentre (70, 22).translated (0, -2));
        area.removeFromTop (22);

        testButton.setBounds (area.getX(), area.getBottom() - 28, 110, 28);

        const int cx = area.getX() + labelW;
        const int cw = area.getWidth() - labelW;
        int y = area.getY();

        for (auto& s : sections)
        {
            s.top = y;
            int yy = y;

            if (s.rows)
            {
                for (auto& b : s.buttons)
                {
                    b->setBounds (cx, yy, cw, 28);
                    yy += 32;
                }
                y = yy - 4;
            }
            else
            {
                int x = cx;
                for (auto& b : s.buttons)
                {
                    const int w = juce::jmin (cw, textWidth (b->getButtonText()) + 28);
                    if (x > cx && x + w > cx + cw)
                    {
                        x = cx;
                        yy += 32;
                    }
                    b->setBounds (x, yy, w, 28);
                    x += w + 6;
                }
                y = yy + 28;
            }

            y += 22;   // spacing + room for the divider line
        }
    }

private:
    struct Section
    {
        juce::String index, title;
        bool rows = false;
        int top = 0;
        std::vector<std::unique_ptr<juce::TextButton>> buttons;
    };

    static constexpr int labelW = 130;

    static int textWidth (const juce::String& t)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (ui::label (10.0f), t, 0.0f, 0.0f);
        return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth());
    }

    static bool isCommonRate (double r)
    {
        for (double c : { 44100.0, 48000.0, 88200.0, 96000.0 })
            if (std::abs (r - c) < 1.0)
                return true;
        return false;
    }

    static bool isCommonBuffer (int b)
    {
        for (int c : { 64, 128, 256, 512, 1024, 2048 })
            if (b == c)
                return true;
        return false;
    }

    Section& addSection (const juce::String& title, bool rows)
    {
        sections.emplace_back();
        auto& s = sections.back();
        s.index = juce::String (sections.size()).paddedLeft ('0', 2);
        s.title = title;
        s.rows = rows;
        return s;
    }

    void addChoice (Section& s, const juce::String& text, bool selected, std::function<void()> action)
    {
        auto b = std::make_unique<juce::TextButton> (text);
        b->setToggleState (selected, juce::dontSendNotification);
        b->getProperties().set ("outline", true);
        b->onClick = std::move (action);
        addAndMakeVisible (*b);
        s.buttons.push_back (std::move (b));
    }

    void applySetup (const std::function<void (juce::AudioDeviceManager::AudioDeviceSetup&)>& change)
    {
        auto st = deviceManager.getAudioDeviceSetup();
        change (st);
        deviceManager.setAudioDeviceSetup (st, true);
    }

    void rebuild()
    {
        sections.clear();

        const auto setup = deviceManager.getAudioDeviceSetup();
        auto* type = deviceManager.getCurrentDeviceTypeObject();
        auto* dev = deviceManager.getCurrentAudioDevice();

        // Driver (only useful if there are several)
        const auto& types = deviceManager.getAvailableDeviceTypes();
        if (types.size() > 1)
        {
            auto& s = addSection ("DRIVER", false);
            for (auto* t : types)
            {
                const auto name = t->getTypeName();
                addChoice (s, name.toUpperCase(), t == type,
                           [this, name] { deviceManager.setCurrentAudioDeviceType (name, true); });
            }
        }

        // Output device as a list
        if (type != nullptr)
        {
            auto& s = addSection ("OUTPUT", true);
            const auto names = type->getDeviceNames (false);
            for (const auto& n : names)
            {
                addChoice (s, n, n == setup.outputDeviceName,
                           [this, n]
                           {
                               applySetup ([n] (juce::AudioDeviceManager::AudioDeviceSetup& st)
                               {
                                   st.outputDeviceName = n;
                                   st.inputDeviceName = juce::String();
                                   st.useDefaultInputChannels = false;
                                   st.useDefaultOutputChannels = true;
                               });
                           });
            }
        }

        if (dev != nullptr)
        {
            // Sample rate: common values only
            auto& sr = addSection ("SAMPLE RATE", false);
            for (double r : dev->getAvailableSampleRates())
            {
                if (! isCommonRate (r))
                    continue;

                addChoice (sr, juce::String (r / 1000.0, 1) + " KHZ",
                           std::abs (r - dev->getCurrentSampleRate()) < 1.0,
                           [this, r] { applySetup ([r] (juce::AudioDeviceManager::AudioDeviceSetup& st) { st.sampleRate = r; }); });
            }

            // Buffer size: common values only (fallback: the first few if none match)
            auto& bs = addSection ("BUFFER", false);
            juce::Array<int> sizes;
            for (int b : dev->getAvailableBufferSizes())
                if (isCommonBuffer (b))
                    sizes.add (b);
            if (sizes.isEmpty())
                for (int b : dev->getAvailableBufferSizes())
                    if (sizes.size() < 6)
                        sizes.add (b);

            const double rate = juce::jmax (1.0, dev->getCurrentSampleRate());
            for (int b : sizes)
            {
                addChoice (bs, juce::String (b) + " SMP / " + juce::String (1000.0 * (double) b / rate, 1) + " MS",
                           b == dev->getCurrentBufferSizeSamples(),
                           [this, b] { applySetup ([b] (juce::AudioDeviceManager::AudioDeviceSetup& st) { st.bufferSize = b; }); });
            }
        }

        resized();
        repaint();
    }

    // Device manager changes arrive asynchronously -> never destroy buttons inside their own onClick
    void changeListenerCallback (juce::ChangeBroadcaster*) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { rebuild(); }

    juce::AudioDeviceManager& deviceManager;
    std::function<void()> onClose;
    juce::TextButton closeButton { "CLOSE" };
    juce::TextButton testButton { "TEST TONE" };
    std::vector<Section> sections;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioSettingsPanel)
};
