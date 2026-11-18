#pragma once

#include "Theme.h"

namespace sx
{
enum class Icon
{
    Play, Stop, Record, Rewind, Loop, Metronome, Plus, Minus, Pointer, Pencil, Eraser, Scissors, Mixer, Piano,
    Inspector, Settings, Plugin, Keyboard, Automation, Export, CountIn, Folder, Undo, Redo, Menu, Close, Expand, Merge, MuteTool,
    Follow, Chord, Marker, Dice
};

inline void drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> r, juce::Colour c)
{
    using juce::Path;
    using juce::PathStrokeType;
    g.setColour (c);
    r = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()), juce::jmin (r.getWidth(), r.getHeight()));
    const float x = r.getX(), y = r.getY(), w = r.getWidth(), h = r.getHeight();
    const PathStrokeType st (1.6f, PathStrokeType::curved, PathStrokeType::rounded);

    switch (icon)
    {
        case Icon::Play:
        {
            Path p;
            p.addTriangle (x + w * 0.22f, y + h * 0.12f, x + w * 0.22f, y + h * 0.88f, x + w * 0.88f, y + h * 0.5f);
            g.fillPath (p);
            break;
        }
        case Icon::Stop:
            g.fillRoundedRectangle (x + w * 0.2f, y + h * 0.2f, w * 0.6f, h * 0.6f, 1.5f);
            break;
        case Icon::Record:
            g.fillEllipse (x + w * 0.18f, y + h * 0.18f, w * 0.64f, h * 0.64f);
            break;
        case Icon::Rewind:
        {
            Path p;
            p.addTriangle (x + w * 0.85f, y + h * 0.2f, x + w * 0.85f, y + h * 0.8f, x + w * 0.35f, y + h * 0.5f);
            g.fillPath (p);
            g.fillRect (x + w * 0.15f, y + h * 0.2f, w * 0.12f, h * 0.6f);
            break;
        }
        case Icon::Loop:
        {
            Path p;
            p.addRoundedRectangle (x + w * 0.12f, y + h * 0.28f, w * 0.76f, h * 0.44f, h * 0.2f);
            g.strokePath (p, st);
            Path a;
            a.addTriangle (x + w * 0.62f, y + h * 0.12f, x + w * 0.62f, y + h * 0.44f, x + w * 0.84f, y + h * 0.28f);
            g.fillPath (a);
            break;
        }
        case Icon::Metronome:
        {
            Path p;
            p.startNewSubPath (x + w * 0.3f, y + h * 0.88f);
            p.lineTo (x + w * 0.42f, y + h * 0.14f);
            p.lineTo (x + w * 0.58f, y + h * 0.14f);
            p.lineTo (x + w * 0.7f, y + h * 0.88f);
            p.closeSubPath();
            g.strokePath (p, st);
            g.drawLine (x + w * 0.5f, y + h * 0.7f, x + w * 0.66f, y + h * 0.3f, 1.6f);
            break;
        }
        case Icon::Plus:
            g.fillRoundedRectangle (x + w * 0.45f, y + h * 0.18f, w * 0.1f, h * 0.64f, 1.0f);
            g.fillRoundedRectangle (x + w * 0.18f, y + h * 0.45f, w * 0.64f, h * 0.1f, 1.0f);
            break;
        case Icon::Minus:
            g.fillRoundedRectangle (x + w * 0.18f, y + h * 0.45f, w * 0.64f, h * 0.1f, 1.0f);
            break;
        case Icon::Pointer:
        {
            Path p;
            p.startNewSubPath (x + w * 0.25f, y + h * 0.12f);
            p.lineTo (x + w * 0.25f, y + h * 0.82f);
            p.lineTo (x + w * 0.43f, y + h * 0.65f);
            p.lineTo (x + w * 0.58f, y + h * 0.92f);
            p.lineTo (x + w * 0.68f, y + h * 0.86f);
            p.lineTo (x + w * 0.54f, y + h * 0.6f);
            p.lineTo (x + w * 0.78f, y + h * 0.58f);
            p.closeSubPath();
            g.fillPath (p);
            break;
        }
        case Icon::Pencil:
        {
            Path p;
            p.startNewSubPath (x + w * 0.18f, y + h * 0.82f);
            p.lineTo (x + w * 0.24f, y + h * 0.58f);
            p.lineTo (x + w * 0.66f, y + h * 0.16f);
            p.lineTo (x + w * 0.84f, y + h * 0.34f);
            p.lineTo (x + w * 0.42f, y + h * 0.76f);
            p.closeSubPath();
            g.fillPath (p);
            break;
        }
        case Icon::Eraser:
        {
            Path p;
            p.addRoundedRectangle (x + w * 0.15f, y + h * 0.4f, w * 0.7f, h * 0.36f, 2.0f);
            g.fillPath (p, juce::AffineTransform::rotation (-0.6f, x + w * 0.5f, y + h * 0.58f));
            break;
        }
        case Icon::Scissors:
        {
            g.drawEllipse (x + w * 0.12f, y + h * 0.62f, w * 0.24f, h * 0.24f, 1.5f);
            g.drawEllipse (x + w * 0.64f, y + h * 0.62f, w * 0.24f, h * 0.24f, 1.5f);
            g.drawLine (x + w * 0.22f, y + h * 0.64f, x + w * 0.66f, y + h * 0.1f, 1.6f);
            g.drawLine (x + w * 0.78f, y + h * 0.64f, x + w * 0.34f, y + h * 0.1f, 1.6f);
            break;
        }
        case Icon::Mixer:
            for (int i = 0; i < 3; ++i)
            {
                const float cx = x + w * (0.22f + 0.28f * (float) i);
                g.fillRoundedRectangle (cx - 0.9f, y + h * 0.12f, 1.8f, h * 0.76f, 0.9f);
                const float ky = y + h * (i == 1 ? 0.28f : i == 0 ? 0.6f : 0.45f);
                g.fillRoundedRectangle (cx - w * 0.1f, ky - h * 0.06f, w * 0.2f, h * 0.12f, 1.5f);
            }
            break;
        case Icon::Piano:
        {
            g.drawRoundedRectangle (x + w * 0.1f, y + h * 0.18f, w * 0.8f, h * 0.64f, 2.0f, 1.5f);
            for (int i = 1; i < 4; ++i)
                g.drawLine (x + w * (0.1f + 0.2f * (float) i), y + h * 0.5f, x + w * (0.1f + 0.2f * (float) i), y + h * 0.82f, 1.2f);
            for (int i = 0; i < 3; ++i)
                if (i != 1) g.fillRect (x + w * (0.25f + 0.2f * (float) i), y + h * 0.18f, w * 0.1f, h * 0.34f);
            break;
        }
        case Icon::Inspector:
            g.drawRoundedRectangle (x + w * 0.1f, y + h * 0.16f, w * 0.8f, h * 0.68f, 2.0f, 1.5f);
            g.fillRect (x + w * 0.1f, y + h * 0.16f, w * 0.28f, h * 0.68f);
            break;
        case Icon::Settings:
        {
            g.drawEllipse (x + w * 0.3f, y + h * 0.3f, w * 0.4f, h * 0.4f, 1.6f);
            for (int i = 0; i < 8; ++i)
            {
                const float a = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
                g.drawLine (x + w * 0.5f + std::cos (a) * w * 0.28f, y + h * 0.5f + std::sin (a) * h * 0.28f,
                            x + w * 0.5f + std::cos (a) * w * 0.4f, y + h * 0.5f + std::sin (a) * h * 0.4f, 2.2f);
            }
            break;
        }
        case Icon::Plugin:
        {
            g.drawRoundedRectangle (x + w * 0.2f, y + h * 0.28f, w * 0.6f, h * 0.6f, 2.0f, 1.5f);
            g.fillRect (x + w * 0.34f, y + h * 0.08f, w * 0.1f, h * 0.2f);
            g.fillRect (x + w * 0.56f, y + h * 0.08f, w * 0.1f, h * 0.2f);
            break;
        }
        case Icon::Keyboard:
        {
            g.drawRoundedRectangle (x + w * 0.08f, y + h * 0.26f, w * 0.84f, h * 0.48f, 2.0f, 1.4f);
            for (int r = 0; r < 2; ++r)
                for (int i = 0; i < 4; ++i)
                    g.fillRect (x + w * (0.2f + 0.16f * (float) i), y + h * (0.38f + 0.14f * (float) r), w * 0.1f, h * 0.07f);
            break;
        }
        case Icon::Automation:
        {
            Path p;
            p.startNewSubPath (x + w * 0.1f, y + h * 0.75f);
            p.lineTo (x + w * 0.35f, y + h * 0.3f);
            p.lineTo (x + w * 0.6f, y + h * 0.62f);
            p.lineTo (x + w * 0.9f, y + h * 0.22f);
            g.strokePath (p, st);
            g.fillEllipse (x + w * 0.29f, y + h * 0.24f, w * 0.12f, h * 0.12f);
            g.fillEllipse (x + w * 0.54f, y + h * 0.56f, w * 0.12f, h * 0.12f);
            break;
        }
        case Icon::Export:
        {
            Path p;
            p.startNewSubPath (x + w * 0.5f, y + h * 0.12f);
            p.lineTo (x + w * 0.5f, y + h * 0.6f);
            p.startNewSubPath (x + w * 0.3f, y + h * 0.4f);
            p.lineTo (x + w * 0.5f, y + h * 0.6f);
            p.lineTo (x + w * 0.7f, y + h * 0.4f);
            p.startNewSubPath (x + w * 0.15f, y + h * 0.65f);
            p.lineTo (x + w * 0.15f, y + h * 0.88f);
            p.lineTo (x + w * 0.85f, y + h * 0.88f);
            p.lineTo (x + w * 0.85f, y + h * 0.65f);
            g.strokePath (p, st);
            break;
        }
        case Icon::CountIn:
            g.setFont (theme::font (h * 0.8f, true));
            g.drawText ("1", r, juce::Justification::centred);
            break;
        case Icon::Folder:
        {
            Path p;
            p.addRoundedRectangle (x + w * 0.1f, y + h * 0.28f, w * 0.8f, h * 0.54f, 2.0f);
            g.strokePath (p, st);
            g.fillRect (x + w * 0.1f, y + h * 0.2f, w * 0.34f, h * 0.12f);
            break;
        }
        case Icon::Undo:
        case Icon::Redo:
        {
            const bool redo = icon == Icon::Redo;
            Path p;
            p.addCentredArc (x + w * 0.5f, y + h * 0.58f, w * 0.3f, h * 0.28f, 0.0f,
                             redo ? 0.0f : -juce::MathConstants<float>::pi, redo ? juce::MathConstants<float>::pi : 0.0f, true);
            g.strokePath (p, st);
            Path a;
            if (redo) a.addTriangle (x + w * 0.9f, y + h * 0.5f, x + w * 0.6f, y + h * 0.5f, x + w * 0.8f, y + h * 0.7f);
            else a.addTriangle (x + w * 0.1f, y + h * 0.5f, x + w * 0.4f, y + h * 0.5f, x + w * 0.2f, y + h * 0.7f);
            g.fillPath (a);
            break;
        }
        case Icon::Close:
            g.drawLine (x + w * 0.25f, y + h * 0.25f, x + w * 0.75f, y + h * 0.75f, 1.6f);
            g.drawLine (x + w * 0.75f, y + h * 0.25f, x + w * 0.25f, y + h * 0.75f, 1.6f);
            break;
        case Icon::Menu:
            for (int i = 0; i < 3; ++i)
                g.fillRoundedRectangle (x + w * 0.15f, y + h * (0.25f + 0.25f * (float) i), w * 0.7f, h * 0.09f, 1.0f);
            break;
        case Icon::Expand:
        {
            Path p;
            p.startNewSubPath (x + w * 0.14f, y + h * 0.4f);
            p.lineTo (x + w * 0.14f, y + h * 0.14f);
            p.lineTo (x + w * 0.4f, y + h * 0.14f);
            p.startNewSubPath (x + w * 0.6f, y + h * 0.14f);
            p.lineTo (x + w * 0.86f, y + h * 0.14f);
            p.lineTo (x + w * 0.86f, y + h * 0.4f);
            p.startNewSubPath (x + w * 0.86f, y + h * 0.6f);
            p.lineTo (x + w * 0.86f, y + h * 0.86f);
            p.lineTo (x + w * 0.6f, y + h * 0.86f);
            p.startNewSubPath (x + w * 0.4f, y + h * 0.86f);
            p.lineTo (x + w * 0.14f, y + h * 0.86f);
            p.lineTo (x + w * 0.14f, y + h * 0.6f);
            g.strokePath (p, st);
            break;
        }
        case Icon::Merge:
        {
            // two clips compressing into one: filled pair, converging chevron, single clip
            g.fillRoundedRectangle (x + w * 0.06f, y + h * 0.14f, w * 0.28f, h * 0.28f, 1.5f);
            g.fillRoundedRectangle (x + w * 0.06f, y + h * 0.58f, w * 0.28f, h * 0.28f, 1.5f);
            Path p;
            p.startNewSubPath (x + w * 0.40f, y + h * 0.26f);
            p.lineTo (x + w * 0.58f, y + h * 0.50f);
            p.lineTo (x + w * 0.40f, y + h * 0.74f);
            g.strokePath (p, st);
            g.drawRoundedRectangle (x + w * 0.68f, y + h * 0.30f, w * 0.26f, h * 0.40f, 1.5f, 1.2f);
            break;
        }
        case Icon::MuteTool:
        {
            // muted speaker: small loudspeaker crossed by one clean diagonal slash
            Path p;
            p.addTriangle (x + w * 0.10f, y + h * 0.40f, x + w * 0.10f, y + h * 0.60f, x + w * 0.28f, y + h * 0.60f);
            p.lineTo (x + w * 0.52f, y + h * 0.78f);
            p.lineTo (x + w * 0.52f, y + h * 0.22f);
            p.closeSubPath();
            g.fillPath (p);
            g.drawLine (x + w * 0.66f, y + h * 0.28f, x + w * 0.90f, y + h * 0.72f, 1.7f);
            break;
        }
        case Icon::Follow:
        {
            // a playhead line with the view sliding along behind it
            const float cx = x + w * 0.46f;
            g.fillRect (cx - 0.9f, y + h * 0.12f, 1.8f, h * 0.76f);
            Path p;
            p.addTriangle (cx - 4.0f, y + h * 0.12f, cx + 4.0f, y + h * 0.12f, cx, y + h * 0.30f);
            g.fillPath (p);
            for (int i = 0; i < 2; ++i)
            {
                Path ch;
                const float ox = x + w * (0.16f + 0.16f * (float) i);
                ch.startNewSubPath (ox, y + h * 0.38f);
                ch.lineTo (ox + w * 0.14f, y + h * 0.50f);
                ch.lineTo (ox, y + h * 0.62f);
                g.strokePath (ch, PathStrokeType (1.5f));
            }
            break;
        }
        case Icon::Chord:
        {
            // three chord tones stacked like a voicing, tied together by a bracket
            const float dots[3][2] = { { 0.30f, 0.74f }, { 0.52f, 0.50f }, { 0.74f, 0.76f } };
            for (auto& d : dots) g.fillEllipse (x + w * d[0] - w * 0.085f, y + h * d[1] - h * 0.085f, w * 0.17f, h * 0.17f);
            Path p;
            p.startNewSubPath (x + w * 0.30f, y + h * 0.24f);
            p.lineTo (x + w * 0.30f, y + h * 0.16f);
            p.lineTo (x + w * 0.74f, y + h * 0.16f);
            p.lineTo (x + w * 0.74f, y + h * 0.24f);
            g.strokePath (p, st);
            break;
        }
        case Icon::Marker:
        {
            // a section flag on a post
            g.fillRect (x + w * 0.24f, y + h * 0.12f, w * 0.1f, h * 0.76f);
            Path p;
            p.startNewSubPath (x + w * 0.34f, y + h * 0.16f);
            p.lineTo (x + w * 0.82f, y + h * 0.30f);
            p.lineTo (x + w * 0.34f, y + h * 0.44f);
            p.closeSubPath();
            g.fillPath (p);
            break;
        }
        case Icon::Dice:
        {
            // a die: the five-pip face, i.e. "randomise this"
            Path p;
            p.addRoundedRectangle (x + w * 0.10f, y + h * 0.10f, w * 0.80f, h * 0.80f, 2.5f);
            g.strokePath (p, st);
            const float pips[5][2] = { { 0.32f, 0.32f }, { 0.68f, 0.32f }, { 0.50f, 0.50f }, { 0.32f, 0.68f }, { 0.68f, 0.68f } };
            const float pipSize = w * 0.088f;
            for (auto& pip : pips)
                g.fillEllipse (x + w * pip[0] - pipSize, y + h * pip[1] - pipSize, pipSize * 2.0f, pipSize * 2.0f);
            break;
        }
    }
}

// the system crosshair cursor is quite large; the drawing tools use a compact one
inline const juce::MouseCursor& drawCrosshairCursor()
{
    static const juce::MouseCursor cursor = []
    {
        const int size = 15, mid = size / 2;
        juce::Image img (juce::Image::ARGB, size, size, true);
        juce::Graphics g (img);
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.fillRect ((float) mid - 1.0f, 0.0f, 3.0f, (float) size);
        g.fillRect (0.0f, (float) mid - 1.0f, (float) size, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.fillRect ((float) mid, 0.0f, 1.0f, (float) size);
        g.fillRect (0.0f, (float) mid, (float) size, 1.0f);
        return juce::MouseCursor (img, mid, mid);
    }();
    return cursor;
}

class IconButton : public juce::Button
{
public:
    IconButton (Icon i, const juce::String& tip, juce::Colour onCol = theme::accent)
        : juce::Button (tip), icon (i), onColour (onCol)
    {
        setTooltip (tip);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void setBackgroundVisible (bool v) { bg = v; repaint(); }
    void setIconColour (juce::Colour c) { offColour = c; repaint(); }
    void setIcon (Icon i) { icon = i; repaint(); }


    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = getToggleState();
        if (on)
        {
            g.setColour (onColour.withAlpha (down ? 0.28f : 0.18f));
            g.fillRoundedRectangle (r, 5.0f);
        }
        else if (down || over)
        {
            g.setColour (down ? theme::accentSoft : theme::accentHover);
            g.fillRoundedRectangle (r, 5.0f);
        }
        else if (bg)
        {
            g.setColour (theme::white);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (theme::line);
            g.drawRoundedRectangle (r, 5.0f, 1.0f);
        }
        auto col = on ? onColour : (isEnabled() ? offColour : theme::textFaint);
        drawIcon (g, icon, r.reduced (juce::jmax (4.0f, r.getHeight() * 0.2f)), col);
    }

private:
    Icon icon;
    juce::Colour onColour;
    juce::Colour offColour { theme::textDim };
    bool bg = false;
};

class TextToggle : public juce::Button
{
public:
    TextToggle (const juce::String& text, juce::Colour on, const juce::String& tip)
        : juce::Button (text), onColour (on)
    {
        setTooltip (tip);
        setClickingTogglesState (true);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        g.setColour (on ? onColour : (down ? theme::accentSoft : over ? theme::accentHover : theme::white));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (on ? onColour : theme::line);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        g.setColour (on ? theme::onAccent : theme::textDim);
        g.setFont (theme::font (juce::jmin (12.5f, r.getHeight() * 0.62f), true));
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred);
    }

private:
    juce::Colour onColour;
};

class LevelMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Orientation { vertical, horizontal };

    explicit LevelMeter (Orientation o = Orientation::vertical) : orientation (o) {}

    /** peak / rms per channel: the bar shows the RMS body with the peak level on top, a peak-hold
        tick stays for a moment and the top LED latches when the channel clipped. The small orange
        tick is the highest peak since the app started and does not decay; click the meter to
        reset the hold / maximum. */
    void setLevels (float peakL, float peakR, float rmsL = -1.0f, float rmsR = -1.0f, bool clipNow = false)
    {
        const float decay = 0.86f;
        left = juce::jmax (peakL, left * decay);
        right = juce::jmax (peakR, right * decay);
        if (rmsL >= 0.0f) { rmsLeft = juce::jmax (rmsL, rmsLeft * 0.80f); rmsRight = juce::jmax (rmsR, rmsRight * 0.80f); }
        leftHold = juce::jmax (peakL, leftHold - 0.02f);
        rightHold = juce::jmax (peakR, rightHold - 0.02f);
        maxLeft = juce::jmax (maxLeft, peakL);
        maxRight = juce::jmax (maxRight, peakR);
        if (clipNow || peakL > 1.0f || peakR > 1.0f) clip = true;
        repaint();
    }

    /** A complete snapshot from MeterCache: the values are already decayed / held, so this does
        not apply a second decay. Used by every fader in the arrangement and the mixer. */
    void setState (float levelL, float levelR, float rmsL, float rmsR,
                   float holdL, float holdR, float maxL, float maxR, bool clipNow)
    {
        left = levelL;
        right = levelR;
        rmsLeft = rmsL;
        rmsRight = rmsR;
        leftHold = holdL;
        rightHold = holdR;
        maxLeft = maxL;
        maxRight = maxR;
        if (clipNow) clip = true;
        repaint();
    }

    void resetClip() { clip = false; repaint(); }
    bool isClipped() const { return clip; }
    float getMaxPeakL() const { return maxLeft; }
    float getMaxPeakR() const { return maxRight; }

    /** Called when the meter is clicked, so the owner can clear the matching cache maximum. */
    std::function<void()> onReset;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        if (r.getWidth() <= 0.0f || r.getHeight() <= 0.0f) return;

        if (orientation == Orientation::horizontal)
        {
            const float gap = r.getHeight() >= 6.0f ? 1.0f : 0.0f;
            const float bh = juce::jmax (1.0f, (r.getHeight() - gap) * 0.5f);
            paintHBar (g, { r.getX(), r.getY(), r.getWidth(), bh }, left, rmsLeft, leftHold, maxLeft);
            paintHBar (g, { r.getX(), r.getY() + bh + gap, r.getWidth(), bh }, right, rmsRight, rightHold, maxRight);
            if (clip) { g.setColour (theme::record); g.fillRect (r.getX(), r.getY(), 2.0f, r.getHeight()); }
            return;
        }

        const float bw = (r.getWidth() - 2.0f) * 0.5f;
        paintBar (g, { r.getX(), r.getY() + 4.0f, bw, r.getHeight() - 4.0f }, left, rmsLeft, leftHold, maxLeft);
        paintBar (g, { r.getX() + bw + 2.0f, r.getY() + 4.0f, bw, r.getHeight() - 4.0f }, right, rmsRight, rightHold, maxRight);
        g.setColour (clip ? theme::record : theme::panelDeep);
        g.fillRect (r.getX(), r.getY(), r.getWidth(), 3.0f);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        clip = false;
        leftHold = rightHold = 0.0f;
        maxLeft = maxRight = 0.0f;
        if (onReset != nullptr) onReset();
        repaint();
    }

private:
    static float toPos (float gain)
    {
        const float db = juce::Decibels::gainToDecibels (gain, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
    }

    void paintBar (juce::Graphics& g, juce::Rectangle<float> r, float level, float rms, float hold, float max) const
    {
        g.setColour (theme::panelDeep);
        g.fillRect (r);

        // the RMS body sits behind the peak bar, so short transients stay readable
        if (rms > 0.0001f)
        {
            const float pr = toPos (rms);
            g.setColour (theme::meterGreen.darker (0.45f));
            g.fillRect (r.withTrimmedTop (r.getHeight() * (1.0f - pr)));
        }
        const float p = toPos (level);
        g.setColour (level > 0.89f ? theme::meterHigh : theme::meterGreen);
        g.fillRect (r.withTrimmedTop (r.getHeight() * (1.0f - p)));

        // recent peak-hold tick
        if (hold > 0.001f)
        {
            const float hp = toPos (hold);
            g.setColour (theme::meterHigh.withAlpha (0.9f));
            g.fillRect (r.getX(), r.getY() + r.getHeight() * (1.0f - hp) - 1.0f, r.getWidth(), 1.5f);
        }

        // highest peak: persistent orange marker, never decays
        if (max > 0.001f)
        {
            const float mp = toPos (max);
            g.setColour (theme::warn);
            g.fillRect (r.getX(), r.getY() + r.getHeight() * (1.0f - mp) - 1.0f, r.getWidth(), 1.5f);
        }

        // dB ticks: 0 / -6 / -12 / -24 / -48
        g.setColour (theme::panelDeep.withAlpha (0.55f));
        for (float db : { -48.0f, -24.0f, -12.0f, -6.0f, 0.0f })
        {
            const float t = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
            g.fillRect (r.getX(), r.getY() + r.getHeight() * (1.0f - t), r.getWidth(), 0.6f);
        }
    }

    void paintHBar (juce::Graphics& g, juce::Rectangle<float> r, float level, float rms, float hold, float max) const
    {
        g.setColour (theme::panelDeep);
        g.fillRect (r);

        if (rms > 0.0001f)
        {
            g.setColour (theme::meterGreen.darker (0.45f));
            g.fillRect (r.withWidth (r.getWidth() * toPos (rms)));
        }
        const float p = toPos (level);
        g.setColour (level > 0.89f ? theme::meterHigh : theme::meterGreen);
        g.fillRect (r.withWidth (r.getWidth() * p));

        if (hold > 0.001f)
        {
            const float hp = toPos (hold);
            g.setColour (theme::meterHigh.withAlpha (0.9f));
            g.fillRect (r.getX() + r.getWidth() * hp - 1.0f, r.getY(), 1.5f, r.getHeight());
        }
        if (max > 0.001f)
        {
            const float mp = toPos (max);
            g.setColour (theme::warn);
            g.fillRect (r.getX() + r.getWidth() * mp - 1.0f, r.getY(), 1.5f, r.getHeight());
        }

        // only a few ticks fit in a 5 px header meter; keep the useful -24 / -12 / -6 marks
        if (r.getWidth() > 30.0f)
        {
            g.setColour (theme::panelDeep.withAlpha (0.55f));
            for (float db : { -24.0f, -12.0f, -6.0f })
            {
                const float t = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
                g.fillRect (r.getX() + r.getWidth() * t, r.getY(), 0.6f, r.getHeight());
            }
        }
    }

    Orientation orientation = Orientation::vertical;
    float left = 0.0f, right = 0.0f, rmsLeft = 0.0f, rmsRight = 0.0f;
    float leftHold = 0.0f, rightHold = 0.0f;
    float maxLeft = 0.0f, maxRight = 0.0f;
    bool clip = false;
};

//==============================================================================
/** Balance meter: where a channel sits between left and right (the "pan meter"). The bar fills
    from the centre towards the side the signal leans to, and the marker grows with the level. */
class PanMeter : public juce::Component
{
public:
    void setLevels (float l, float r, float pan)
    {
        const float sum = l + r;
        target = sum > 1.0e-6f ? juce::jlimit (-1.0f, 1.0f, (r - l) / sum) : juce::jlimit (-1.0f, 1.0f, pan);
        energy = juce::jmax (energy * 0.9f, juce::jmax (l, r));
        repaint();
    }
    float position() const { return target; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        auto bar = r.withSizeKeepingCentre (juce::jmax (10.0f, r.getWidth() - 6.0f), 9.0f);
        g.setColour (theme::panelDeep);
        g.fillRoundedRectangle (bar, 3.5f);

        const float cx = bar.getCentreX();
        const float halfW = bar.getWidth() * 0.5f - 4.0f;
        const float x = cx + target * halfW;
        const float from = juce::jmin (cx, x), to = juce::jmax (cx, x);
        if (to - from > 0.5f)
        {
            g.setColour (theme::accent.withAlpha (0.55f));
            g.fillRoundedRectangle ({ from, bar.getY() + 2.0f, to - from, bar.getHeight() - 4.0f }, 2.0f);
        }
        g.setColour (theme::line);
        g.drawVerticalLine ((int) cx, bar.getY() - 2.0f, bar.getBottom() + 2.0f);

        const float rad = 2.0f + 2.0f * juce::jlimit (0.0f, 1.0f, energy);
        g.setColour (energy > 0.001f ? theme::meterHigh : theme::textDim);
        g.fillEllipse (x - rad, bar.getCentreY() - rad, rad * 2.0f, rad * 2.0f);

        g.setColour (theme::textDim);
        g.setFont (theme::font (9.0f));
        g.drawText ("L", getLocalBounds().removeFromLeft (8), juce::Justification::centredLeft);
        g.drawText ("R", getLocalBounds().removeFromRight (8), juce::Justification::centredRight);
    }

private:
    float target = 0.0f, energy = 0.0f;
};

//==============================================================================
/** Goniometer for the master: L against R in a 45 degree square. A mono signal draws a vertical
    line, a wide stereo mix a cloud - the classic "pan meter" for the whole mix. */
class StereoScope : public juce::Component
{
public:
    void setSamples (const float* interleaved, int pairs)
    {
        points.clear();
        points.reserve ((size_t) pairs);
        for (int i = 0; i < pairs; ++i)
            points.push_back ({ interleaved[i * 2], interleaved[i * 2 + 1] });
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (theme::panelDeep);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (theme::line.withAlpha (0.6f));
        g.drawLine (r.getCentreX(), r.getY(), r.getCentreX(), r.getBottom(), 0.6f);
        g.drawLine (r.getX(), r.getCentreY(), r.getRight(), r.getCentreY(), 0.6f);

        const float half = juce::jmin (r.getWidth(), r.getHeight()) * 0.42f;
        const auto c = r.getCentre();
        g.setColour (theme::accent.withAlpha (0.10f));
        g.drawEllipse (c.x - half, c.y - half, half * 2.0f, half * 2.0f, 1.0f);

        g.setColour (theme::accent);
        for (auto& p : points)
        {
            const float x = c.x + (p.first - p.second) * half;
            const float y = c.y - (p.first + p.second) * half * 0.5f;
            if (! r.contains (x, y)) continue;
            g.fillRect (x - 0.7f, y - 0.7f, 1.4f, 1.4f);
        }
    }

private:
    std::vector<std::pair<float, float>> points;
};
}