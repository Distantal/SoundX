#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace sx
{
/** The main window's own title bar.

    The application used to leave the title bar to the operating system, which meant it kept the
    light Windows frame while the rest of the window was dark. This component draws the whole bar
    itself (brand, project title and the minimise / maximise / close buttons) from the same
    theme:: palette as the rest of the UI, so switching Dark Mode repaints it with the night
    colours immediately.

    It is a normal child of the window: dragging the empty part moves the window, a double click
    toggles maximise, and the buttons are real juce::Buttons (so they get hover / pressed states
    and keyboard focus for free).
*/
class SXTitleBar : public juce::Component
{
public:
    explicit SXTitleBar (juce::Component& windowToMove);
    ~SXTitleBar() override = default;

    /** The text shown next to the brand ("SoundX - Project *"). */
    void setTitle (const juce::String& newTitle);
    juce::String getTitle() const { return title; }

    /** Switches the maximise icon between "maximise" and "restore". */
    void setMaximised (bool shouldBeMaximised);

    /** Re-reads theme:: colours: called after Dark Mode is toggled. */
    void refreshTheme() { repaint(); }

    /** The window asks for these; the defaults minimise / close the window itself. */
    std::function<void()> onMinimise;
    std::function<void()> onMaximise;
    std::function<void()> onClose;

    //== self test hooks ==================================================================
    juce::Button* minimiseButton() noexcept { return &minimise; }
    juce::Button* maximiseButton() noexcept { return &maximise; }
    juce::Button* closeButton()    noexcept { return &close; }

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    class WindowButton : public juce::Button
    {
    public:
        enum class Kind { minimise, maximise, close };

        explicit WindowButton (Kind k);
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        Kind getKind() const noexcept { return kind; }

    private:
        Kind kind;
    };

    juce::Component& window;
    juce::ComponentDragger dragger;
    bool dragging = false;
    bool maximised = false;
    juce::String title { "SoundX" };

    WindowButton minimise { WindowButton::Kind::minimise };
    WindowButton maximise { WindowButton::Kind::maximise };
    WindowButton close    { WindowButton::Kind::close };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SXTitleBar)
};
}
