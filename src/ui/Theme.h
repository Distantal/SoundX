#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sx
{
namespace theme
{
    // Flat palette (light by default; setDark() swaps every value for the night variant)
    inline juce::Colour window       { 0xffe6e9ee };
    inline juce::Colour panel        { 0xfff4f5f8 };
    inline juce::Colour panelAlt     { 0xffeceff3 };
    inline juce::Colour panelDeep    { 0xffdfe3e9 };
    inline juce::Colour white        { 0xffffffff };
    inline juce::Colour laneA        { 0xfffbfcfd };
    inline juce::Colour laneB        { 0xfff3f5f8 };
    inline juce::Colour line         { 0xffd0d4db };
    inline juce::Colour lineSoft     { 0xffe3e6eb };
    inline juce::Colour gridBar      { 0xffbfc5cf };
    inline juce::Colour gridBeat     { 0xffdde1e7 };
    inline juce::Colour gridSub      { 0xffeceef2 };
    inline juce::Colour text         { 0xff2a2f37 };
    inline juce::Colour textDim      { 0xff6a7280 };
    inline juce::Colour textFaint    { 0xff9aa2b0 };
    inline juce::Colour accent       { 0xff2f7cf6 };
    inline juce::Colour accentDark   { 0xff1f5fcc };
    inline juce::Colour accentSoft   { 0xffdbe8fd };
    inline juce::Colour accentHover  { 0xffeaf2fe };
    inline juce::Colour record       { 0xffe5484d };
    inline juce::Colour warn         { 0xfff0a400 };
    inline juce::Colour meterGreen   { 0xff3fa7e0 };
    inline juce::Colour meterHigh    { 0xff2f7cf6 };
    // selection: a warm peach instead of the accent blue, so a selected note never reads as
    // "playing / active" (the accent stays the colour of the playhead, tools and toggles)
    inline juce::Colour selectionFill { 0xffffd9c4 };
    inline juce::Colour selectionEdge { 0xffe07a4a };
    inline juce::Colour selectionText { 0xff7a3a14 };

    //== colours that do not follow the theme ==============================================
    // Everything above is swapped by setDark(). These are not: white marks drawn on the accent
    // colour or on a clip stay white in a night theme (text on a clip, fade handles, the gain
    // line), a tooltip is a dark bubble with white text in either mode, and the piano keyboard is
    // black and white in a night theme too - a "grey on grey" keyboard is unreadable.
    inline juce::Colour onAccent  { 0xffffffff };   // text / handles drawn on a coloured fill
    inline juce::Colour tooltipBg { 0xff343b45 };   // tooltips and the value tips on the timeline
    inline juce::Colour keyWhite  { 0xfff8f9fb };   // piano keyboard: the white keys
    inline juce::Colour keyBlack  { 0xff24272c };   // piano keyboard: the black keys
    inline juce::Colour keyLine   { 0xffccd2da };   // separators between the keys
    inline juce::Colour keyLabel  { 0xff5b6572 };   // "C4" style labels on the white keys
    inline juce::Colour keyDown   { 0xffb7d3fb };   // a white key held down (mouse / MIDI)

    inline bool dark = false;
    inline bool isDark() { return dark; }

    inline void setDark (bool on)
    {
        if (dark == on) return;
        dark = on;
        if (on)
        {
            // night mode: deeper than before, so the arrangement reads as one dark surface and the
            // clips stand out instead of the background competing with them
            window      = juce::Colour (0xff191c21);
            panel       = juce::Colour (0xff202429);
            panelAlt    = juce::Colour (0xff252a30);
            panelDeep   = juce::Colour (0xff13161a);
            white       = juce::Colour (0xff2a2f35);
            laneA       = juce::Colour (0xff1d2126);
            laneB       = juce::Colour (0xff191d22);
            line        = juce::Colour (0xff343a41);
            lineSoft    = juce::Colour (0xff272c33);
            gridBar     = juce::Colour (0xff3b4149);
            gridBeat    = juce::Colour (0xff2b3037);
            gridSub     = juce::Colour (0xff24282e);
            text        = juce::Colour (0xffe4e8ed);
            textDim     = juce::Colour (0xffa3abb6);
            textFaint   = juce::Colour (0xff737d88);
            accent      = juce::Colour (0xff4589ee);
            accentDark  = juce::Colour (0xff74a5f5);
            accentSoft  = juce::Colour (0xff24344f);
            accentHover = juce::Colour (0xff2b3a50);
            record      = juce::Colour (0xffe04a4f);
            warn        = juce::Colour (0xffe0a838);
            meterGreen  = juce::Colour (0xff3f9cc8);
            meterHigh   = juce::Colour (0xff4589ee);
            selectionFill = juce::Colour (0xff61381f);
            selectionEdge = juce::Colour (0xffd98f63);
            selectionText = juce::Colour (0xfff7dcc9);
        }
        else
        {
            window      = juce::Colour (0xffe6e9ee);
            panel       = juce::Colour (0xfff4f5f8);
            panelAlt    = juce::Colour (0xffeceff3);
            panelDeep   = juce::Colour (0xffdfe3e9);
            white       = juce::Colour (0xffffffff);
            laneA       = juce::Colour (0xfffbfcfd);
            laneB       = juce::Colour (0xfff3f5f8);
            line        = juce::Colour (0xffd0d4db);
            lineSoft    = juce::Colour (0xffe3e6eb);
            gridBar     = juce::Colour (0xffbfc5cf);
            gridBeat    = juce::Colour (0xffdde1e7);
            gridSub     = juce::Colour (0xffeceef2);
            text        = juce::Colour (0xff2a2f37);
            textDim     = juce::Colour (0xff6a7280);
            textFaint   = juce::Colour (0xff9aa2b0);
            accent      = juce::Colour (0xff2f7cf6);
            accentDark  = juce::Colour (0xff1f5fcc);
            accentSoft  = juce::Colour (0xffdbe8fd);
            accentHover = juce::Colour (0xffeaf2fe);
            record      = juce::Colour (0xffe5484d);
            warn        = juce::Colour (0xfff0a400);
            meterGreen  = juce::Colour (0xff3fa7e0);
            meterHigh   = juce::Colour (0xff2f7cf6);
            selectionFill = juce::Colour (0xffffd9c4);
            selectionEdge = juce::Colour (0xffe07a4a);
            selectionText = juce::Colour (0xff7a3a14);
        }
    }

    // How much saturation a track colour keeps. The clips fill a large part of the arrangement, so
    // full strength track colours made the whole area look loud; this calms them down while the
    // hue still tells tracks apart.
    inline float trackSaturation = 0.62f;

    inline juce::Colour trackColour (juce::Colour c)
    {
        return c.withSaturation (juce::Colour (c).withSaturation (1.0f).getSaturation() * trackSaturation);
    }

    inline const juce::Array<juce::Colour>& trackPalette()
    {
        static const juce::Array<juce::Colour> p {
            trackColour (juce::Colour (0xff2f7cf6)), trackColour (juce::Colour (0xff4fa3e8)),
            trackColour (juce::Colour (0xff3f63c4)), trackColour (juce::Colour (0xff5b7aa6)),
            trackColour (juce::Colour (0xff3fb0c4)), trackColour (juce::Colour (0xff7d8fb3)),
            trackColour (juce::Colour (0xff6c87d9)), trackColour (juce::Colour (0xff8896a8)) };
        return p;
    }

    // bus / effect-send tracks use their own accent colours so they read differently
    inline const juce::Colour busTrackColour  { juce::Colour (0xff1f9e7a).withSaturation (0.62f) };   // green-teal
    inline const juce::Colour sendTrackColour { juce::Colour (0xffa55cd6).withSaturation (0.58f) };   // purple
    inline const juce::Colour masterTrackColour { juce::Colour (0xffd98a2b).withSaturation (0.62f) };  // amber: the master bus row

    //== fonts ==============================================================================
    // One family for the whole UI: JUCE's "system-ui" placeholder. It resolves to the platform's
    // own system font - SF on macOS (with the system's Chinese fallback) and Segoe UI on Windows -
    // and it keeps working for bold text (JUCE looks the style up by the resolved face).
    //
    // Naming "Segoe UI" explicitly was a Windows-only assumption: macOS does not have that font,
    // so every label silently fell back to a different face and the Chinese glyphs came from yet
    // another one, which is the mismatched font of the mac port.
    inline juce::String uiFontName()   { return juce::Font::getSystemUIFontName(); }
    inline juce::String monoFontName() { return juce::Font::getDefaultMonospacedFontName(); }  // Menlo / Consolas

    inline juce::Font font (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (uiFontName(), h, bold ? juce::Font::bold : juce::Font::plain));
    }

    inline juce::Font mono (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (monoFontName(), h, bold ? juce::Font::bold : juce::Font::plain));
    }
}

class SXLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SXLookAndFeel() { applyColours(); }

    // re-applies every themed colour; call after setDark() so menus / sliders / editors update too
    void applyColours()
    {
        using namespace theme;
        // Stock JUCE widgets we do not draw ourselves (scan-folder list, file chooser, alert
        // windows) fall back to the LookAndFeel colour scheme, which defaults to the dark one.
        // Left alone that gives dark text on a dark panel inside our light UI.
        setColourScheme (dark ? getDarkColourScheme() : getLightColourScheme());
        setColour (juce::ResizableWindow::backgroundColourId, dark ? panel : white);
        setColour (juce::FileSearchPathListComponent::backgroundColourId, dark ? panelAlt : white);
        setColour (juce::ResizableWindow::backgroundColourId, window);
        setColour (juce::DocumentWindow::textColourId, text);
        setColour (juce::Label::textColourId, text);
        setColour (juce::TextButton::buttonColourId, white);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, onAccent);
        setColour (juce::ComboBox::backgroundColourId, white);
        setColour (juce::ComboBox::outlineColourId, line);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::arrowColourId, textDim);
        setColour (juce::PopupMenu::backgroundColourId, white);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour (juce::PopupMenu::highlightedTextColourId, onAccent);
        setColour (juce::TextEditor::backgroundColourId, white);
        setColour (juce::TextEditor::textColourId, text);
        setColour (juce::TextEditor::outlineColourId, line);
        setColour (juce::TextEditor::focusedOutlineColourId, accent);
        setColour (juce::TextEditor::highlightColourId, accentSoft);
        setColour (juce::TextEditor::highlightedTextColourId, text);
        setColour (juce::CaretComponent::caretColourId, accent);
        setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xffb7bdc8));
        setColour (juce::Slider::thumbColourId, white);
        setColour (juce::Slider::trackColourId, accent);
        setColour (juce::Slider::backgroundColourId, panelDeep);
        setColour (juce::Slider::textBoxTextColourId, text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ListBox::backgroundColourId, white);
        setColour (juce::ListBox::textColourId, text);
        setColour (juce::ListBox::outlineColourId, line);
        setColour (juce::AlertWindow::backgroundColourId, panel);
        setColour (juce::AlertWindow::textColourId, text);
        setColour (juce::AlertWindow::outlineColourId, line);
        setColour (juce::TooltipWindow::backgroundColourId, tooltipBg);
        setColour (juce::TooltipWindow::textColourId, onAccent);
        setColour (juce::TooltipWindow::outlineColourId, tooltipBg);
        setColour (juce::ToggleButton::textColourId, text);
        setColour (juce::ToggleButton::tickColourId, accent);
        setColour (juce::TableHeaderComponent::backgroundColourId, panelAlt);
        setColour (juce::TableHeaderComponent::textColourId, text);
        setColour (juce::TableHeaderComponent::outlineColourId, line);
        setColour (juce::TabbedButtonBar::tabOutlineColourId, line);
        setColour (juce::ProgressBar::backgroundColourId, panelDeep);
        setColour (juce::ProgressBar::foregroundColourId, accent);
        setColour (juce::FileChooserDialogBox::titleTextColourId, text);
        setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, accentSoft);
        setColour (juce::DirectoryContentsDisplayComponent::textColourId, text);
    }

    // JUCE-drawn pieces (alert windows, text editors, anything we do not paint ourselves) ask for
    // the "default sans serif", which JUCE would resolve to "Lucida Grande" on macOS - a face
    // without Chinese glyphs. Send those requests through the same system font as the rest of the
    // UI, so the whole window (menus, dialogs, labels) is one family on every platform.
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& font) override
    {
        juce::Font f (font);
        if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
            f.setTypefaceName (theme::uiFontName());
        return juce::LookAndFeel_V4::getTypefaceForFont (f);
    }

    juce::Font getPopupMenuFont() override { return theme::font (14.0f); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return theme::font (13.0f); }
    juce::Font getLabelFont (juce::Label& l) override
    {
        auto f = l.getFont();
        return f.getHeight() < 1.0f ? theme::font (13.0f) : f;
    }
    juce::Font getTextButtonFont (juce::TextButton&, int h) override { return theme::font (juce::jmin (13.0f, (float) h * 0.6f)); }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto base = b.findColour (b.getToggleState() ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
        if (b.getToggleState())
        {
            if (down) base = base.darker (0.15f);
            else if (over) base = base.brighter (0.08f);
        }
        else
        {
            base = bg;
            if (down) base = theme::accentSoft;
            else if (over) base = theme::accentHover;
        }
        g.setColour (base);
        g.fillRoundedRectangle (r, 4.0f);
        if (! b.getToggleState())
        {
            g.setColour (theme::line);
            g.drawRoundedRectangle (r, 4.0f, 1.0f);
        }
    }

    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (theme::white);
        g.setColour (theme::line);
        g.drawRect (0, 0, w, h, 1);
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcut, const juce::Drawable* icon, const juce::Colour* textColour) override
    {
        if (isSeparator)
        {
            g.setColour (theme::lineSoft);
            g.fillRect (area.reduced (8, 0).withHeight (1).withY (area.getCentreY()));
            return;
        }
        auto r = area.reduced (3, 1);
        const bool hi = isHighlighted && isActive;
        if (hi)
        {
            g.setColour (theme::accent);
            g.fillRoundedRectangle (r.toFloat(), 3.0f);
        }
        auto col = hi ? theme::onAccent : (isActive ? theme::text : theme::textFaint);
        if (textColour != nullptr && ! hi) col = *textColour;
        g.setColour (col);
        g.setFont (theme::font (14.0f));
        auto tr = r.reduced (24, 0);
        if (isTicked)
        {
            juce::Path p;
            // with an icon the tick moves to the right so the two never overlap
            const float cx = icon != nullptr ? (float) r.getRight() - 12.0f : (float) r.getX() + 6.0f;
            auto c = juce::Rectangle<float> (cx, (float) r.getCentreY() - 4.0f, 10.0f, 8.0f);
            p.startNewSubPath (c.getX(), c.getCentreY());
            p.lineTo (c.getX() + 3.5f, c.getBottom());
            p.lineTo (c.getRight(), c.getY());
            g.strokePath (p, juce::PathStrokeType (1.8f));
        }
        if (icon != nullptr)
        {
            // item icons (e.g. the track colour swatches) drawn in the left gutter
            const float ih = (float) juce::jlimit (8, juce::jmax (8, r.getHeight() - 8), 14);
            juce::Rectangle<float> iconArea ((float) r.getX() + 5.0f, (float) r.getCentreY() - ih * 0.5f, 18.0f, ih);
            icon->drawWithin (g, iconArea, juce::RectanglePlacement::stretchToFit, 1.0f);
        }
        g.setColour (col);
        g.drawFittedText (text, tr.withTrimmedRight (hasSubMenu ? 14 : 0), juce::Justification::centredLeft, 1);
        if (shortcut.isNotEmpty())
        {
            g.setColour (hi ? theme::onAccent.withAlpha (0.85f) : theme::textFaint);
            g.setFont (theme::font (12.5f));
            g.drawText (shortcut, r.reduced (8, 0), juce::Justification::centredRight);
        }
        if (hasSubMenu)
        {
            juce::Path p;
            auto cx = (float) r.getRight() - 10.0f, cy = (float) r.getCentreY();
            p.startNewSubPath (cx - 2.0f, cy - 4.0f);
            p.lineTo (cx + 2.0f, cy);
            p.lineTo (cx - 2.0f, cy + 4.0f);
            g.setColour (col);
            g.strokePath (p, juce::PathStrokeType (1.5f));
        }
    }

    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardHeight, int& w, int& h) override
    {
        if (isSeparator) { w = 50; h = 7; return; }
        auto f = getPopupMenuFont();
        w = juce::GlyphArrangement::getStringWidthInt (f, text) + 60;
        h = 25;
        juce::ignoreUnused (standardHeight);
    }

    int getPopupMenuBorderSize() override { return 2; }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (box.hasKeyboardFocus (true) ? theme::accent : theme::line);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        juce::Path p;
        auto cx = (float) w - 12.0f, cy = (float) h * 0.5f;
        p.startNewSubPath (cx - 3.5f, cy - 1.5f);
        p.lineTo (cx, cy + 2.0f);
        p.lineTo (cx + 3.5f, cy - 1.5f);
        g.setColour (theme::textDim);
        g.strokePath (p, juce::PathStrokeType (1.5f));
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (6, 1, box.getWidth() - 24, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           const juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        using namespace theme;
        const bool enabled = s.isEnabled();
        if (style == juce::Slider::LinearVertical)
        {
            const float cx = (float) x + (float) w * 0.5f;
            g.setColour (panelDeep);
            g.fillRoundedRectangle (cx - 2.0f, (float) y, 4.0f, (float) h, 2.0f);
            g.setColour (enabled ? accent : textFaint);
            g.fillRoundedRectangle (cx - 2.0f, pos, 4.0f, (float) (y + h) - pos, 2.0f);
            auto th = juce::Rectangle<float> (cx - 12.0f, pos - 7.0f, 24.0f, 14.0f);
            g.setColour (white);
            g.fillRoundedRectangle (th, 3.0f);
            g.setColour (line);
            g.drawRoundedRectangle (th, 3.0f, 1.0f);
            g.setColour (accent);
            g.fillRect (juce::Rectangle<float> (th.getX() + 4.0f, th.getCentreY() - 0.75f, th.getWidth() - 8.0f, 1.5f));
        }
        else if (style == juce::Slider::LinearHorizontal || style == juce::Slider::LinearBar)
        {
            const float cy = (float) y + (float) h * 0.5f;
            g.setColour (panelDeep);
            g.fillRoundedRectangle ((float) x, cy - 2.0f, (float) w, 4.0f, 2.0f);
            g.setColour (enabled ? accent : textFaint);
            const float zero = s.getMinimum() < 0.0 && s.getMaximum() > 0.0
                                   ? (float) x + (float) w * (float) s.valueToProportionOfLength (0.0)
                                   : (float) x;
            g.fillRect (juce::Rectangle<float> (juce::jmin (zero, pos), cy - 2.0f, std::abs (pos - zero), 4.0f));
            g.setColour (white);
            g.fillEllipse (pos - 6.0f, cy - 6.0f, 12.0f, 12.0f);
            g.setColour (accent);
            g.drawEllipse (pos - 6.0f, cy - 6.0f, 12.0f, 12.0f, 1.5f);
        }
        else
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
        }
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s) override
    {
        using namespace theme;
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
        const float rad = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f;
        const auto c = r.getCentre();
        const float ang = a0 + pos * (a1 - a0);
        juce::Path bg;
        bg.addCentredArc (c.x, c.y, rad - 1.5f, rad - 1.5f, 0.0f, a0, a1, true);
        g.setColour (panelDeep);
        g.strokePath (bg, juce::PathStrokeType (3.0f));
        juce::Path val;
        const float mid = (s.getMinimum() < 0.0 && s.getMaximum() > 0.0) ? a0 + (float) s.valueToProportionOfLength (0.0) * (a1 - a0) : a0;
        val.addCentredArc (c.x, c.y, rad - 1.5f, rad - 1.5f, 0.0f, juce::jmin (mid, ang), juce::jmax (mid, ang), true);
        g.setColour (s.isEnabled() ? accent : textFaint);
        g.strokePath (val, juce::PathStrokeType (3.0f));
        g.setColour (white);
        g.fillEllipse (c.x - rad + 5.0f, c.y - rad + 5.0f, (rad - 5.0f) * 2.0f, (rad - 5.0f) * 2.0f);
        g.setColour (line);
        g.drawEllipse (c.x - rad + 5.0f, c.y - rad + 5.0f, (rad - 5.0f) * 2.0f, (rad - 5.0f) * 2.0f, 1.0f);
        juce::Path tick;
        tick.addLineSegment ({ 0.0f, -(rad - 7.0f), 0.0f, -(rad - 11.0f) }, 1.0f);
        g.setColour (accent);
        g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                      juce::AffineTransform::rotation (ang).translated (c.x, c.y));
    }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar& sb, int x, int y, int w, int h, bool vertical,
                        int thumbStart, int thumbSize, bool over, bool down) override
    {
        juce::ignoreUnused (sb);
        juce::Rectangle<float> t = vertical ? juce::Rectangle<float> ((float) x + 3.0f, (float) (y + thumbStart), (float) w - 6.0f, (float) thumbSize)
                                            : juce::Rectangle<float> ((float) (x + thumbStart), (float) y + 3.0f, (float) thumbSize, (float) h - 6.0f);
        // follows the theme: a light grey thumb on a dark scrollbar is the brightest thing on a
        // night-theme screen
        const auto thumb = theme::line.interpolatedWith (theme::textFaint, 0.55f);
        g.setColour (down ? thumb.darker (0.15f) : over ? thumb.brighter (0.08f) : thumb);
        g.fillRoundedRectangle (t, 3.0f);
    }

    int getDefaultScrollbarWidth() override { return 12; }

    void drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& te) override
    {
        auto r = juce::Rectangle<float> (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f);
        g.setColour (te.hasKeyboardFocus (true) ? theme::accent : theme::line);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }

    void fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& te) override
    {
        g.setColour (te.findColour (juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle (0.0f, 0.0f, (float) w, (float) h, 3.0f);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool) override
    {
        auto r = b.getLocalBounds().toFloat();
        auto box = juce::Rectangle<float> (2.0f, r.getCentreY() - 8.0f, 16.0f, 16.0f);
        g.setColour (b.getToggleState() ? theme::accent : theme::white);
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (b.getToggleState() ? theme::accent : (over ? theme::textDim : theme::line));
        g.drawRoundedRectangle (box.reduced (0.5f), 3.0f, 1.0f);
        if (b.getToggleState())
        {
            juce::Path p;
            p.startNewSubPath (box.getX() + 4.0f, box.getCentreY());
            p.lineTo (box.getX() + 7.0f, box.getBottom() - 4.5f);
            p.lineTo (box.getRight() - 3.5f, box.getY() + 4.5f);
            g.setColour (theme::onAccent);
            g.strokePath (p, juce::PathStrokeType (1.8f));
        }
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f));
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft (24), juce::Justification::centredLeft, 1);
    }

    void drawProgressBar (juce::Graphics& g, juce::ProgressBar& pb, int w, int h, double progress, const juce::String& text) override
    {
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
        g.setColour (theme::panelDeep);
        g.fillRoundedRectangle (r, 4.0f);
        if (progress >= 0.0)
        {
            g.setColour (theme::accent);
            g.fillRoundedRectangle (r.withWidth (r.getWidth() * (float) progress), 4.0f);
        }
        g.setColour (theme::text);
        g.setFont (theme::font (12.5f));
        g.drawText (text, r, juce::Justification::centred);
        juce::ignoreUnused (pb);
    }

    void drawMenuBarBackground (juce::Graphics& g, int w, int h, bool, juce::MenuBarComponent&) override
    {
        g.fillAll (theme::white);
        g.setColour (theme::line);
        g.fillRect (0, h - 1, w, 1);
    }

    juce::Font getMenuBarFont (juce::MenuBarComponent&, int, const juce::String&) override { return theme::font (13.5f); }

    int getMenuBarItemWidth (juce::MenuBarComponent& mb, int itemIndex, const juce::String& itemText) override
    {
        juce::ignoreUnused (mb, itemIndex);
        return juce::GlyphArrangement::getStringWidthInt (theme::font (13.5f), itemText) + 24;
    }

    void drawMenuBarItem (juce::Graphics& g, int w, int h, int, const juce::String& text, bool over, bool open, bool,
                          juce::MenuBarComponent&) override
    {
        auto r = juce::Rectangle<int> (0, 0, w, h).reduced (2, 3);
        if (open || over)
        {
            g.setColour (open ? theme::accentSoft : theme::accentHover);
            g.fillRoundedRectangle (r.toFloat(), 4.0f);
        }
        g.setColour (open ? theme::accentDark : theme::text);
        g.setFont (theme::font (13.5f));
        g.drawText (text, 0, 0, w, h, juce::Justification::centred);
    }

    void drawDocumentWindowTitleBar (juce::DocumentWindow& w, juce::Graphics& g, int width, int height, int tbl, int tbw,
                                     const juce::Image*, bool) override
    {
        g.fillAll (theme::panel);
        g.setColour (theme::line);
        g.fillRect (0, height - 1, width, 1);
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        g.drawText (w.getName(), tbl, 0, width - tbl - tbw, height, juce::Justification::centredLeft);
    }
};
}
