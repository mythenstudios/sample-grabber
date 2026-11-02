// Sample Grabber - https://github.com/mythenstudios/sample-grabber
// Copyright (C) 2026 Mythen Studios <hello@mythenstudios.com>
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "PluginProcessor.h"
#include "ui/SGLookAndFeel.h"
#include <cmath>
#include <functional>
#include <vector>

// Waveform in bar style (RMS body + peak envelope) with zoom.
//  - Drag on free area: new selection, drag on the handles: adjust trim
//  - Mouse wheel: zoom (around the cursor), Shift+wheel: scroll
//  - Double click: zoom to selection / back to full view
//
// Performance: the bars are rendered into two images only on load/zoom/resize
// (inside/outside the selection). While dragging, only these images are composed
// via clip - fast even without a GPU (WSLg).
class WaveformView : public juce::Component
{
public:
    explicit WaveformView (SampleGrabberProcessor& p) : proc (p) { setOpaque (true); }
    ~WaveformView() override { endFadeGestures(); }   // editor closes during a drag: do not leave the host in touch/latch

    std::function<void()> onTrimChanged;

    void refresh()
    {
        if (! proc.hasSample() || getWidth() <= 0)
            peaks.clear();
        else
            peaks = proc.computePeaks (juce::jmax (1, getWidth() / pitchPx), viewStart, viewEnd);   // one column per bar

        rebuildImages();
        repaint();
    }

    // After a theme change: colours are baked into the pre-rendered images -> re-render
    void themeChanged()
    {
        rebuildImages();
        repaint();
    }

    void resetView()
    {
        viewStart = 0.0f;
        viewEnd = 1.0f;
        refresh();
    }

    void resized() override { refresh(); }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds();
        const float w = (float) bounds.getWidth();
        const float h = (float) bounds.getHeight();

        g.fillAll (ui::c().canvas);

        if (peaks.empty())
        {
            g.setColour (ui::c().textSecondary);
            g.setFont (ui::label (10.0f));
            g.drawText ("PASTE A URL OR DROP A FILE", bounds, juce::Justification::centred);
        }
        else
        {
            const float mid = h * 0.5f;
            const float tl = xOf (proc.getTrimStart());
            const float tr = xOf (proc.getTrimEnd());

            g.setColour (ui::c().hairline);
            g.fillRect (0.0f, mid - 0.5f, w, 1.0f);

            // Bars from the pre-rendered images: light outside, dark inside the selection
            g.drawImageAt (imgOutside, 0, 0);

            const int x0 = juce::jlimit (0, bounds.getWidth(), (int) std::floor (tl));
            const int x1 = juce::jlimit (0, bounds.getWidth(), (int) std::ceil (tr));
            if (x1 > x0)
            {
                juce::Graphics::ScopedSaveState ss (g);
                g.reduceClipRegion (x0, 0, x1 - x0, bounds.getHeight());
                g.drawImageAt (imgInside, 0, 0);
            }

            // Darken the fade areas of the selection (overlay over the cached image, no rebuildImages)
            const auto fg = fadeGeometry();
            if (fg.valid)
                drawFadeRamps (g, fg, proc.getTrimStart(), proc.getTrimEnd(), false, 0.0f);

            // Slice markers (no numbers while dragging - text is the most expensive part)
            const auto sb = proc.getSliceBoundsNorm();
            const bool labels = (mode == Drag::none);
            g.setFont (ui::label (8.0f));

            for (int i = 0; i < SampleGrabberProcessor::numPads; ++i)
            {
                const float x = xOf (sb[(size_t) i]);
                const float sliceW = xOf (sb[(size_t) i + 1]) - x;

                if (i > 0 && sb[(size_t) i] < sb[(size_t) SampleGrabberProcessor::numPads] && x >= 0.0f && x <= w)
                {
                    g.setColour (ui::c().red.withAlpha (0.55f));
                    g.fillRect (x, 0.0f, 1.0f, h);
                }

                if (labels && sliceW >= 16.0f && x >= 0.0f && x < w - 12.0f)
                {
                    g.setColour (ui::c().red);
                    g.drawText (juce::String (i + 1), juce::Rectangle<float> (x + 3.0f, fadeBandH + 1.0f, 16.0f, 10.0f),
                                juce::Justification::centredLeft);
                }
            }

            // Fade curves: selection (strong) and, if a pad is playing, the same on the pad (faint, no handles)
            if (fg.valid)
            {
                drawFadeRamps (g, fg, proc.getTrimStart(), proc.getTrimEnd(), true, 0.6f);

                const int ap = proc.getActivePad();
                if (ap >= 0 && ap < SampleGrabberProcessor::numPads)
                    drawFadeRamps (g, fg, sb[(size_t) ap], sb[(size_t) ap + 1], true, 0.35f);
            }

            // Trim handles
            g.setColour (ui::c().textPrimary);
            if (tl >= -1.0f && tl <= w + 1.0f)
            {
                g.fillRect (tl - 1.0f, 0.0f, 2.0f, h);
                g.fillRect (tl - 1.0f, 0.0f, 8.0f, 3.0f);
            }
            if (tr >= -1.0f && tr <= w + 1.0f)
            {
                g.fillRect (tr - 1.0f, 0.0f, 2.0f, h);
                g.fillRect (tr - 7.0f, h - 3.0f, 8.0f, 3.0f);
            }

            // Fade handles: 8x8 at the top at the end of each fade, value in mono next to it while dragging
            if (fg.valid)
            {
                for (const auto which : { Drag::fadeIn, Drag::fadeOut })
                {
                    const float hx = which == Drag::fadeIn ? fg.inX : fg.outX;
                    if (hx < -4.0f || hx > w + 4.0f)
                        continue;

                    const auto sq = juce::Rectangle<float> (hx - 4.0f, (float) fadeHandleTop, 8.0f, 8.0f);
                    const bool hot = (hoverHandle == which) || (mode == which);
                    g.setColour (hot ? ui::c().textPrimary : ui::c().canvas);
                    g.fillRect (sq);
                    g.setColour (ui::c().textPrimary);
                    g.drawRect (sq, 1.0f);

                    if (mode == which)
                    {
                        const bool in = which == Drag::fadeIn;
                        const float pct = in ? proc.getFadeInPct() : proc.getFadeOutPct();
                        const int shape = juce::roundToInt (proc.getFadeShape());
                        const juce::String text = juce::String (in ? "IN " : "OUT ") + juce::String (pct, 1) + " %  SHAPE "
                                                  + (shape > 0 ? "+" : "") + juce::String (shape);

                        const auto font = ui::mono (10.0f);
                        g.setFont (font);
                        const float tw = juce::GlyphArrangement::getStringWidth (font, text) + 6.0f;
                        float tx = in ? hx + 8.0f : hx - 8.0f - tw;
                        if (tx + tw > w - 2.0f) tx = hx - 8.0f - tw;   // at the edge, to the other side
                        if (tx < 2.0f)          tx = juce::jmin (hx + 8.0f, w - 2.0f - tw);
                        const auto tb = juce::Rectangle<float> (tx, (float) fadeHandleTop - 2.0f, tw, 12.0f);
                        g.setColour (ui::c().canvas);
                        g.fillRect (tb);
                        g.setColour (ui::c().textPrimary);
                        g.drawText (text, tb, juce::Justification::centred, false);
                    }
                }
            }

            // Playhead
            const float ph = proc.getPlayheadNorm();
            if (ph >= 0.0f)
            {
                const float x = xOf (ph);
                if (x >= 0.0f && x <= w)
                {
                    g.setColour (ui::c().red);
                    g.fillRect (x - 0.5f, 0.0f, 1.5f, h);
                }
            }

            // Zoom indicator
            if (viewEnd - viewStart < 0.999f)
            {
                g.setColour (ui::c().textSecondary);
                g.setFont (ui::label (8.0f));
                g.drawText ("ZOOM " + juce::String (1.0f / (viewEnd - viewStart), 1) + "X   DOUBLE-CLICK TO RESET",
                            bounds.reduced (6, 4), juce::Justification::bottomRight);
            }
        }

        g.setColour (ui::c().hairline);
        g.drawRect (bounds, 1);
    }

    //==========================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        if (peaks.empty())
            return;

        const Drag h = fadeHandleAt (e.position);
        if (h != hoverHandle)
        {
            hoverHandle = h;
            repaint();   // only when the hover state changes, not on every mouse move
        }

        if (h != Drag::none)
            setMouseCursor (juce::MouseCursor::UpDownLeftRightResizeCursor);
        else
            setMouseCursor (nearLeft (e.x) || nearRight (e.x) ? juce::MouseCursor::LeftRightResizeCursor
                                                              : juce::MouseCursor::CrosshairCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverHandle != Drag::none && mode == Drag::none)
        {
            hoverHandle = Drag::none;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (peaks.empty())
            return;

        prevStart = proc.getTrimStart();
        prevEnd = proc.getTrimEnd();
        anchor = normOf (e.x);
        lastCommitMs = 0;
        selectStarted = false;

        // defensive: if a fade drag ended without mouseUp, close gestures now
        if (mode == Drag::fadeIn || mode == Drag::fadeOut)
        {
            endFadeGestures();
            mode = Drag::none;
        }

        // Fade handle takes precedence over the trim handles in the top band (left mouse button only).
        // Gestures are opened only on the first real value change (dragFadeHandle).
        const Drag h = e.mods.isLeftButtonDown() ? fadeHandleAt (e.position) : Drag::none;
        if (h != Drag::none)
        {
            mode = h;
            downPos = e.position;
            downPct = h == Drag::fadeIn ? proc.getFadeInPct() : proc.getFadeOutPct();
            downShape = proc.getFadeShape();
            shapeActive = false;
            repaint();
            return;
        }

        if (nearLeft (e.x))        mode = Drag::left;
        else if (nearRight (e.x))  mode = Drag::right;
        else                       mode = Drag::select;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (mode == Drag::none || peaks.empty())
            return;

        if (mode == Drag::fadeIn || mode == Drag::fadeOut)
        {
            dragFadeHandle (e);
            return;
        }

        const float x = normOf (e.x);
        const float gap = 0.01f * (viewEnd - viewStart);

        // Click with slight jitter: do not start a selection yet
        if (mode == Drag::select && ! selectStarted)
        {
            if (std::abs (x - anchor) < 0.015f * (viewEnd - viewStart))
                return;
            selectStarted = true;
        }

        // Set trim immediately, but recompute slices only throttled
        if (mode == Drag::left)
            proc.setTrim (juce::jmin (x, proc.getTrimEnd() - gap), proc.getTrimEnd(), false);
        else if (mode == Drag::right)
            proc.setTrim (proc.getTrimStart(), juce::jmax (x, proc.getTrimStart() + gap), false);
        else
            proc.setTrim (juce::jmin (anchor, x), juce::jmax (anchor, x), false);

        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastCommitMs >= 60)
        {
            proc.commitSlices();
            lastCommitMs = now;
        }

        repaint();
        if (onTrimChanged)
            onTrimChanged();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (mode == Drag::none)
            return;

        if (mode == Drag::fadeIn || mode == Drag::fadeOut)
        {
            endFadeGestures();
            mode = Drag::none;
            repaint();
            return;
        }

        // plain click without a real selection: nothing changed, nothing to recompute
        if (mode == Drag::select && ! selectStarted)
        {
            mode = Drag::none;
            return;
        }

        // discard a too-small new selection (accidental click)
        if (mode == Drag::select && (proc.getTrimEnd() - proc.getTrimStart()) < 0.015f * (viewEnd - viewStart))
        {
            proc.setTrim (prevStart, prevEnd);
        }
        else
        {
            proc.commitSlices();

            // Selection really changed: running playback (also in loop) restarts with the new bounds
            if (proc.getTrimStart() != prevStart || proc.getTrimEnd() != prevEnd)
                proc.retriggerIfPlaying();
        }

        mode = Drag::none;
        repaint();
        if (onTrimChanged)
            onTrimChanged();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! proc.hasSample())
            return;

        // Double click on a fade handle: fade to 0 (instead of zoom)
        const Drag h = fadeHandleAt (e.position);
        if (h != Drag::none)
        {
            if (auto* lp = lengthParam (h))
            {
                lp->beginChangeGesture();
                lp->setValueNotifyingHost (lp->convertTo0to1 (0.0f));
                lp->endChangeGesture();
            }
            hoverHandle = fadeHandleAt (e.position);   // handle may now be elsewhere
            repaint();
            return;
        }

        if (viewEnd - viewStart < 0.999f)
        {
            resetView();
        }
        else
        {
            const float a = proc.getTrimStart(), b = proc.getTrimEnd();
            if (b - a < 0.98f)
            {
                const float pad = (b - a) * 0.08f;
                setView (a - pad, b + pad);
            }
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (! proc.hasSample())
            return;

        const float span = viewEnd - viewStart;
        const bool horizontal = e.mods.isShiftDown() || std::abs (wheel.deltaX) > std::abs (wheel.deltaY);

        if (horizontal)
        {
            const float d = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
            const float move = -d * span * 0.5f;
            setView (viewStart + move, viewEnd + move);
        }
        else
        {
            const float at = normOf (e.x);
            const float newSpan = juce::jlimit (0.002f, 1.0f, span * std::exp (-wheel.deltaY * 1.2f));
            const float t = (at - viewStart) / span;
            setView (at - t * newSpan, at - t * newSpan + newSpan);
        }
    }

private:
    enum class Drag { none, left, right, select, fadeIn, fadeOut };
    static constexpr int pitchPx = 3;   // 2 px bar + 1 px gap
    static constexpr int fadeHandleTop = 4;        // top edge of the 8x8 fade handles
    static constexpr float fadeBandH = 16.0f;      // hit band of the fade handles (top), below it the trim handles grab
    static constexpr float fadeHitHalf = 6.0f;     // hit zone 12 px wide
    static constexpr float shapePerPx = 100.0f / 80.0f;   // vertical drag: 100 shape units per 80 px
    static constexpr float shapeDeadPx = 4.0f;            // dead zone before vertical drag changes SHAPE

    // Everything curve, darkening and handles need - same values as playback (fadeEnvelope)
    struct FadeGeom
    {
        bool valid = false;
        double numSamples = 0.0, inFrac = 0.0, outFrac = 0.0, exponent = 1.0, clickSamples = 0.0;
        float tl = 0.0f, tr = 0.0f, inX = 0.0f, outX = 0.0f;
    };

    FadeGeom fadeGeometry() const
    {
        FadeGeom f;
        const int total = proc.getNumSamples();
        f.tl = xOf (proc.getTrimStart());
        f.tr = xOf (proc.getTrimEnd());
        if (total <= 0 || f.tr - f.tl < 2.0f)
            return f;

        f.valid = true;
        f.numSamples = (double) total;
        f.inFrac  = juce::jlimit (0.0, 0.5, (double) proc.getFadeInPct()  * 0.01);
        f.outFrac = juce::jlimit (0.0, 0.5, (double) proc.getFadeOutPct() * 0.01);
        f.exponent = SampleGrabberProcessor::fadeExponent ((double) proc.getFadeShape());
        f.clickSamples = SampleGrabberProcessor::clickGuardSeconds * proc.getFileSampleRate();
        const float selPx = f.tr - f.tl;
        f.inX  = f.tl + (float) f.inFrac  * selPx;
        f.outX = f.tr - (float) f.outFrac * selPx;
        return f;
    }

    // Fade ramps of a region [aNorm, bNorm] (selection or pad): curve = false -> darken bars, otherwise 1 px curve
    // (at the bottom at the region edge, at the top at the fade end). Computes only within the ramps and the view:
    // darkening on the bar grid (pitchPx), curve in 2 px steps.
    // Click guard as in playback: start always (first pass), end only without loop.
    void drawFadeRamps (juce::Graphics& g, const FadeGeom& f, float aNorm, float bNorm, bool curve, float alpha) const
    {
        const double clickOut = proc.isLoopOn() ? 0.0 : f.clickSamples;
        const float w = (float) getWidth();
        const float yBot = (float) getHeight() - 2.0f;
        const float yTop = (float) fadeHandleTop + 4.0f;   // middle of the handles
        const float xa = xOf (aNorm), xb = xOf (bNorm);
        const float lenPx = xb - xa;
        if (lenPx < 2.0f)
            return;

        const double s0 = (double) aNorm * f.numSamples, s1 = (double) bNorm * f.numSamples;
        const float clickFracPx = (float) juce::jmin (0.5, f.clickSamples / juce::jmax (1.0, s1 - s0)) * lenPx;
        const float inPx  = juce::jmax ((float) f.inFrac  * lenPx, clickFracPx);
        const float outPx = juce::jmax ((float) f.outFrac * lenPx, clickOut > 0.0 ? clickFracPx : 0.0f);
        const double span = (double) (viewEnd - viewStart);

        auto envAtX = [&] (float x)
        {
            const double pos = ((double) viewStart + (double) x / (double) juce::jmax (1.0f, w) * span) * f.numSamples;
            return SampleGrabberProcessor::fadeEnvelope (juce::jlimit (s0, s1, pos), s0, s1, f.inFrac, f.outFrac,
                                                         f.exponent, f.clickSamples, clickOut);
        };

        auto ramp = [&] (float from, float to)
        {
            if (to - from < 1.0f)
                return;
            const float vf = juce::jmax (from, -1.0f), vt = juce::jmin (to, w + 1.0f);
            if (vt <= vf)
                return;

            if (! curve)
            {
                // one rectangle per bar (grid pitchPx as in rebuildImages), env at the bar centre
                const int c0 = juce::jmax (0, (int) std::floor (vf / (float) pitchPx));
                for (int c = c0; (float) (c * pitchPx) < vt && c * pitchPx < (int) w; ++c)
                {
                    const float env = envAtX ((float) (c * pitchPx) + 1.0f);
                    if (env < 0.999f)
                    {
                        g.setColour (ui::c().canvas.withAlpha ((1.0f - env) * 0.85f));
                        g.fillRect ((float) (c * pitchPx), 1.0f, (float) pitchPx, (float) getHeight() - 2.0f);
                    }
                }
                return;
            }

            juce::Path p;
            p.startNewSubPath (vf, yBot - envAtX (vf) * (yBot - yTop));
            for (float x = std::floor (vf) + 2.0f; x < vt; x += 2.0f)
                p.lineTo (x, yBot - envAtX (x) * (yBot - yTop));
            p.lineTo (vt, yBot - envAtX (vt) * (yBot - yTop));
            g.setColour (ui::c().textPrimary.withAlpha (alpha));
            g.strokePath (p, juce::PathStrokeType (1.0f));
        };

        ramp (xa, xa + inPx);
        ramp (xb - outPx, xb);
    }

    // Fade handle under the mouse (top band only). If both overlap (50/50), the side decides.
    Drag fadeHandleAt (juce::Point<float> pt) const
    {
        if (peaks.empty() || pt.y > fadeBandH)
            return Drag::none;

        const auto f = fadeGeometry();
        if (! f.valid)
            return Drag::none;

        // same visibility as when drawing: a handle outside the view cannot be grabbed
        const float w = (float) getWidth();
        auto visible = [w] (float hx) { return hx >= -4.0f && hx <= w + 4.0f; };
        const float dIn = std::abs (pt.x - f.inX), dOut = std::abs (pt.x - f.outX);
        const bool hitIn = visible (f.inX) && dIn <= fadeHitHalf, hitOut = visible (f.outX) && dOut <= fadeHitHalf;
        if (hitIn && hitOut)
        {
            if (std::abs (f.inX - f.outX) < 1.0f)
                return pt.x <= f.inX ? Drag::fadeIn : Drag::fadeOut;
            return dIn <= dOut ? Drag::fadeIn : Drag::fadeOut;
        }
        return hitIn ? Drag::fadeIn : (hitOut ? Drag::fadeOut : Drag::none);
    }

    juce::RangedAudioParameter* lengthParam (Drag d) const
    {
        return proc.apvts.getParameter (d == Drag::fadeIn ? "fadeInPct" : "fadeOutPct");
    }
    juce::RangedAudioParameter* shapeParam() const { return proc.apvts.getParameter ("fadeShape"); }

    // Close open fade gestures in pairs (mouseUp, destructor, defensive in mouseDown)
    void endFadeGestures()
    {
        if (lengthGesture != nullptr)
        {
            lengthGesture->endChangeGesture();
            lengthGesture = nullptr;
        }
        if (shapeGestureOpen)
        {
            if (auto* sp = shapeParam())
                sp->endChangeGesture();
            shapeGestureOpen = false;
        }
    }

    // Drag on the fade handle, relative to the click point (no jump on click):
    // horizontal = length (fraction of the selection), vertical = SHAPE (up = more negative, curve bulges upward).
    void dragFadeHandle (const juce::MouseEvent& e)
    {
        const auto f = fadeGeometry();
        if (! f.valid)
            return;

        const float selPx = f.tr - f.tl;
        const float dx = e.position.x - downPos.x;
        const float dy = e.position.y - downPos.y;

        const float pct = juce::jlimit (0.0f, SampleGrabberProcessor::fadeMaxPct,
                                        downPct + (mode == Drag::fadeIn ? dx : -dx) / selPx * 100.0f);
        if (auto* lp = lengthParam (mode))
        {
            const float v = lp->convertTo0to1 (pct);
            if (std::abs (v - lp->getValue()) > 1.0e-6f)
            {
                if (lengthGesture == nullptr)   // gesture only on the first real value change
                {
                    lengthGesture = lp;
                    lp->beginChangeGesture();
                }
                lp->setValueNotifyingHost (v);
            }
        }

        // Shape only after 4 px vertical movement (from there without a jump), so horizontal dragging does not jitter the curve
        if (! shapeActive && std::abs (dy) > shapeDeadPx)
            shapeActive = true;
        if (shapeActive)
            if (auto* sp = shapeParam())
            {
                const float eff = (std::abs (dy) - shapeDeadPx) * (dy < 0.0f ? -1.0f : 1.0f);
                const float v = sp->convertTo0to1 (juce::jlimit (-100.0f, 100.0f, downShape + eff * shapePerPx));
                if (std::abs (v - sp->getValue()) > 1.0e-6f)
                {
                    if (! shapeGestureOpen)
                    {
                        shapeGestureOpen = true;
                        sp->beginChangeGesture();
                    }
                    sp->setValueNotifyingHost (v);
                }
            }

        repaint();
    }

    float normOf (int x) const
    {
        const float t = juce::jlimit (0.0f, 1.0f, (float) x / (float) juce::jmax (1, getWidth()));
        return viewStart + t * (viewEnd - viewStart);
    }

    float xOf (float norm) const
    {
        return (norm - viewStart) / (viewEnd - viewStart) * (float) getWidth();
    }

    bool nearLeft (int x) const  { return std::abs ((float) x - xOf (proc.getTrimStart())) <= 7.0f; }
    bool nearRight (int x) const { return std::abs ((float) x - xOf (proc.getTrimEnd())) <= 7.0f; }

    void setView (float s, float e)
    {
        const float span = juce::jlimit (0.002f, 1.0f, e - s);
        s = juce::jlimit (0.0f, 1.0f - span, s);
        viewStart = s;
        viewEnd = s + span;
        refresh();
    }

    // Render bars once into two images (inside dark / outside light)
    void rebuildImages()
    {
        imgInside = juce::Image();
        imgOutside = juce::Image();

        const int w = getWidth(), h = getHeight();
        if (peaks.empty() || w <= 0 || h <= 0)
            return;

        imgInside  = juce::Image (juce::Image::ARGB, w, h, true);
        imgOutside = juce::Image (juce::Image::ARGB, w, h, true);
        juce::Graphics gi (imgInside);
        juce::Graphics go (imgOutside);

        const float mid = (float) h * 0.5f;
        const float maxH = mid - 8.0f;

        for (int c = 0; c < (int) peaks.size(); ++c)
        {
            const auto& pk = peaks[(size_t) c];
            const float x = (float) (c * pitchPx);

            const float peak = juce::jmax (std::abs (pk.mn), pk.mx);
            const float ph = juce::jmax (1.0f, peak * maxH);
            const float rh = juce::jmax (1.0f, juce::jmin (ph, pk.rms * 1.5f * maxH));

            gi.setColour (ui::c().textPrimary.withAlpha (0.22f));
            gi.fillRect (x, mid - ph, 2.0f, ph * 2.0f);
            gi.setColour (ui::c().textPrimary);
            gi.fillRect (x, mid - rh, 2.0f, rh * 2.0f);

            go.setColour (ui::c().hairline.withAlpha (0.7f));
            go.fillRect (x, mid - ph, 2.0f, ph * 2.0f);
            go.setColour (ui::c().textSecondary.withAlpha (0.45f));
            go.fillRect (x, mid - rh, 2.0f, rh * 2.0f);
        }
    }

    SampleGrabberProcessor& proc;
    std::vector<SampleGrabberProcessor::PeakColumn> peaks;
    juce::Image imgInside, imgOutside;
    float viewStart = 0.0f, viewEnd = 1.0f;
    Drag mode = Drag::none;
    bool selectStarted = false;
    Drag hoverHandle = Drag::none;      // fade handle under the mouse (fadeIn/fadeOut/none)
    juce::Point<float> downPos;         // click point while dragging a fade handle
    float downPct = 0.0f, downShape = 0.0f;
    bool shapeActive = false;
    juce::RangedAudioParameter* lengthGesture = nullptr;   // parameter with open length gesture (nullptr = none)
    bool shapeGestureOpen = false;
    float anchor = 0.0f, prevStart = 0.0f, prevEnd = 1.0f;
    juce::uint32 lastCommitMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};
