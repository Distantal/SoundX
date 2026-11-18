#include "VirtualKeyboard.h"
#include "Localisation.h"

namespace sx
{
using namespace juce;

VirtualKeyboard::VirtualKeyboard (AppState& a) : app (a)
{
    setOpaque (false);
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setTooltip (TRANS ("Drag the title bar to move the keyboard. Double-click it to reset the position."));

    closeBtn.setWantsKeyboardFocus (false);
    closeBtn.setMouseClickGrabsKeyboardFocus (false);
    closeBtn.onClick = [this] { if (onClose != nullptr) onClose(); };
    addAndMakeVisible (closeBtn);
}

void VirtualKeyboard::resized()
{
    closeBtn.setBounds (getWidth() - 28, 4, 22, 20);
}

void VirtualKeyboard::paint (Graphics& g)
{
    // a soft shadow lifts the panel off the arrangement and makes it read as a floating window
    DropShadow (Colours::black.withAlpha (0.22f), 12, { 0, 4 })
        .drawForRectangle (g, getLocalBounds().reduced (3));

    auto panel = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (theme::panel.withAlpha (0.985f));
    g.fillRoundedRectangle (panel, 9.0f);
    g.setColour (theme::accent.withAlpha (0.55f));
    g.drawRoundedRectangle (panel, 9.0f, 1.0f);

    //== title bar ------------------------------------------------------------------------------
    auto head = headerArea().reduced (12, 0);
    head.removeFromRight (30);                    // keep clear of the close button
    g.setFont (theme::font (12.5f, true));
    g.setColour (theme::text);
    g.drawText (TRANS ("Musical Typing"), head.removeFromLeft (118), Justification::centredLeft);

    g.setFont (theme::font (9.5f));
    g.setColour (theme::textFaint);
    g.drawText (TRANS ("Drag to move"), head.removeFromLeft (jmin (94, head.getWidth())), Justification::centredLeft);

    const int base = (app.keyboardOctave + 2) * 12;
    auto noteName = [] (int n)
    {
        static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return String (names[jlimit (0, 11, n % 12)]) + String (n / 12 - 1);
    };
    g.setFont (theme::font (10.5f));
    g.setColour (theme::textDim);
    g.drawText (noteName (base) + " - " + noteName (base + 16) + "   Z/X OCT   C/V VEL "
                    + String (app.keyboardVelocity),
                head, Justification::centredRight);

    g.setColour (theme::line);
    g.fillRect (8, headerArea().getBottom() - 1, getWidth() - 16, 1);

    //== keys -----------------------------------------------------------------------------------
    auto keysArea = getLocalBounds().withTrimmedTop (headerArea().getHeight()).reduced (12, 8).toFloat();
    if (keysArea.getWidth() <= 4.0f || keysArea.getHeight() <= 4.0f) return;

    const int whiteCount = 10;
    const float whiteW = keysArea.getWidth() / (float) whiteCount;
    const float blackW = whiteW * 0.60f;
    const float blackH = keysArea.getHeight() * 0.62f;
    auto isBlack = [] (int pc) { return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; };

    struct Key { Rectangle<float> r; int note = 0; int keyIndex = 0; bool black = false; };
    std::vector<Key> keys;
    keys.reserve (17);

    int wi = 0;
    for (int i = 0; i < 17; ++i)
    {
        const int note = base + i;
        if (! isBlack (note % 12))
        {
            keys.push_back ({ { keysArea.getX() + (float) wi * whiteW, keysArea.getY(),
                                whiteW - 1.5f, keysArea.getHeight() }, note, i, false });
            ++wi;
        }
    }
    wi = 0;
    for (int i = 0; i < 17; ++i)
    {
        const int note = base + i;
        if (isBlack (note % 12))
            keys.push_back ({ { keysArea.getX() + (float) wi * whiteW - blackW * 0.5f, keysArea.getY(),
                                blackW, blackH }, note, i, true });
        else
            ++wi;
    }

    auto active = [this] (int note)
    {
        return isNoteActive != nullptr && isNoteActive (note);
    };

    // white keys first, then the black keys on top
    for (int pass = 0; pass < 2; ++pass)
        for (auto& k : keys)
        {
            if (k.black != (pass == 1)) continue;
            const bool on = active (k.note);
            g.setColour (k.black ? (on ? theme::accent : theme::keyBlack)
                                 : (on ? theme::keyDown : theme::keyWhite));
            g.fillRoundedRectangle (k.r, 2.5f);
            g.setColour (k.black ? theme::keyLine.darker (0.65f) : theme::keyLine);
            g.drawRoundedRectangle (k.r, 2.5f, 1.0f);

            const auto label = String::charToString ((juce_wchar) kTypingKeys[k.keyIndex]).toUpperCase();
            g.setFont (theme::font (k.black ? 9.0f : 11.0f, true));
            g.setColour (k.black ? theme::keyWhite : theme::keyBlack);
            auto labelArea = k.r.toNearestInt().removeFromBottom (k.black ? 16 : 34);
            g.drawText (label, labelArea.removeFromTop (k.black ? 14 : 18), Justification::centred);
            if (! k.black)
            {
                g.setFont (theme::font (8.5f));
                g.setColour (theme::keyLabel);
                g.drawText (noteName (k.note), labelArea, Justification::centred);
            }
        }
}

void VirtualKeyboard::layoutIn (Rectangle<int> availableParentArea, bool resetToDefault)
{
    dragBounds = availableParentArea;

    const int w = jmin (kPreferredWidth, jmax (320, availableParentArea.getWidth()));
    const int h = jmin (kHeight, jmax (108, availableParentArea.getHeight()));
    setSize (w, h);

    int x, y;
    if (resetToDefault || app.keyboardX < 0 || app.keyboardY < 0)
    {
        x = availableParentArea.getCentreX() - w / 2;
        y = availableParentArea.getBottom() - h - 8;
    }
    else
    {
        x = app.keyboardX;
        y = app.keyboardY;
    }

    x = jlimit (availableParentArea.getX(), jmax (availableParentArea.getX(), availableParentArea.getRight() - w), x);
    y = jlimit (availableParentArea.getY(), jmax (availableParentArea.getY(), availableParentArea.getBottom() - h), y);
    setTopLeftPosition (x, y);
    app.keyboardX = x;
    app.keyboardY = y;
}

void VirtualKeyboard::updateCursor (const MouseEvent& e)
{
    setMouseCursor (headerArea().contains (e.getPosition()) && ! closeBtn.getBounds().contains (e.getPosition())
                        ? MouseCursor::DraggingHandCursor
                        : MouseCursor::NormalCursor);
}

void VirtualKeyboard::mouseMove (const MouseEvent& e)
{
    updateCursor (e);
}

void VirtualKeyboard::mouseDown (const MouseEvent& e)
{
    if (! headerArea().contains (e.getPosition())) return;
    dragging = true;
    dragFrom = e.getScreenPosition();
    dragStartBounds = getBounds();
    setMouseCursor (MouseCursor::DraggingHandCursor);
}

void VirtualKeyboard::mouseDrag (const MouseEvent& e)
{
    if (! dragging) return;

    auto delta = e.getScreenPosition() - dragFrom;
    auto b = dragStartBounds.translated (delta.x, delta.y);
    auto area = dragBounds;
    if (area.isEmpty() && getParentComponent() != nullptr)
        area = getParentComponent()->getLocalBounds().reduced (8);
    if (area.isEmpty()) area = getBounds();
    b.setX (jlimit (area.getX(), jmax (area.getX(), area.getRight() - getWidth()), b.getX()));
    b.setY (jlimit (area.getY(), jmax (area.getY(), area.getBottom() - getHeight()), b.getY()));
    setTopLeftPosition (b.getPosition());

    app.keyboardX = getX();
    app.keyboardY = getY();
    if (onMoved != nullptr) onMoved (getPosition());
}

void VirtualKeyboard::mouseUp (const MouseEvent& e)
{
    dragging = false;
    setMouseCursor (headerArea().contains (e.getPosition())
                        ? MouseCursor::DraggingHandCursor : MouseCursor::NormalCursor);
}

void VirtualKeyboard::mouseDoubleClick (const MouseEvent& e)
{
    if (headerArea().contains (e.getPosition()) && onResetPosition != nullptr)
        onResetPosition();
}
}
