#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../../brand/tokens/BrandColours.h"
#include "../UserSettings.h"

#if SG_HAS_BRAND_FONTS
 #include "BinaryData.h"
#endif

// Theme tokens for the whole UI (UI_BRAND.md 2 and 3). Never hardcode a colour outside this file.
// Two themes: Dark (brand kit, default) and Classic (the previous light Swiss design, pixel-identical).
// A theme consists of colours and fonts only; shapes and drawing code are the same for both.
//
// All tokens are OPAQUE (overlays on canvas precomputed), because the drawing code calls withAlpha()
// on tokens and withAlpha replaces the alpha value instead of multiplying it.
//
// Message thread only (state is a plain static value without synchronisation).
namespace ui
{
    struct Palette
    {
        juce::Colour canvas;           // window background, waveform area                 (old: swiss::bg)
        juce::Colour surface1;         // panels, text fields
        juce::Colour surface2;         // pads, knob bodies, buttons
        juce::Colour surface3;         // hover state
        juce::Colour hairline;         // divider lines, borders                            (old: swiss::hair)
        juce::Colour hairlineStrong;   // focus border, knob track
        juce::Colour textPrimary;      // values, main text, active icons                   (old: swiss::ink)
        juce::Colour textSecondary;    // labels                                            (old: swiss::muted)
        juce::Colour textTertiary;     // placeholders, pad numbers, disabled
        juce::Colour red;              // selection, trim handles, active state, error       (old: swiss::accent)
        juce::Colour redTint;          // area behind the selection
        juce::Colour waveDim;          // waveform outside the selection
        juce::Colour shade;            // overlay for pressed buttons (Classic black, Dark paper)
    };

    enum class ThemeId { dark, classic };

    namespace detail
    {
        inline Palette makeDark()
        {
            const auto canvas = juce::Colour (0xff0b0b0c);
            const auto paper  = sg::brand::paper;
            const auto red    = sg::brand::grabRed;

            Palette p;
            p.canvas         = canvas;
            p.surface1       = juce::Colour (0xff131314);
            p.surface2       = juce::Colour (0xff1a1a1c);
            p.surface3       = juce::Colour (0xff242426);
            p.hairline       = canvas.interpolatedWith (paper, 0.10f);
            p.hairlineStrong = canvas.interpolatedWith (paper, 0.22f);
            p.textPrimary    = paper;
            p.textSecondary  = sg::brand::subOnDark;   // #8F8F8A
            p.textTertiary   = juce::Colour (0xff5a5a57);
            p.red            = red;
            p.redTint        = canvas.interpolatedWith (red, sg::brand::selTintAlpha);
            p.waveDim        = canvas.interpolatedWith (paper, sg::brand::dimAlphaDark);
            p.shade          = paper;
            return p;
        }

        // Classic: exactly the previous swiss:: values. Tokens that did not exist before
        // are derived from canvas/ink and currently unused in Classic.
        inline Palette makeClassic()
        {
            const auto canvas = juce::Colour (0xfffafaf8);
            const auto ink    = juce::Colour (0xff111111);
            const auto red    = juce::Colour (0xffff0028);   // KAZU-MARU red

            Palette p;
            p.canvas         = canvas;
            p.surface1       = canvas.interpolatedWith (ink, 0.03f);
            p.surface2       = canvas.interpolatedWith (ink, 0.05f);
            p.surface3       = canvas.interpolatedWith (ink, 0.09f);
            p.hairline       = juce::Colour (0xffd6d5cf);
            p.hairlineStrong = juce::Colour (0xffb5b4ae);
            p.textPrimary    = ink;
            p.textSecondary  = juce::Colour (0xff8a8984);
            p.textTertiary   = juce::Colour (0xffb5b4ae);
            p.red            = red;
            p.redTint        = canvas.interpolatedWith (red, 0.07f);
            p.waveDim        = juce::Colour (0xffd6d5cf);
            p.shade          = juce::Colour (0xff000000);
            return p;
        }

        inline ThemeId current = ThemeId::dark;

        inline const Palette& palette (ThemeId id)
        {
            static const Palette dark = makeDark();
            static const Palette classic = makeClassic();
            return id == ThemeId::classic ? classic : dark;
        }

        // ---- Fonts: load once from BinaryData. The typefaces live as long as a FontCache user
        // exists (SGLookAndFeel holds one), so they are not released only during static destruction
        // after the JUCE shutdown. ----
        struct FontCache
        {
            FontCache()
            {
               #if SG_HAS_BRAND_FONTS
                regular  = load (BinaryData::InterRegular_ttf,       BinaryData::InterRegular_ttfSize);
                medium   = load (BinaryData::InterMedium_ttf,        BinaryData::InterMedium_ttfSize);
                semiBold = load (BinaryData::InterSemiBold_ttf,      BinaryData::InterSemiBold_ttfSize);
                mono     = load (BinaryData::JetBrainsMonoMedium_ttf, BinaryData::JetBrainsMonoMedium_ttfSize);
               #endif
            }

            juce::Typeface::Ptr regular, medium, semiBold, mono;   // nullptr = not available -> default font

        private:
            [[maybe_unused]] static juce::Typeface::Ptr load (const char* data, int size)
            {
                if (data == nullptr || size <= 0)
                    return nullptr;
                return juce::Typeface::createSystemTypefaceFor (data, (size_t) size);
            }
        };

        using FontCachePtr = juce::SharedResourcePointer<FontCache>;

        inline juce::Font fontFrom (const juce::Typeface::Ptr& tf, float size, juce::FontOptions fallback, float kerning = 0.08f)
        {
            if (tf == nullptr)
                return juce::Font (fallback.withKerningFactor (kerning));
            return juce::Font (juce::FontOptions (tf).withHeight (size).withKerningFactor (kerning));
        }

    }

    // ---- Access to the active theme ----
    inline const Palette& c()       { return detail::palette (detail::current); }
    inline ThemeId theme()          { return detail::current; }
    inline juce::Colour transparent() { return juce::Colours::transparentBlack; }

    // The theme choice is a user setting (not plugin state): settings.xml in the user data folder.
    // Without a file, Dark stays.
    inline bool themeIsTransient = false;   // true: snapshot mode, do not load the saved choice

    inline void loadThemeSetting()
    {
        if (themeIsTransient)
            return;

        detail::current = sg::settings::get ("theme", "dark") == "classic" ? ThemeId::classic : ThemeId::dark;
    }

    // Sets the theme for this process only, without saving (snapshot mode of the app)
    inline void setThemeTransient (ThemeId id) { detail::current = id; themeIsTransient = true; }

    // Sets the theme and saves the choice. The caller must trigger the repaint (applyTheme of the
    // LookAndFeels/editor, waveform images).
    inline void setTheme (ThemeId id)
    {
        detail::current = id;
        sg::settings::set ("theme", id == ThemeId::classic ? "classic" : "dark");
    }

    // ---- Fonts (kerning 0.08 as before) ----
    // Classic: default font (strong = bold), Dark: Inter Medium / Inter SemiBold.
    inline juce::Font label (float size = 10.0f)
    {
        if (theme() == ThemeId::classic)
            return juce::Font (juce::FontOptions (size).withKerningFactor (0.08f));

        detail::FontCachePtr fonts;
        return detail::fontFrom (fonts->medium, size, juce::FontOptions (size));
    }

    inline juce::Font strong (float size)
    {
        if (theme() == ThemeId::classic)
            return juce::Font (juce::FontOptions (size, juce::Font::bold).withKerningFactor (0.08f));

        detail::FontCachePtr fonts;
        return detail::fontFrom (fonts->semiBold, size, juce::FontOptions (size, juce::Font::bold));
    }

    // Mono for values (Dark: JetBrains Mono Medium, Classic: the previous label font)
    // Mono is already set wide: no extra tracking by default (labels may pass 0.08)
    inline juce::Font mono (float size, float kerning = 0.0f)
    {
        if (theme() == ThemeId::classic)
            return label (size);

        detail::FontCachePtr fonts;
        return detail::fontFrom (fonts->mono, size, juce::FontOptions (size), kerning);
    }

    // Mono for readable lines (status line, overlay): JetBrains Mono (Dark) looks noticeably smaller at the same height
    // than the Classic font. Factor measured against cap height in the snapshot (Dark vs Classic).
    inline juce::Font monoUi (float size)
    {
        return mono (theme() == ThemeId::dark ? size * 1.15f : size);
    }

    // Default sans font for JUCE's own texts (Dark: Inter Medium, Classic: nullptr = system default)
    inline juce::Typeface::Ptr defaultSansTypeface()
    {
        if (theme() == ThemeId::classic)
            return nullptr;

        detail::FontCachePtr fonts;
        return fonts->medium;
    }
}
