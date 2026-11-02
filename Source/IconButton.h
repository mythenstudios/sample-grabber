// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "ui/SGLookAndFeel.h"
#include <cmath>
#include <functional>

// Square icon button (play/pause, stop, loop, back, gear, close). Brand 9: at rest without border and background,
// hover surface3, active (toggle on) red icon, disabled textTertiary. All drawn as juce::Path.
class IconButton : public juce::Button
{
public:
    enum class Icon { play, pause, stop, loop, back, gear, close };

    explicit IconButton (Icon i) : juce::Button ("icon"), icon (i) {}

    // Switch icon at runtime (play <-> pause)
    void setIcon (Icon i)
    {
        if (icon != i)
        {
            icon = i;
            repaint();
        }
    }

    Icon getIcon() const { return icon; }

    // Small red dot top right (gear: update available or error)
    void setBadge (bool b)
    {
        if (badge != b)
        {
            badge = b;
            repaint();
        }
    }

    // Hint instead of a tooltip: the editor shows the text in the footer
    std::function<void (bool)> onHover;

    void mouseEnter (const juce::MouseEvent& e) override
    {
        juce::Button::mouseEnter (e);
        if (onHover != nullptr)
            onHover (true);
    }

    void mouseExit (const juce::MouseEvent& e) override
    {
        juce::Button::mouseExit (e);
        if (onHover != nullptr)
            onHover (false);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto r = getLocalBounds().toFloat();
        const bool enabled = isEnabled();
        const bool on = getToggleState();
        const bool hot = enabled && (over || down);

        if (hot)
        {
            g.setColour (ui::c().surface3);
            g.fillRoundedRectangle (r, 2.0f);
            if (down)
            {
                g.setColour (ui::c().shade.withAlpha (0.08f));
                g.fillRoundedRectangle (r, 2.0f);
            }
        }

        juce::Colour col = ui::c().textSecondary;
        if (! enabled)      col = ui::c().textTertiary;
        else if (on)        col = ui::c().red;
        else if (hot)       col = ui::c().textPrimary;
        g.setColour (col);

        const auto c = r.getCentre();
        const float s = 7.0f;   // half edge length of the icon (14 px)
        const juce::PathStrokeType stroke (1.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt);

        switch (icon)
        {
            case Icon::play:
            {
                juce::Path p;
                p.addTriangle (c.x - s * 0.6f, c.y - s, c.x - s * 0.6f, c.y + s, c.x + s, c.y);
                g.fillPath (p);
                break;
            }

            case Icon::pause:
            {
                juce::Path p;
                p.addRectangle (c.x - s * 0.8f, c.y - s, s * 0.6f, s * 2.0f);
                p.addRectangle (c.x + s * 0.2f, c.y - s, s * 0.6f, s * 2.0f);
                g.fillPath (p);
                break;
            }

            case Icon::stop:
            {
                juce::Path p;
                p.addRectangle (c.x - s * 0.85f, c.y - s * 0.85f, s * 1.7f, s * 1.7f);
                g.fillPath (p);
                break;
            }

            case Icon::loop:
            {
                // Rectangle outline 14x10 with gaps top left and bottom right, an arrowhead at each gap
                const float x0 = c.x - s, x1 = c.x + s, y0 = c.y - 5.0f, y1 = c.y + 5.0f;

                juce::Path a;   // top to right, right to bottom, tip at the bottom
                a.startNewSubPath (c.x - 3.0f, y0);
                a.lineTo (x1, y0);
                a.lineTo (x1, c.y);
                g.strokePath (a, stroke);

                juce::Path b;   // bottom to left, left to top, tip at the top
                b.startNewSubPath (c.x + 3.0f, y1);
                b.lineTo (x0, y1);
                b.lineTo (x0, c.y);
                g.strokePath (b, stroke);

                juce::Path h;
                h.addTriangle (x1 - 2.5f, c.y, x1 + 2.5f, c.y, x1, c.y + 3.0f);
                h.addTriangle (x0 - 2.5f, c.y, x0 + 2.5f, c.y, x0, c.y - 3.0f);
                g.fillPath (h);
                break;
            }

            case Icon::back:
            {
                // Line to the left with arrowhead, hook bending down on the right
                juce::Path p;
                p.startNewSubPath (c.x - s + 1.0f, c.y - 2.0f);
                p.lineTo (c.x + s, c.y - 2.0f);
                p.lineTo (c.x + s, c.y + 5.0f);
                g.strokePath (p, stroke);

                juce::Path head;
                head.startNewSubPath (c.x - s + 4.0f, c.y - 6.0f);
                head.lineTo (c.x - s, c.y - 2.0f);
                head.lineTo (c.x - s + 4.0f, c.y + 2.0f);
                g.strokePath (head, stroke);
                break;
            }

            case Icon::gear:
            {
                // 8 teeth (outer 7, inner 5 px), hole 2.5 px; EvenOdd so the hole stays open
                juce::Path p;
                constexpr int teeth = 8;
                constexpr float ro = 7.0f, ri = 5.0f;
                const float step = juce::MathConstants<float>::twoPi / (float) (teeth * 4);
                for (int k = 0; k < teeth * 4; ++k)
                {
                    const float rad = (k % 4 == 0 || k % 4 == 1) ? ro : ri;
                    const float a = ((float) k - 0.5f) * step;
                    const float px = c.x + rad * std::sin (a), py = c.y - rad * std::cos (a);
                    if (k == 0) p.startNewSubPath (px, py); else p.lineTo (px, py);
                }
                p.closeSubPath();
                p.addEllipse (c.x - 2.5f, c.y - 2.5f, 5.0f, 5.0f);
                p.setUsingNonZeroWinding (false);
                g.fillPath (p);
                break;
            }

            case Icon::close:
            {
                const float k = 5.0f;
                juce::Path p;
                p.startNewSubPath (c.x - k, c.y - k);
                p.lineTo (c.x + k, c.y + k);
                p.startNewSubPath (c.x + k, c.y - k);
                p.lineTo (c.x - k, c.y + k);
                g.strokePath (p, stroke);
                break;
            }
        }

        if (badge)
        {
            g.setColour (ui::c().red);
            g.fillEllipse (c.x + 4.0f, c.y - 9.0f, 5.0f, 5.0f);
        }
    }

private:
    Icon icon;
    bool badge = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
};
