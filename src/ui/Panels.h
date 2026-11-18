#pragma once

#include "ChannelStrip.h"

namespace sx
{
class MixerView : public juce::Component, private Project::Listener, private AppState::Listener
{
public:
    explicit MixerView (AppState& a);
    ~MixerView() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void refreshTheme();

private:
    void projectStructureChanged() override { rebuild(); }
    void selectionChanged() override { repaint(); for (auto& s : strips) s->repaint(); }
    void rebuild();

    AppState& app;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<ChannelStrip>> strips;
    ChannelStrip master;
    ChannelStrip listen;          // the listen bus strip sits to the right of the master
    int effectiveStripWidth = 104; // the width the strips were actually laid out at (auto-fit may
                                   // narrow app.stripWidth when the window is too small for it)
    float stripZoomAccum = 0.0f;   // fractional wheel travel for the smooth strip-width zoom
    bool inWheelForward = false;   // re-entry guard for the strip viewport's wheel forwarding
    static constexpr int kStripW = 104;
};

class InspectorView : public juce::Component, private Project::Listener, private AppState::Listener
{
public:
    explicit InspectorView (AppState& a);
    ~InspectorView() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void refreshTheme();

private:
    void projectMixChanged() override { refresh(); }
    void projectStructureChanged() override { refresh(); }
    void selectionChanged() override { refresh(); }
    void refresh();

    AppState& app;
    ChannelStrip strip, master;
    juce::ComboBox channelBox;
    juce::Slider delaySlider;
    bool updating = false;
    juce::Rectangle<int> swatchArea, delayCaption;
};
}
