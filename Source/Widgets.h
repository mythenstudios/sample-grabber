// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "PluginProcessor.h"
#include "ui/SGLookAndFeel.h"
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

//==============================================================================
// One cell of the parameter bar: title left, value right, slider below.
// empty paramID = placeholder slider (disabled, no parameter).
class ParamCell : public juce::Component
{
public:
    ParamCell (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID,
               const juce::String& title, const juce::String& unitText, int decimalPlaces, bool signedValue)
        : titleText (title), unit (unitText), decimals (decimalPlaces), showSign (signedValue)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.onValueChange = [this] { repaint(); };
        addAndMakeVisible (slider);

        if (paramID.isNotEmpty())
        {
            attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, paramID, slider);

            if (auto* p = apvts.getParameter (paramID))
                slider.setDoubleClickReturnValue (true, (double) p->convertFrom0to1 (p->getDefaultValue()));
        }
        else
        {
            slider.setRange (50.0, 150.0, 1.0);
            slider.setValue (100.0, juce::dontSendNotification);
            slider.setEnabled (false);
        }
    }

    void resized() override
    {
        slider.setBounds (12, getHeight() - 28, getWidth() - 24, 20);
    }

    void paint (juce::Graphics& g) override
    {
        auto row = getLocalBounds().reduced (12, 0).withTrimmedTop (8).removeFromTop (14);
        const bool on = slider.isEnabled();

        g.setColour (ui::c().textSecondary);
        g.setFont (ui::label (9.0f));
        g.drawText (titleText, row, juce::Justification::centredLeft);

        juce::String text = juce::String (slider.getValue(), decimals);
        if (showSign && slider.getValue() > 0.0)
            text = "+" + text;

        g.setColour (on ? ui::c().textPrimary : ui::c().textSecondary);
        g.setFont (ui::strong (10.0f));
        g.drawText (on ? text + unit : juce::String ("SOON"), row, juce::Justification::centredRight);
    }

private:
    juce::String titleText, unit;
    int decimals;
    bool showSign;

    juce::Slider slider;   // declared before the attachment -> destroyed after it
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamCell)
};

//==============================================================================
// 16 pads with a mini waveform of each slice.
// Click = preview, drag = drag slice as WAV into the DAW.
class SliceGrid : public juce::Component, private juce::Timer
{
public:
    explicit SliceGrid (SampleGrabberProcessor& p) : proc (p)
    {
        // Developer aid only: SG_SNAPSHOT_PAD=<1..16> shows this pad as playing (snapshot)
        const int sp = juce::SystemStats::getEnvironmentVariable ("SG_SNAPSHOT_PAD", {}).getIntValue();
        if (sp >= 1 && sp <= SampleGrabberProcessor::numPads)
            snapshotPad = sp - 1;
    }

    ~SliceGrid() override { stopTimer(); }

    std::function<void (int)> onDragPad;

    int knownVersion() const { return version; }

    void refreshThumbs()
    {
        const auto sb = proc.getSliceBoundsNorm();
        const bool has = proc.hasSample();

        for (int i = 0; i < SampleGrabberProcessor::numPads; ++i)
        {
            auto& t = thumbs[(size_t) i];
            t.clear();
            isEmpty[(size_t) i] = has && sb[(size_t) i + 1] <= sb[(size_t) i];   // pad without onset (TRANSIENT)
            if (! has || isEmpty[(size_t) i])
                continue;

            const auto cols = proc.computePeaks (thumbCols, sb[(size_t) i], sb[(size_t) i + 1]);
            for (const auto& c : cols)
                t.push_back (juce::jmax (std::abs (c.mn), c.mx));
        }

        // Hover on a pad that is now empty (e.g. after a mode change): discard hover state
        if (hoverPad >= 0 && (! has || isEmpty[(size_t) hoverPad]))
            hoverPad = -1;

        version = proc.getSliceVersion();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const int n = SampleGrabberProcessor::numPads;
        const float gap = 2.0f;
        const float cw = ((float) getWidth() - gap * (float) (n - 1)) / (float) n;
        const int active = snapshotPad >= 0 ? snapshotPad : proc.getActivePad();
        const auto& pal = ui::c();

        for (int i = 0; i < n; ++i)
        {
            const juce::Rectangle<float> r ((float) i * (cw + gap), 0.0f, cw, (float) getHeight());
            const bool on = (i == active);
            const bool empty = isEmpty[(size_t) i] || thumbs[(size_t) i].empty();
            const bool flash = (i == flashPad);

            // Fill: empty = none, filled surface2, hover surface3, trigger flash brighter than surface3
            if (! empty)
            {
                juce::Colour fill = pal.surface2;
                if (flash)
                    fill = pal.surface3.interpolatedWith (pal.textPrimary, 0.12f);
                else if (i == hoverPad)
                    fill = pal.surface3;
                g.setColour (fill);
                g.fillRoundedRectangle (r, 2.0f);
            }

            // Border: playing = 2 px red inside, otherwise 1 px hairline (empty dimmed)
            if (on)
            {
                g.setColour (pal.red);
                g.drawRoundedRectangle (r.reduced (1.0f), 1.0f, 2.0f);
            }
            else
            {
                g.setColour (empty ? pal.hairline.withAlpha (0.45f) : pal.hairline);
                g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
            }

            // Mini waveform in the lower half
            const auto& t = thumbs[(size_t) i];
            if (! t.empty())
            {
                const float maxH = r.getHeight() * 0.5f - 6.0f;
                const float midY = r.getBottom() - r.getHeight() * 0.25f;
                const float x0 = r.getX() + (cw - (float) thumbCols * 3.0f + 1.0f) * 0.5f;
                g.setColour (on ? pal.textPrimary : pal.textPrimary.withAlpha (0.45f));

                for (size_t k = 0; k < t.size(); ++k)
                {
                    const float bh = juce::jlimit (1.0f, juce::jmax (1.0f, maxH), t[k] * maxH);
                    g.fillRect (x0 + (float) k * 3.0f, midY - bh * 0.5f, 2.0f, bh);
                }
            }

            // Number top left, two digits, mono
            g.setColour (empty ? pal.textTertiary.withAlpha (0.5f) : pal.textTertiary);
            g.setFont (ui::mono (9.0f));
            g.drawText (juce::String (i + 1).paddedLeft ('0', 2),
                        juce::Rectangle<float> (r.getX() + 4.0f, r.getY() + 3.0f, 20.0f, 10.0f),
                        juce::Justification::centredLeft);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { setHover (padAt (e.x)); }
    void mouseExit (const juce::MouseEvent&) override { setHover (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragPad = padAt (e.x);
        if (dragPad >= 0 && isEmpty[(size_t) dragPad])
            dragPad = -1;   // empty pads: no preview, no drag
        dragging = false;
        if (dragPad >= 0)
        {
            proc.triggerPad (dragPad);
            flashPad = dragPad;
            startTimer (120);   // flash 120 ms, timer stops itself
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragPad >= 0 && ! dragging && e.getDistanceFromDragStart() > 8)
        {
            dragging = true;
            if (onDragPad)
                onDragPad (dragPad);
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragging = false;
        dragPad = -1;
    }

private:
    static constexpr int thumbCols = 10;

    void timerCallback() override
    {
        stopTimer();
        flashPad = -1;
        repaint();
    }

    void setHover (int i)
    {
        if (i >= 0 && isEmpty[(size_t) i])
            i = -1;
        if (i != hoverPad)
        {
            hoverPad = i;
            repaint();
        }
    }

    int padAt (int x) const
    {
        const int n = SampleGrabberProcessor::numPads;
        const float gap = 2.0f;
        const float cw = ((float) getWidth() - gap * (float) (n - 1)) / (float) n;
        const int i = (int) ((float) x / (cw + gap));
        return (i >= 0 && i < n) ? i : -1;
    }

    SampleGrabberProcessor& proc;
    std::array<std::vector<float>, SampleGrabberProcessor::numPads> thumbs;
    std::array<bool, SampleGrabberProcessor::numPads> isEmpty {};
    int version = -1;
    int dragPad = -1;
    int hoverPad = -1;
    int flashPad = -1;
    int snapshotPad = -1;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SliceGrid)
};

//==============================================================================
// "DRAG TO TRACK": dragging starts a file drag of the current selection.
class DragHandle : public juce::Component
{
public:
    DragHandle() { setMouseCursor (juce::MouseCursor::DraggingHandCursor); }

    std::function<void()> onDragStart;

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour (hover ? ui::c().red : ui::c().textPrimary);
        g.drawRect (r.reduced (0.5f), 1.0f);
        g.setFont (ui::label (10.0f));
        g.drawText ("DRAG TO TRACK", getLocalBounds(), juce::Justification::centred);
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown  (const juce::MouseEvent&) override { started = false; }
    void mouseUp    (const juce::MouseEvent&) override { started = false; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! started && e.getDistanceFromDragStart() > 6)
        {
            started = true;
            if (onDragStart)
                onDragStart();
        }
    }

private:
    bool started = false, hover = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DragHandle)
};
