#pragma once

#include "AppState.h"
#include "Widgets.h"

namespace sx
{
// Overlay listing the autosave history of the previous session (newest first)
class AutosaveDialog : public juce::Component
{
public:
    explicit AutosaveDialog (AppState& a);

    void open (juce::Array<juce::File> snapshots, const juce::File& sourceProject, bool crashed);
    void close();
    bool isOpen() const { return openState; }

    std::function<void (const juce::File&)> onRecover;
    std::function<void()> onDiscard;
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int kRowH = 36;
    juce::Rectangle<int> panelRect() const;
    juce::Rectangle<int> rowsRect() const;
    int rowAt (juce::Point<int> p) const;
    void updateButtons();

    AppState& app;
    bool openState = false, crashedLast = false;
    juce::File source;
    juce::Array<juce::File> files;
    int selected = 0;

    juce::TextButton recoverBtn { TRANS ("Recover") };
    juce::TextButton discardBtn { TRANS ("Discard All") };
    juce::TextButton closeBtn { TRANS ("Close") };
};
}
