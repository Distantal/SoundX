#include "PluginPicker.h"
#include "../engine/BuiltInEffects.h"
#include "SlotMenus.h"      // showError
#include "Theme.h"

namespace sx
{
using namespace juce;

namespace
{
    // loading a plug-in can take a second, so the cursor says the app is busy (the slot menu does
    // the same thing for its own insertion path)
    struct WaitCursor
    {
        WaitCursor() { MouseCursor::showWaitCursor(); }
        ~WaitCursor() { MouseCursor::hideWaitCursor(); }
    };

    bool hasEveryWord (const String& text, const StringArray& words)
    {
        for (auto& w : words)
            if (! text.contains (w)) return false;
        return true;
    }

    // The search box: the keys that belong to the result list are taken here, everything else
    // goes to the text editor as usual.
    class SearchEditor : public TextEditor
    {
    public:
        std::function<bool (const KeyPress&)> onNavKey;

        bool keyPressed (const KeyPress& k) override
        {
            if (onNavKey != nullptr && onNavKey (k)) return true;
            return TextEditor::keyPressed (k);
        }
    };

    // Draws 'text' with every occurrence of one of 'tokens' picked out in 'hit', so it is obvious
    // why a plug-in matched the search.
    void drawHighlighted (Graphics& g, const String& text, const StringArray& tokens,
                          Rectangle<int> area, const Font& font, Colour base, Colour hit)
    {
        const auto lower = text.toLowerCase();
        std::vector<std::pair<int, int>> spans;
        for (auto& t : tokens)
        {
            if (t.isEmpty()) continue;
            int from = 0;
            for (;;)
            {
                const int at = lower.indexOf (from, t);
                if (at < 0) break;
                spans.push_back ({ at, at + t.length() });
                from = at + jmax (1, t.length());
            }
        }

        if (spans.empty())
        {
            g.setColour (base);
            g.setFont (font);
            g.drawText (text, area, Justification::centredLeft, true);
            return;
        }

        std::sort (spans.begin(), spans.end());
        AttributedString out;
        int pos = 0;
        for (auto& s : spans)
        {
            const int a = jmax (pos, s.first);
            const int b = jmax (a, s.second);
            if (a > pos) out.append (text.substring (pos, a), font, base);
            if (b > a) out.append (text.substring (a, b), font, hit);
            pos = b;
        }
        if (pos < text.length()) out.append (text.substring (pos), font, base);
        out.setJustification (Justification::centredLeft);
        out.draw (g, area.toFloat());
    }

    //==============================================================================
    class PluginPickerPanel : public Component,
                              public ListBoxModel
    {
    public:
        PluginPickerPanel (Array<PluginDescription> items, bool instrumentSlotToUse, bool replacing,
                           bool scannedSomething, bool multiWindowsToUse)
            : list (std::move (items)),
              instrumentSlot (instrumentSlotToUse),
              noScanYet (! scannedSomething)
        {
            heading = instrumentSlot ? (replacing ? TRANS ("Replace Instrument") : TRANS ("Choose Instrument"))
                                     : (replacing ? TRANS ("Replace Effect") : TRANS ("Add Effect"));
            actionName = replacing ? TRANS ("Replace") : TRANS ("Insert");

            // vendor folder: "All", then one entry per manufacturer (SoundX first)
            vendorBox.addItem (TRANS ("All vendors"), 1);
            {
                auto vendors = pluginPickerVendors (list);
                int id = 2;
                for (auto& v : vendors)
                {
                    int count = 0;
                    for (auto& d : list) if ((d.manufacturerName.trim().isEmpty() ? String (TRANS ("Unknown")) : d.manufacturerName.trim()) == v) ++count;
                    vendorBox.addItem (v + "  (" + String (count) + ")", id++);
                }
            }
            vendorBox.setSelectedId (1, dontSendNotification);
            vendorBox.setTooltip (TRANS ("Vendor folder: show only the plug-ins of one manufacturer"));
            vendorBox.onChange = [this] { rebuild(); };
            addAndMakeVisible (vendorBox);

            search.setTextToShowWhenEmpty (TRANS ("Search by name, manufacturer or category"), theme::textFaint);
            search.setFont (theme::font (13.5f));
            search.setColour (TextEditor::backgroundColourId, theme::white);
            search.setColour (TextEditor::textColourId, theme::text);
            search.setColour (TextEditor::highlightColourId, theme::accentSoft);
            search.setColour (TextEditor::highlightedTextColourId, theme::text);
            search.setColour (TextEditor::outlineColourId, theme::line);
            search.setColour (TextEditor::focusedOutlineColourId, theme::accent);
            search.onTextChange = [this] { rebuild(); };
            search.onNavKey = [this] (const KeyPress& k) { return navKey (k); };
            addAndMakeVisible (search);

            results.setRowHeight (kRowH);
            results.setMultipleSelectionEnabled (false);
            results.setColour (ListBox::backgroundColourId, theme::laneA);
            results.setColour (ListBox::outlineColourId, theme::line);
            results.setOutlineThickness (1);
            results.setModel (this);
            addAndMakeVisible (results);

            emptyHint.setJustificationType (Justification::centred);
            emptyHint.setFont (theme::font (12.5f));
            emptyHint.setColour (Label::textColourId, theme::textFaint);
            emptyHint.setInterceptsMouseClicks (false, false);
            addChildComponent (emptyHint);

            actionBtn.setButtonText (actionName);
            actionBtn.onClick = [this] { choose (results.getSelectedRow()); };
            addAndMakeVisible (actionBtn);

            cancelBtn.setButtonText (TRANS ("Cancel"));
            cancelBtn.onClick = [this] { if (onCancel) onCancel(); };
            addAndMakeVisible (cancelBtn);

            scanBtn.setButtonText (TRANS ("Scan Plug-ins..."));
            scanBtn.setTooltip (TRANS ("Scan the plug-in folders - the search covers whatever has been scanned"));
            scanBtn.onClick = [this] { if (onScan) onScan(); };
            addAndMakeVisible (scanBtn);
            scanBtn.setVisible (noScanYet);   // only offered while nothing has been scanned

            // The insert-time choice the user asked for: when this is off, inserting a plug-in
            // hides the panel of the one before it; when it is on, the panels stay side by side.
            keepMultiple.setButtonText (TRANS ("Keep other panels open"));
            keepMultiple.setTooltip (TRANS ("When off, opening a plug-in panel replaces the previous one."));
            keepMultiple.setToggleState (multiWindowsToUse, dontSendNotification);
            keepMultiple.onClick = [this]
            {
                if (onMultiWindowsChanged != nullptr) onMultiWindowsChanged (keepMultiple.getToggleState());
            };
            addAndMakeVisible (keepMultiple);

            setSize (600, 440);
            rebuild();
        }

        std::function<void (const PluginDescription&)> onChoose;
        std::function<void()> onCancel;
        std::function<void()> onScan;
        std::function<void (bool)> onMultiWindowsChanged;

        void focusSearch() { search.grabKeyboardFocus(); }

        // sets the search field without firing the callbacks (self test / visual check)
        void setQuery (const String& q)
        {
            search.setText (q, dontSendNotification);
            rebuild();
        }

        //== list model ==========================================================================
        int getNumRows() override { return (int) matches.size(); }

        void paintListBoxItem (int row, Graphics& g, int width, int height, bool selected) override
        {
            if (row < 0 || row >= (int) matches.size()) return;
            const auto& d = list.getReference (matches[(size_t) row]);
            auto r = Rectangle<int> (0, 0, width, height);

            if (selected)
            {
                g.setColour (theme::accentSoft);
                g.fillRect (r);
                g.setColour (theme::accent);
                g.fillRect (0, 0, 3, height);
            }
            else if ((row & 1) != 0)
            {
                g.setColour (theme::panelAlt.withAlpha (0.6f));
                g.fillRect (r);
            }

            // right: what kind of plug-in it is and who made it
            StringArray trailing;
            if (d.pluginFormatName.isNotEmpty()) trailing.add (d.pluginFormatName);
            if (d.manufacturerName.isNotEmpty()) trailing.add (d.manufacturerName);
            if (d.category.isNotEmpty()) trailing.add (d.category);
            if (PluginManager::isAraCapable (d)) trailing.add ("ARA");
            const int rightW = jmin (width / 2, 250);
            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.0f));
            g.drawFittedText (trailing.joinIntoString ("  /  "), r.withTrimmedRight (10).removeFromRight (rightW),
                              Justification::centredRight, 1);

            String name = d.name;
            if (isBuiltInSynth (d) || isBuiltInSampler (d)) name << "  (" << TRANS ("built-in") << ")";
            drawHighlighted (g, name, tokens, r.withTrimmedLeft (12).withTrimmedRight (rightW + 18),
                             theme::font (12.5f, true), theme::text,
                             selected ? theme::accentDark : theme::accent);
        }

        // clicking a row keeps the keyboard in the search field, so typing carries on filtering
        // (the arrows / Enter are handled there too, see navKey)
        void listBoxItemClicked (int, const MouseEvent&) override { search.grabKeyboardFocus(); }
        void listBoxItemDoubleClicked (int row, const MouseEvent&) override { choose (row); }
        void returnKeyPressed (int row) override { choose (row); }

        //== painting / layout ===================================================================
        void paint (Graphics& g) override
        {
            g.fillAll (theme::panel);

            g.setColour (theme::text);
            g.setFont (theme::font (14.0f, true));
            g.drawText (heading, 16, 10, getWidth() - 32, 20, Justification::centredLeft);

            g.setColour (theme::textDim);
            g.setFont (theme::font (11.5f));
            g.drawText (TRANS ("Plug-ins") + ": " + String ((int) matches.size()) + " / " + String (list.size()),
                        16, 30, getWidth() - 32, 16, Justification::centredLeft);

            g.setColour (theme::textFaint);
            g.setFont (theme::font (11.0f));
            g.drawFittedText (TRANS ("Up / Down choose, Enter inserts, Esc closes"), hintArea,
                              Justification::centredLeft, 1);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (16, 0);
            r.removeFromTop (52);                         // heading + count are painted above
            auto topRow = r.removeFromTop (28);
            vendorBox.setBounds (topRow.removeFromLeft (jmin (190, topRow.getWidth() / 2)));
            topRow.removeFromLeft (8);
            search.setBounds (topRow);
            r.removeFromTop (10);

            auto buttons = r.removeFromBottom (28);
            hintArea = r.removeFromBottom (18);            // the keyboard hint sits above the buttons
            auto options = r.removeFromBottom (24);         // insert-time window choice
            keepMultiple.setBounds (options.removeFromLeft (jmin (260, options.getWidth())));
            r.removeFromBottom (4);
            results.setBounds (r);
            emptyHint.setBounds (r.reduced (24, 24));

            cancelBtn.setBounds (buttons.removeFromRight (96));
            buttons.removeFromRight (8);
            actionBtn.setBounds (buttons.removeFromRight (128));
            buttons.removeFromRight (12);
            if (scanBtn.isVisible()) scanBtn.setBounds (buttons.removeFromLeft (jmin (150, buttons.getWidth())));
        }

    private:
        static constexpr int kRowH = 26;

        void rebuild()
        {
            tokens = StringArray::fromTokens (search.getText().trim().toLowerCase(), " ", "");
            tokens.removeEmptyStrings();

            // the vendor folder narrows the list before the search runs
            Array<PluginDescription> pool;
            const int vid = vendorBox.getSelectedId();
            if (vid <= 1) pool = list;
            else
            {
                auto vendors = pluginPickerVendors (list);
                const int idx = vid - 2;
                if (idx >= 0 && idx < vendors.size())
                {
                    const auto want = vendors[idx];
                    for (auto& d : list)
                    {
                        auto v = d.manufacturerName.trim();
                        if (v.isEmpty()) v = TRANS ("Unknown");
                        if (v == want) pool.add (d);
                    }
                }
                else pool = list;
            }
            matches = searchPluginList (pool, search.getText());

            results.updateContent();
            if (! matches.empty()) results.selectRow (0, true, true);
            else results.deselectAllRows();

            emptyHint.setVisible (matches.empty());
            emptyHint.setText (list.isEmpty() ? TRANS ("No plug-ins have been scanned yet.")
                                             : TRANS ("No plug-in matches the search"),
                               dontSendNotification);
            actionBtn.setEnabled (! matches.empty());
            repaint();
        }

        void choose (int row)
        {
            if (row < 0 || row >= (int) matches.size()) return;
            if (onChoose) onChoose (list.getReference (matches[(size_t) row]));
        }

        void moveTo (int row)
        {
            if (matches.empty()) return;
            row = jlimit (0, (int) matches.size() - 1, row);
            results.selectRow (row);
            results.scrollToEnsureRowIsOnscreen (row);
        }

        void move (int delta)
        {
            if (matches.empty()) return;
            const int row = results.getSelectedRow();
            moveTo ((row < 0 ? 0 : row) + delta);
        }

        bool navKey (const KeyPress& k)
        {
            const int code = k.getKeyCode();
            if (code == KeyPress::upKey)       { move (-1); return true; }
            if (code == KeyPress::downKey)     { move (1); return true; }
            if (code == KeyPress::pageUpKey)   { move (-8); return true; }
            if (code == KeyPress::pageDownKey) { move (8); return true; }
            if (code == KeyPress::homeKey)     { moveTo (0); return true; }
            if (code == KeyPress::endKey)      { moveTo ((int) matches.size() - 1); return true; }
            if (code == KeyPress::returnKey)   { choose (results.getSelectedRow()); return true; }
            if (code == KeyPress::escapeKey)   { if (onCancel) onCancel(); return true; }
            return false;
        }

        Array<PluginDescription> list;
        std::vector<int> matches;
        StringArray tokens;
        bool instrumentSlot = false, noScanYet = false;
        String heading, actionName;
        Rectangle<int> hintArea;

        SearchEditor search;
        juce::ComboBox vendorBox;                  // the vendor folder filter (SoundX first)
        ListBox results { "pluginResults", this };
        Label emptyHint;
        juce::ToggleButton keepMultiple;
        TextButton actionBtn, cancelBtn, scanBtn;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginPickerPanel)
    };
}

//==============================================================================
Array<PluginDescription> pluginPickerList (PluginManager& plugins, bool instrumentSlot)
{
    auto list = instrumentSlot ? plugins.instruments() : plugins.effects();
    if (instrumentSlot)
    {
        // both built-in instruments are always playable without a plug-in, so they come first
        list.insert (0, makeBuiltInSynthSlot().desc);
        list.insert (1, makeBuiltInSamplerSlot().desc);
    }
    else
    {
        // the built-in effect family comes first: it needs no scan and always loads
        Array<PluginDescription> builtIns;
        for (auto& e : builtInEffects())
            builtIns.add (makeBuiltInEffectSlot (e.id).desc);
        builtIns.addArray (list);
        list = builtIns;
    }
    return list;
}

StringArray pluginPickerVendors (const Array<PluginDescription>& list)
{
    // "SoundX" (the built-in family) always comes first, then the rest alphabetically
    StringArray names;
    for (auto& d : list)
    {
        auto v = d.manufacturerName.trim();
        if (v.isEmpty()) v = TRANS ("Unknown");
        if (! names.contains (v)) names.add (v);
    }
    names.sort (true);
    const String builtInPrefix = "SoundX";
    for (int i = 0; i < names.size(); ++i)
        if (names[i].startsWith (builtInPrefix))
        {
            names.move (i, 0);
            break;
        }
    return names;
}

std::vector<int> searchPluginList (const Array<PluginDescription>& list, const String& query)
{
    const auto whole = query.trim().toLowerCase();
    auto words = StringArray::fromTokens (whole, " ", "");
    words.removeEmptyStrings();

    struct Hit { int index = 0; int rank = 0; String key; };
    std::vector<Hit> hits;
    hits.reserve ((size_t) list.size());

    for (int i = 0; i < list.size(); ++i)
    {
        const auto& d = list.getReference (i);
        const auto name = d.name.toLowerCase();
        const auto maker = d.manufacturerName.toLowerCase();
        const auto rest = (d.category + " " + d.descriptiveName + " " + d.pluginFormatName).toLowerCase();

        int rank = -1;
        if (whole.isEmpty())                     rank = 0;
        else if (name.startsWith (whole))        rank = 0;   // "pro" -> Pro-Q 3 first
        else if (name.contains (whole))          rank = 1;
        else if (hasEveryWord (name, words))     rank = 2;   // "pro fab" style, out of order
        else if (hasEveryWord (maker, words))    rank = 3;
        else if (hasEveryWord (rest, words))     rank = 4;
        else if (hasEveryWord (name + " " + maker + " " + rest, words)) rank = 5;

        if (rank >= 0) hits.push_back ({ i, rank, name + "|" + maker });
    }

    std::stable_sort (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b)
    {
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.key < b.key;                                // alphabetical inside one rank
    });

    std::vector<int> out;
    out.reserve (hits.size());
    for (auto& h : hits) out.push_back (h.index);
    return out;
}

Image renderPluginPickerPanel (const Array<PluginDescription>& list, bool instrumentSlot,
                               const String& query, bool anyScanned, bool replacing, bool multiWindows)
{
    PluginPickerPanel panel (list, instrumentSlot, replacing, anyScanned, multiWindows);
    panel.setQuery (query);
    return panel.createComponentSnapshot (panel.getLocalBounds(), true);
}

void showAddInstrumentPicker (AppState& app, int trackId)
{
    auto items = pluginPickerList (app.plugins, true);
    const bool scannedSomething = items.size() > 2;   // the two built-in instruments are not "scanned"
    auto* panel = new PluginPickerPanel (std::move (items), true, false, scannedSomething, app.multiPluginWindows);

    DialogWindow::LaunchOptions lo;
    lo.dialogTitle = TRANS ("Add Instrument to Track");
    lo.content.setOwned (panel);
    lo.dialogBackgroundColour = theme::panel;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = false;
    auto* dw = lo.launchAsync();

    auto* appPtr = &app;
    panel->onCancel = [dw] { dw->exitModalState (0); };
    panel->onScan = [dw, appPtr]
    {
        dw->exitModalState (0);
        appPtr->plugins.showScannerWindow();
    };
    panel->onMultiWindowsChanged = [appPtr] (bool on) { appPtr->setMultiPluginWindows (on); };
    panel->onChoose = [dw, appPtr, trackId] (PluginDescription desc)
    {
        dw->exitModalState (1);

        auto& a = *appPtr;
        String err;
        bool ok = false;
        {
            WaitCursor wait;
            ok = a.engine.addInstrument (trackId, desc, err);
        }
        if (! ok) { showError (TRANS ("Failed to load plug-in") + " \"" + desc.name + "\"\n" + err); return; }

        // open the new layer's editor so the user hears / sees it right away
        if (auto* t = a.project.findTrack (trackId))
        {
            auto& layers = t->extraInstruments;
            if (! layers.empty()) a.showInsertedPluginEditor (layers.back().uid);
        }
    };

    Component::SafePointer<PluginPickerPanel> safe (panel);
    MessageManager::callAsync ([safe] { if (safe != nullptr) safe->focusSearch(); });
}

void showPluginPicker (AppState& app, int trackId, int64 replaceUid, bool instrumentSlot)
{
    auto items = pluginPickerList (app.plugins, instrumentSlot);
    // "nothing scanned yet" means nothing of *this* kind (the two built-in instruments always exist)
    const bool scannedSomething = items.size() > (instrumentSlot ? 2 : 0);
    auto* panel = new PluginPickerPanel (std::move (items), instrumentSlot, replaceUid != 0, scannedSomething,
                                         app.multiPluginWindows);

    DialogWindow::LaunchOptions lo;
    lo.dialogTitle = instrumentSlot ? TRANS ("Search Instrument") : TRANS ("Search Plug-in");
    lo.content.setOwned (panel);
    lo.dialogBackgroundColour = theme::panel;
    lo.escapeKeyTriggersCloseButton = true;
    lo.useNativeTitleBar = true;
    lo.resizable = false;
    auto* dw = lo.launchAsync();

    auto* appPtr = &app;
    panel->onCancel = [dw] { dw->exitModalState (0); };
    panel->onScan = [dw, appPtr]
    {
        dw->exitModalState (0);
        appPtr->plugins.showScannerWindow();
    };
    panel->onMultiWindowsChanged = [appPtr] (bool on) { appPtr->setMultiPluginWindows (on); };
    panel->onChoose = [dw, appPtr, trackId, replaceUid, instrumentSlot] (PluginDescription desc)
    {
        dw->exitModalState (1);            // the dialog deletes itself on the next message loop pass

        auto& a = *appPtr;
        String err;
        bool ok = false;
        int64 newUid = 0;
        {
            WaitCursor wait;
            if (instrumentSlot) ok = a.engine.setInstrument (trackId, desc, err);
            else
            {
                ok = a.engine.addInsert (trackId, desc, err);
                if (ok) newUid = a.lastInsertUid (trackId);
                if (ok && replaceUid != 0) a.engine.removeSlot (trackId, replaceUid);
            }
        }

        if (! ok) { showError (TRANS ("Failed to load plug-in") + " \"" + desc.name + "\"\n" + err); return; }
        if (instrumentSlot)
        {
            if (auto* t = a.project.findTrack (trackId)) a.showInsertedPluginEditor (t->instrument.uid);
        }
        else
            a.showInsertedPluginEditor (newUid);
    };

    // the search field must have the keyboard as soon as the window is on screen
    Component::SafePointer<PluginPickerPanel> safe (panel);
    MessageManager::callAsync ([safe] { if (safe != nullptr) safe->focusSearch(); });
}
}
