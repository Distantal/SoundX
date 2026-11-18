#include "AutosaveDialog.h"
#include "Theme.h"

namespace sx
{
using namespace juce;

AutosaveDialog::AutosaveDialog (AppState& a) : app (a)
{
    setVisible (false);
    recoverBtn.onClick = [this]
    {
        if (selected >= 0 && selected < files.size())
        {
            const auto f = files[selected];
            close();
            if (onRecover) onRecover (f);
        }
    };
    discardBtn.onClick = [this]
    {
        close();
        if (onDiscard) onDiscard();
    };
    closeBtn.onClick = [this]
    {
        close();
        if (onClose) onClose();
    };
    addAndMakeVisible (recoverBtn);
    addAndMakeVisible (discardBtn);
    addAndMakeVisible (closeBtn);
}

void AutosaveDialog::open (Array<File> snapshots, const File& sourceProject, bool crashed)
{
    files = std::move (snapshots);
    source = sourceProject;
    crashedLast = crashed;
    selected = 0;
    openState = true;
    setVisible (true);
    toFront (true);
    updateButtons();
    resized();
    repaint();
}

void AutosaveDialog::close()
{
    openState = false;
    setVisible (false);
}

void AutosaveDialog::updateButtons()
{
    recoverBtn.setEnabled (files.size() > 0 && selected >= 0 && selected < files.size());
}

Rectangle<int> AutosaveDialog::panelRect() const
{
    const int w = jmin (560, getWidth() - 40);
    const int h = jmin (420, getHeight() - 40);
    return Rectangle<int> ((getWidth() - w) / 2, (getHeight() - h) / 2, w, h);
}

Rectangle<int> AutosaveDialog::rowsRect() const
{
    auto p = panelRect().reduced (16);
    p.removeFromTop (74);          // title + info lines
    p.removeFromBottom (42);       // buttons
    return p;
}

int AutosaveDialog::rowAt (Point<int> pt) const
{
    auto rows = rowsRect();
    if (! rows.contains (pt)) return -1;
    const int idx = (pt.y - rows.getY()) / kRowH;
    return idx >= 0 && idx < files.size() ? idx : -1;
}

void AutosaveDialog::resized()
{
    auto p = panelRect().reduced (16);
    auto btnRow = p.removeFromBottom (30);
    closeBtn.setBounds (btnRow.removeFromRight (80));
    btnRow.removeFromRight (8);
    discardBtn.setBounds (btnRow.removeFromRight (100));
    btnRow.removeFromRight (8);
    recoverBtn.setBounds (btnRow.removeFromRight (100));
}

void AutosaveDialog::paint (Graphics& g)
{
    g.fillAll (Colours::black.withAlpha (0.45f));

    auto panel = panelRect();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (panel.toFloat(), 8.0f);
    g.setColour (theme::line);
    g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), 8.0f, 1.0f);

    auto p = panel.reduced (16);
    g.setColour (theme::text);
    g.setFont (theme::font (16.0f, true));
    g.drawText (TRANS ("Recover autosave?"), p.removeFromTop (24), Justification::centredLeft);

    g.setColour (crashedLast ? theme::accent : theme::textDim);
    g.setFont (theme::font (11.5f));
    g.drawText (crashedLast ? TRANS ("SoundX did not close normally. Pick a snapshot to recover:")
                            : TRANS ("A snapshot newer than the project was found. Pick one to recover:"),
                p.removeFromTop (18), Justification::centredLeft);

    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f));
    g.drawFittedText (TRANS ("Project:") + " " + (source.existsAsFile() ? source.getFullPathName() : TRANS ("(unsaved project)")),
                      p.removeFromTop (16), Justification::centredLeft, 1);

    g.setColour (theme::textFaint);
    g.setFont (theme::font (10.5f));
    g.drawText (TRANS ("Newest first"), p.removeFromTop (16), Justification::centredLeft);

    // rows
    auto rows = rowsRect();
    g.setColour (theme::laneB);
    g.fillRoundedRectangle (rows.toFloat(), 4.0f);

    for (int i = 0; i < files.size(); ++i)
    {
        auto row = Rectangle<int> (rows.getX(), rows.getY() + i * kRowH, rows.getWidth(), kRowH).reduced (1, 1);
        const bool sel = i == selected;
        if (sel)
        {
            g.setColour (theme::accentSoft);
            g.fillRect (row);
            g.setColour (theme::accent);
            g.fillRect (row.getX(), row.getY(), 3, row.getHeight());
        }
        g.setColour (sel ? theme::text : theme::textDim);
        g.setFont (theme::font (12.5f, sel));
        g.drawText (files[i].getFileNameWithoutExtension(), row.getX() + 12, row.getY(), row.getWidth() - 200, row.getHeight(),
                    Justification::centredLeft);

        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f));
        const auto when = files[i].getLastModificationTime().toString (true, true);
        g.drawText (when, row.getRight() - 190, row.getY(), 178, row.getHeight(), Justification::centredRight);
    }

    if (files.isEmpty())
    {
        g.setColour (theme::textFaint);
        g.setFont (theme::font (12.0f));
        g.drawText (TRANS ("No snapshots found"), rows, Justification::centred);
    }
}

void AutosaveDialog::mouseMove (const MouseEvent& e)
{
    setMouseCursor (rowAt (e.getPosition()) >= 0 ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
}

void AutosaveDialog::mouseDown (const MouseEvent& e)
{
    const int idx = rowAt (e.getPosition());
    if (idx >= 0)
    {
        selected = idx;
        updateButtons();
        repaint();
        return;
    }
    if (e.getNumberOfClicks() > 1) return;
}
}
