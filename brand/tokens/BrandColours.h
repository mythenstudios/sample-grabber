// Sample Grabber - brand colour tokens for the JUCE UI.
// Keep RED for selection / trim handles / active state only.
#pragma once
#include <juce_graphics/juce_graphics.h>

namespace sg::brand
{
    // Core
    inline const juce::Colour ink       { 0xff0b0b0c };  // primary on light, canvas on dark
    inline const juce::Colour paper     { 0xfff5f4ef };  // primary on dark, canvas on light
    inline const juce::Colour grabRed   { 0xffff3b21 };  // selection on dark
    inline const juce::Colour grabRedL  { 0xfff2301a };  // selection on light (slightly deeper)

    // Secondary text
    inline const juce::Colour subOnLight { 0xff6e6e6a };
    inline const juce::Colour subOnDark  { 0xff8f8f8a };

    // Derived
    constexpr float dimAlphaLight = 0.22f;  // unselected waveform on light
    constexpr float dimAlphaDark  = 0.26f;  // unselected waveform on dark
    constexpr float ruleAlpha     = 0.15f;  // hairlines
    constexpr float selTintAlpha  = 0.07f;  // fill behind the selected region
}
