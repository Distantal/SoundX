#include "TitleBar.h"
#include "Theme.h"

namespace sx
{
using namespace juce;

SXTitleBar::WindowButton::WindowButton (Kind k)
    : Button (k == Kind::close ? "Close" : (k == Kind::minimise ? "Minimise" : "Maximise")),
      kind (k)
{
    setWantsKeyboardFocus (false);
    setOpaque (false);
    setMouseCursor (MouseCursor::PointingHandCursor);
}

void SXTitleBar::WindowButton::paintButton (Graphics& g, bool highlighted, bool down)
{
    // The glyph is drawn by SXTitleBar::paintOverChildren so it always remains visible even if
    // the child component's own paint is clipped by the platform window frame. This method only
    // paints the hover / pressed background.
    if (! highlighted && ! down) return;

    auto r = getLocalBounds().toFloat().reduced (2.0f, 3.0f);
    const bool isClose = kind == Kind::close;
    auto bg = isClose ? theme::record : theme::panelDeep;
    if (down) bg = bg.darker (0.12f);
    else if (! isClose) bg = theme::accentSoft;
    g.setColour (bg);
    g.fillRoundedRectangle (r, 4.0f);
}

//==============================================================================
SXTitleBar::SXTitleBar (Component& windowToMove) : window (windowToMove)
{
    setOpaque (true);

    addAndMakeVisible (minimise);
    addAndMakeVisible (maximise);
    addAndMakeVisible (close);

    minimise.onClick = [this]
    {
        if (onMinimise != nullptr) onMinimise();
        else if (auto* rw = findParentComponentOfClass<ResizableWindow>()) rw->setMinimised (true);
    };
    maximise.onClick = [this] { if (onMaximise != nullptr) onMaximise(); };
    close.onClick = [this]
    {
        if (onClose != nullptr) onClose();
        else if (auto* doc = findParentComponentOfClass<DocumentWindow>()) doc->closeButtonPressed();
    };
}

void SXTitleBar::setTitle (const String& newTitle)
{
    if (title == newTitle) return;
    title = newTitle;
    repaint();
}

void SXTitleBar::setMaximised (bool shouldBeMaximised)
{
    if (maximised == shouldBeMaximised) return;
    maximised = shouldBeMaximised;
    maximise.repaint();
}

void SXTitleBar::paintOverChildren (Graphics& g)
{
    // Draw the window-control glyphs above the child buttons. The buttons still handle clicks and
    // paint their hover background, but this keeps the icons visible even when the platform clips
    // the child component paint (Windows' non-client frame can do that).
    const WindowButton* buttons[] = { &minimise, &maximise, &close };
    for (auto* b : buttons)
    {
        auto r = b->getBounds().toFloat().reduced (2.0f, 3.0f);
        const auto kind = b->getKind();
        const bool isClose = kind == WindowButton::Kind::close;
        g.setColour ((isClose && (b->isOver() || b->isDown())) ? theme::onAccent : theme::textDim);

        const auto c = r.getCentre();
        const float s = 5.0f;
        const float th = 1.4f;

        if (kind == WindowButton::Kind::minimise)
        {
            g.fillRect (Rectangle<float> (c.x - s, c.y - 0.5f, s * 2.0f, th));
        }
        else if (kind == WindowButton::Kind::maximise)
        {
            if (maximised)
            {
                g.drawRoundedRectangle (c.x - s + 2.0f, c.y - s, s * 2.0f - 2.0f, s * 2.0f - 2.0f, 1.5f, th);
                g.drawRoundedRectangle (c.x - s, c.y - s + 2.0f, s * 2.0f - 2.0f, s * 2.0f - 2.0f, 1.5f, th);
            }
            else
            {
                g.drawRoundedRectangle (c.x - s, c.y - s + 0.5f, s * 2.0f, s * 2.0f - 1.0f, 1.5f, th);
            }
        }
        else
        {
            g.drawLine (c.x - s, c.y - s, c.x + s, c.y + s, th);
            g.drawLine (c.x + s, c.y - s, c.x - s, c.y + s, th);
        }
    }
}

void SXTitleBar::resized()
{
    auto r = getLocalBounds();
    const int buttonWidth = 44;
    const int buttonHeight = jmax (1, r.getHeight() - 1);

    // Windows / Linux: [minimise] [maximise] [close] at the right end.
    // macOS: the traffic lights sit at the left end, in the conventional order.
   #if JUCE_MAC
    close.setBounds (r.removeFromLeft (buttonWidth).withHeight (buttonHeight));
    minimise.setBounds (r.removeFromLeft (buttonWidth).withHeight (buttonHeight));
    maximise.setBounds (r.removeFromLeft (buttonWidth).withHeight (buttonHeight));
   #else
    close.setBounds (r.removeFromRight (buttonWidth).withHeight (buttonHeight));
    maximise.setBounds (r.removeFromRight (buttonWidth).withHeight (buttonHeight));
    minimise.setBounds (r.removeFromRight (buttonWidth).withHeight (buttonHeight));
   #endif
}

void SXTitleBar::paint (Graphics& g)
{
    auto r = getLocalBounds();
    auto* top = findParentComponentOfClass<TopLevelWindow>();
    const bool active = top == nullptr || top->isActiveWindow();

    g.fillAll (theme::panel);
    ColourGradient gradient (active ? theme::panelAlt : theme::panel,
                             0.0f, 0.0f,
                             theme::panel, 0.0f, (float) r.getHeight(), false);
    g.setGradientFill (gradient);
    g.fillRect (r);

    // no coloured frame around the window: the bar is separated from the content by a line only
    g.setColour (theme::line);
    g.fillRect (0, r.getHeight() - 1, r.getWidth(), 1);

   #if JUCE_MAC
    int x = 12 + 3 * 44;
   #else
    int x = 12;
   #endif

    // The app-icon slot is intentionally empty for now: keep its width so the brand and title
    // stay where they are, and a custom icon can be dropped in later without moving the layout.
    x += 28;
    g.setFont (theme::font (14.5f, true));
    g.setColour (active ? theme::text : theme::textDim);
    const String brand ("SoundX");
    const int brandW = GlyphArrangement::getStringWidthInt (theme::font (14.5f, true), brand);
    g.drawText (brand, x, 0, brandW + 2, r.getHeight(), Justification::centredLeft, false);
    x += brandW + 10;

    String rest = title;
    if (rest.startsWith ("SoundX - ")) rest = rest.substring (9);
    else if (rest == "SoundX") rest.clear();

    if (rest.isNotEmpty())
    {
        g.setColour (theme::line);
        g.fillRect (x, r.getHeight() / 2 - 7, 1, 14);
        x += 10;

        const int right = r.getWidth() - 16;
        g.setFont (theme::font (13.0f));
        g.setColour (active ? theme::textDim : theme::textFaint);
        g.drawFittedText (rest, x, 0, jmax (0, right - x), r.getHeight(),
                          Justification::centredLeft, 1, 0.85f);
    }
}

void SXTitleBar::mouseDown (const MouseEvent& e)
{
    if (! e.mods.isLeftButtonDown()) return;
    // a maximised window first has to be restored before it can be dragged; leaving that to the
    // maximise button (and the double click) keeps the interaction predictable
    if (maximised) return;

    dragging = true;
    dragger.startDraggingComponent (&window, e);
}

void SXTitleBar::mouseDrag (const MouseEvent& e)
{
    if (dragging)
        dragger.dragComponent (&window, e, nullptr);
}

void SXTitleBar::mouseUp (const MouseEvent&)
{
    dragging = false;
}

void SXTitleBar::mouseDoubleClick (const MouseEvent& e)
{
    if (! e.mods.isLeftButtonDown()) return;
    dragging = false;
    if (onMaximise != nullptr) onMaximise();
}
}
