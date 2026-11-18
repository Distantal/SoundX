#pragma once

#include "AppState.h"

namespace sx
{
// Fullscreen editor for one audio clip: a large waveform view with the gain
// envelope on top plus fade in / out grips. The view zooms and pans: wheel =
// horizontal zoom, Shift+wheel = scroll left / right, Ctrl+wheel = dB zoom
// (always around 0 dB, so the envelope line stays in the middle), middle-drag
// or the arrow keys pan the time axis, and the buttons reset / fit the view.
class ClipGainEditor : public juce::Component
{
public:
    explicit ClipGainEditor (AppState& a);
    ~ClipGainEditor() override = default;

    void open (int trackId, int clipId);
    void close();
    bool isOpen() const { return openState; }
    /** Re-apply the theme colours held by the toolbar buttons. */
    void refreshTheme();
    // false after an undo / delete removed the clip this editor points at
    bool clipExists() const { return clip() != nullptr; }

    // current view transform, also used by the self test
    struct ViewState { double start = 0.0, beats = 0.0; float dbTop = 12.0f, dbSpan = 36.0f; };
    ViewState getViewState() const { return { viewStart, viewBeats, dbTop, dbSpan() }; }
    juce::Rectangle<float> getPlotRect() const { return plotRect(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

    std::function<void()> onClose;

private:
    AudioClip* clip() const;
    double clipBeats() const;
    juce::Rectangle<float> plotRect() const;

    // view transform: 'viewStart' is the clip-relative beat at the left edge,
    // 'viewBeats' the visible window (0 = fit the whole clip); the dB range is
    // always symmetric around 0 dB (dbTop = +half, dbBottom = -half) so the
    // 0 dB line and a flat envelope never leave the middle of the waveform
    double effViewBeats() const;
    float dbSpan() const { return dbTop - dbBottom; }
    void zoomHorizontal (double factor, float anchorX);
    void zoomVertical (double factor, float anchorY);   // anchored on 0 dB, not the mouse
    void panView (double dBeats, float ddB);            // time axis only
    void fitView();

    float dbToY (float db) const;
    float yToDb (float y) const;
    float beatToX (double beat) const;
    double xToBeat (float x) const;
    int hitPoint (juce::Point<int> p) const;
    int hitFadeGrip (juce::Point<int> p) const;      // 1 = fade in, 2 = fade out, 0 = none
    void refreshModeButtons();

    AppState& app;
    int trackId = 0, clipId = 0;
    bool openState = false;
    int dragIdx = -1;
    bool draggingAll = false;
    int dragStartY = 0;
    std::vector<ClipGainPoint> dragBackup;

    // zoom / pan state
    double viewStart = 0.0, viewBeats = 0.0;
    float dbTop = 24.0f, dbBottom = -24.0f;   // always symmetric: 0 dB in the middle
    bool panning = false;
    bool erasing = false;   // eraser tool active
    juce::Point<int> panFrom;
    double panViewStart = 0.0, panViewBeats = 0.0;

    int fadeDrag = 0;                                 // 1 / 2 while dragging a fade grip

    // the texts are set in the constructor (and refreshModeButtons) so they go through TRANS
    juce::TextButton modeBtn, snapBtn, closeBtn, eraseBtn;
    juce::TextButton hzOut { "-" }, hzIn { "+" }, vzOut { "dB-" }, vzIn { "dB+" }, fitBtn;
};
}
