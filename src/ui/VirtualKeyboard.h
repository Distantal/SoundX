#pragma once

#include "AppState.h"
#include "Widgets.h"
#include <functional>

namespace sx
{
/** The letters the computer-keyboard performance mode maps to the white keys. */
inline constexpr const char* kTypingKeys = "awsedftgyhujkolp;";

/** The on-screen piano that belongs to computer-keyboard performance (Ctrl+K).

    It used to be painted straight over the arrangement, so it always sat at the bottom of the
    window and could not be moved. It is an ordinary child component now: the title bar drags it
    anywhere over the main window (the position is remembered in AppState), it can be closed with
    the X, and it only takes the repaints it needs. Keeping it inside the main window means the
    keyboard mode keeps the keyboard focus, so typing notes never stops when the panel is clicked.
*/
class VirtualKeyboard : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    explicit VirtualKeyboard (AppState& a);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    /** Repaint after the held notes / octave / velocity changed. */
    void notesChanged() { repaint(); }
    void refreshTheme() { repaint(); }

    /** Asks the owner where the panel may live and applies the remembered position. */
    void layoutIn (juce::Rectangle<int> availableParentArea, bool resetToDefault);

    std::function<void()> onClose;
    std::function<void (juce::Point<int>)> onMoved;
    std::function<void()> onResetPosition;
    /** True while one of the letter keys for this MIDI note is held down. */
    std::function<bool (int)> isNoteActive;

    static constexpr int kPreferredWidth = 760;
    static constexpr int kHeight = 152;

private:
    juce::Rectangle<int> headerArea() const { return { 0, 0, getWidth(), 28 }; }
    void updateCursor (const juce::MouseEvent& e);

    AppState& app;
    IconButton closeBtn { Icon::Close, TRANS ("Close the on-screen keyboard") };
    bool dragging = false;
    juce::Point<int> dragFrom;
    juce::Rectangle<int> dragStartBounds;
    juce::Rectangle<int> dragBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VirtualKeyboard)
};
}
