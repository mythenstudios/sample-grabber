#pragma once
#include "PluginProcessor.h"
#include "ui/SGLookAndFeel.h"
#include <functional>
#include <memory>

// Rotary knob with title on top and value below. The value arc starts at the neutral point
// (e.g. 0 dB, 0 semitones, 100 % speed), not at the left stop.
// Optional: custom value text (setFormatter) and warning state (setWarning: value in accent red).
class KnobCell : public juce::Component
{
public:
    KnobCell (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID,
              const juce::String& title, const juce::String& unitText,
              int decimalPlaces, bool signedValue, double neutralValue)
        : titleText (title), unit (unitText), decimals (decimalPlaces), showSign (signedValue)
    {
        knob.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
        knob.setMouseDragSensitivity (200);
        knob.getProperties().set ("neutral", neutralValue);
        knob.onValueChange = [this] { repaint(); };
        addAndMakeVisible (knob);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, paramID, knob);

        if (auto* p = apvts.getParameter (paramID))
            knob.setDoubleClickReturnValue (true, (double) p->convertFrom0to1 (p->getDefaultValue()));
    }

    // Custom display text for specific values, e.g. "-INF DB" at the stop. Empty string -> default display (number + unit).
    void setFormatter (std::function<juce::String (double)> f)
    {
        formatter = std::move (f);
        repaint();
    }

    void setWarning (bool shouldWarn)
    {
        if (warning != shouldWarn)
        {
            warning = shouldWarn;
            repaint();
        }
    }

    bool isWarning() const { return warning; }
    double getValue() const { return knob.getValue(); }

    void resized() override
    {
        knob.setBounds (getWidth() / 2 - 27, 22, 54, 54);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (ui::c().textSecondary);
        g.setFont (ui::label (9.0f));
        g.drawText (titleText, juce::Rectangle<int> (0, 6, getWidth(), 14), juce::Justification::centred);

        g.setColour (warning ? ui::c().red : ui::c().textPrimary);
        g.setFont (ui::strong (10.0f));
        auto text = formatter != nullptr ? formatter (knob.getValue()) : juce::String();
        if (text.isEmpty())
            text = defaultText();
        g.drawText (text, juce::Rectangle<int> (0, getHeight() - 17, getWidth(), 14), juce::Justification::centred);
    }

private:
    juce::String defaultText() const
    {
        double v = knob.getValue();
        if (std::abs (v) < 0.5 * std::pow (10.0, -decimals))
            v = 0.0;   // avoid "-0.0" from rounding residue of the skew mapping

        juce::String text = juce::String (v, decimals);
        if (showSign && v > 0.0)
            text = "+" + text;
        return text + unit;
    }

    juce::String titleText, unit;
    int decimals;
    bool showSign;
    bool warning = false;
    std::function<juce::String (double)> formatter;

    juce::Slider knob;   // declared before the attachment -> destroyed after it
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KnobCell)
};
