// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "UiTheme.h"

// LookAndFeel for both themes (Dark/Classic): fixed shapes, colours and fonts from ui::c() / ui::label().

// Minimal buttons for the window title bar of the standalone app (dash / cross)
class SGWindowButton : public juce::Button
{
public:
    explicit SGWindowButton (int type) : juce::Button ("windowButton"), buttonType (type) {}

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        const auto r = getLocalBounds().toFloat().withSizeKeepingCentre (11.0f, 11.0f);
        g.setColour (over ? ui::c().red : ui::c().textPrimary);

        if (buttonType == juce::DocumentWindow::closeButton)
        {
            g.drawLine (r.getX(), r.getY(), r.getRight(), r.getBottom(), 1.2f);
            g.drawLine (r.getRight(), r.getY(), r.getX(), r.getBottom(), 1.2f);
        }
        else
        {
            g.fillRect (r.getX(), r.getCentreY() - 0.6f, r.getWidth(), 1.2f);
        }
    }

private:
    int buttonType;
};

class SGLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static bool isPrimary (juce::Component& c)
    {
        return static_cast<bool> (c.getProperties().getWithDefault ("primary", false));
    }

    SGLookAndFeel()
    {
        applyTheme();
    }

    // Adopt colours and default font of the active theme. Repaint afterwards when switching.
    void applyTheme()
    {
        const auto& t = ui::c();

        setColour (juce::TextButton::buttonColourId,   t.canvas);
        setColour (juce::TextButton::buttonOnColourId, t.textPrimary);
        setColour (juce::TextButton::textColourOffId,  t.textPrimary);
        setColour (juce::TextButton::textColourOnId,   t.canvas);

        setColour (juce::TextEditor::textColourId,            t.textPrimary);
        setColour (juce::TextEditor::backgroundColourId,      t.surface1);
        setColour (juce::TextEditor::highlightColourId,       t.red.withAlpha (0.25f));
        setColour (juce::TextEditor::highlightedTextColourId, t.textPrimary);
        setColour (juce::TextEditor::outlineColourId,         ui::transparent());
        setColour (juce::TextEditor::focusedOutlineColourId,  ui::transparent());
        setColour (juce::CaretComponent::caretColourId,       t.textPrimary);

        // Dark: Inter Medium as default sans, Classic: nullptr = system default as before.
        // Note: JUCE resolves fonts via the global default LookAndFeel; on this instance
        // it only takes effect if it is set as the default.
        setDefaultSansSerifTypeface (ui::defaultSansTypeface());
    }

    // ---- Buttons: square, flat ----
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bgc,
                               bool over, bool down) override
    {
        // Primary button (GRAB): ink/paper, hover 85 %, disabled surface2
        if (isPrimary (b))
        {
            const auto r = b.getLocalBounds().toFloat();
            g.setColour (! b.isEnabled() ? ui::c().surface2
                                         : ((over || down) ? ui::c().textPrimary.withAlpha (0.85f) : ui::c().textPrimary));
            g.fillRect (r);
            return;
        }

        auto col = bgc;
        if (! b.isEnabled())
            col = col.withMultipliedAlpha (0.4f);

        const auto r = b.getLocalBounds().toFloat();
        g.setColour (col);
        g.fillRect (r);

        if (static_cast<bool> (b.getProperties().getWithDefault ("outline", false)))
        {
            g.setColour (ui::c().textPrimary);
            g.drawRect (r.reduced (0.5f), 1.0f);
        }

        if (down)
        {
            g.setColour (ui::c().shade.withAlpha (0.14f));
            g.fillRect (r);
        }
        else if (over && b.isEnabled())
        {
            g.setColour (ui::c().red.withAlpha (0.10f));
            g.fillRect (r);
        }
    }

    juce::Font getTextButtonFont (juce::TextButton& b, int) override
    {
        return isPrimary (b) ? ui::mono (12.0f) : ui::label (10.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool over, bool down) override
    {
        if (! isPrimary (b))
        {
            juce::LookAndFeel_V4::drawButtonText (g, b, over, down);
            return;
        }

        g.setFont (ui::mono (12.0f));
        g.setColour (b.isEnabled() ? ui::c().canvas : ui::c().textTertiary);
        g.drawText (b.getButtonText().toUpperCase(), b.getLocalBounds(), juce::Justification::centred);
    }

    // ---- Text field: no border, the editor draws the border; the only TextEditor field is the URL field ----
    void fillTextEditorBackground (juce::Graphics& g, int, int, juce::TextEditor&) override
    {
        g.fillAll (ui::c().surface1);
    }

    void drawTextEditorOutline (juce::Graphics&, int, int, juce::TextEditor&) override {}

    // ---- Slider: hairline + thin fill bar + narrow thumb ----
    int getSliderThumbRadius (juce::Slider&) override { return 1; }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float,
                           juce::Slider::SliderStyle, juce::Slider& s) override
    {
        const float cy = (float) y + (float) height * 0.5f;
        const float x0 = (float) x;
        const float x1 = (float) (x + width);

        g.setColour (ui::c().hairline);
        g.fillRect (x0, cy - 0.5f, x1 - x0, 1.0f);

        const double mn = s.getMinimum(), mx = s.getMaximum();
        const double zeroVal = (mn < 0.0 && mx > 0.0) ? 0.0 : mn;
        const float zeroX = x0 + (float) s.valueToProportionOfLength (zeroVal) * (x1 - x0);

        g.setColour (s.isEnabled() ? ui::c().textPrimary : ui::c().textSecondary.withAlpha (0.5f));
        g.fillRect (juce::jmin (zeroX, sliderPos), cy - 1.0f, std::abs (sliderPos - zeroX), 2.0f);

        if (s.isEnabled())
        {
            g.setColour (s.isMouseOverOrDragging() ? ui::c().red : ui::c().textPrimary);
            g.fillRect (sliderPos - 1.0f, cy - 6.0f, 2.0f, 12.0f);
        }
    }

    // ---- Rotary knob: hairline arc, value arc from neutral point, pointer ----
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        const auto r = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (5.0f);
        const float radius = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
        const auto c = r.getCentre();
        const float angle = startAngle + sliderPos * (endAngle - startAngle);

        juce::Path track;
        track.addCentredArc (c.x, c.y, radius, radius, 0.0f, startAngle, endAngle, true);
        g.setColour (ui::c().hairline);
        g.strokePath (track, juce::PathStrokeType (1.5f));

        const double neutral = static_cast<double> (s.getProperties().getWithDefault ("neutral", s.getMinimum()));
        const float neutralAngle = startAngle + (float) s.valueToProportionOfLength (neutral) * (endAngle - startAngle);

        juce::Path value;
        value.addCentredArc (c.x, c.y, radius, radius, 0.0f,
                             juce::jmin (neutralAngle, angle), juce::jmax (neutralAngle, angle), true);
        const bool hot = s.isMouseOverOrDragging();
        g.setColour (hot ? ui::c().red : ui::c().textPrimary);
        g.strokePath (value, juce::PathStrokeType (2.5f));

        const float inner = radius * 0.42f, outer = radius - 5.0f;
        g.setColour (hot ? ui::c().red : ui::c().textPrimary);
        g.drawLine (c.x + inner * std::sin (angle), c.y - inner * std::cos (angle),
                    c.x + outer * std::sin (angle), c.y - outer * std::cos (angle), 2.0f);
    }

    // ---- Window frame of the standalone app: flat, no text, just two symbols ----
    void drawDocumentWindowTitleBar (juce::DocumentWindow&, juce::Graphics& g, int, int, int, int,
                                     const juce::Image*, bool) override
    {
        g.fillAll (ui::c().canvas);
    }

    juce::Button* createDocumentWindowButton (int buttonType) override
    {
        return new SGWindowButton (buttonType);
    }

    void positionDocumentWindowButtons (juce::DocumentWindow&, int titleBarX, int titleBarY, int titleBarW, int titleBarH,
                                        juce::Button* minimiseButton, juce::Button* maximiseButton,
                                        juce::Button* closeButton, bool) override
    {
        int x = titleBarX + titleBarW - 6;
        auto place = [&] (juce::Button* b)
        {
            if (b != nullptr)
            {
                x -= titleBarH;
                b->setBounds (x, titleBarY, titleBarH, titleBarH);
            }
        };
        place (closeButton);
        place (maximiseButton);
        place (minimiseButton);
    }

    // No shadow windows for popup menus: X11/WSLg draws them very slowly
    int getMenuWindowFlags() override { return 0; }

private:
    ui::detail::FontCachePtr fontCache;   // keeps the brand fonts loaded as long as the LookAndFeel lives
};
