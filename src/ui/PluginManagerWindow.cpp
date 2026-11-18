/*
  ==============================================================================

   The plug-in manager window.

   Everything here runs on the message thread and therefore does no real work: the scan itself is
   on PluginScanner's thread (see engine/PluginScanner.h). The window polls the progress counters
   with a Timer and repaints, which is what keeps it responsive - and out of the Windows "not
   responding" state - while a plug-in is loaded in a helper process.

  ==============================================================================
*/

#include "PluginManagerWindow.h"
#include "../engine/PluginManager.h"
#include "../engine/PluginScanProcess.h"
#include "Theme.h"
#include "WindowChrome.h"

namespace sx
{
using namespace juce;

namespace
{
    enum ColumnIds { nameCol = 1, formatCol = 2, categoryCol = 3, manufacturerCol = 4, descriptionCol = 5 };

    enum MenuIds
    {
        kScanEverything = 1,
        kScanFolder,
        kRescanFailed,
        kRemoveMissing,
        kShowReport,
        kShowLog
    };

    enum RowMenuIds { kReenable = 1, kRemoveRow, kRevealFile, kRevealLog };

    String formatOfFile (const File& file)
    {
        auto ext = file.getFileExtension().toUpperCase();
        return ext.startsWithChar ('.') ? ext.substring (1) : ext;
    }
}

//==============================================================================
class PluginManagerWindow::ListPanel final : public Component,
                                            public TableListBoxModel,
                                            private Timer,
                                            private ChangeListener,
                                            private FileDragAndDropTarget
{
public:
    explicit ListPanel (PluginManager& pm) : plugins (pm)
    {
        table.setRowHeight (20);
        table.setHeaderHeight (22);
        table.setMultipleSelectionEnabled (true);
        table.setColour (ListBox::backgroundColourId, theme::laneA);
        table.setColour (ListBox::outlineColourId, theme::line);
        table.setOutlineThickness (1);

        auto& header = table.getHeader();
        header.addColumn (TRANS ("Name"),         nameCol,         220, 100, 700, TableHeaderComponent::defaultFlags | TableHeaderComponent::sortedForwards);
        header.addColumn (TRANS ("Format"),       formatCol,        70,  60,  90, TableHeaderComponent::notResizable);
        header.addColumn (TRANS ("Category"),     categoryCol,     110,  80, 220);
        header.addColumn (TRANS ("Manufacturer"), manufacturerCol, 180, 100, 320);
        header.addColumn (TRANS ("Description"),  descriptionCol,  300, 100, 520, TableHeaderComponent::notSortable);

        table.setModel (this);
        addAndMakeVisible (table);

        emptyHint.setText (TRANS ("No plug-ins have been scanned yet.") + "\n"
                             + TRANS ("Press Scan / Options to look for them."),
                           dontSendNotification);
        emptyHint.setJustificationType (Justification::centred);
        emptyHint.setFont (theme::font (13.0f));
        emptyHint.setColour (Label::textColourId, theme::textDim);
        emptyHint.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (emptyHint);

        statusLabel.setFont (theme::font (12.5f));
        statusLabel.setColour (Label::textColourId, theme::textDim);
        statusLabel.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (statusLabel);

        progressBar.setPercentageDisplay (false);
        addAndMakeVisible (progressBar);
        progressBar.setVisible (false);

        scanButton.setButtonText (TRANS ("Scan / Options"));
        scanButton.onClick = [this] { showOptions(); };
        addAndMakeVisible (scanButton);

        stopButton.setButtonText (TRANS ("Stop"));
        stopButton.onClick = [this]
        {
            plugins.scanner().cancel();
            statusLabel.setText (TRANS ("Stopping..."), dontSendNotification);
        };
        addAndMakeVisible (stopButton);
        stopButton.setVisible (false);

        // Stop ends the whole scan; Skip only drops the plug-in that is loading right now. That is
        // what a user wants when one plug-in takes minutes (or hangs) and the rest is fine.
        skipButton.setButtonText (TRANS ("Skip Plug-in"));
        skipButton.onClick = [this] { skipCurrentPlugin(); };
        addAndMakeVisible (skipButton);
        skipButton.setVisible (false);

        summaryLabel.setFont (theme::font (12.0f));
        summaryLabel.setColour (Label::textColourId, theme::textFaint);
        summaryLabel.setJustificationType (Justification::centredRight);
        addAndMakeVisible (summaryLabel);

        plugins.known.addChangeListener (this);

        // no blacklist read while a scan is running: KnownPluginList hands out that array without
        // a lock, and reading it from here while the scan thread appends to it is a data race
        const bool scanning = plugins.scanner().isScanning();
        refreshRows (! scanning);
        if (scanning) beginScanUI();
    }

    ~ListPanel() override
    {
        stopTimer();
        plugins.known.removeChangeListener (this);
    }

    void refreshTheme()
    {
        table.setColour (ListBox::backgroundColourId, theme::laneA);
        table.setColour (ListBox::outlineColourId, theme::line);
        emptyHint.setColour (Label::textColourId, theme::textDim);
        statusLabel.setColour (Label::textColourId, theme::textDim);
        summaryLabel.setColour (Label::textColourId, theme::textFaint);
        table.repaint();
        repaint();
    }

    bool startEverything()
    {
        if (! plugins.scanner().scanEverything()) { announceBusy(); return false; }
        beginScanUI();
        return true;
    }

    bool startFolder (const File& folder)
    {
        if (! plugins.scanner().scanFolder (folder, true)) { announceBusy(); return false; }
        beginScanUI();
        return true;
    }

    /** Same, without adding the folder to the remembered scan paths (used by --selftest). */
    bool startFolderOnce (const File& folder)
    {
        if (! plugins.scanner().scanFolder (folder, false)) { announceBusy(); return false; }
        beginScanUI();
        return true;
    }

    void startFiles (const StringArray& files)
    {
        if (! plugins.scanner().scanFiles (files)) { announceBusy(); return; }
        beginScanUI();
    }

    void paint (Graphics& g) override { g.fillAll (theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);

        auto bar = r.removeFromBottom (74);
        table.setBounds (r);
        emptyHint.setBounds (r.withTrimmedTop (table.getHeaderHeight()));

        statusLabel.setBounds (bar.removeFromTop (18));
        bar.removeFromTop (2);
        progressBar.setBounds (bar.removeFromTop (8));
        bar.removeFromTop (6);

        auto buttons = bar.removeFromTop (26);
        scanButton.setBounds (buttons.removeFromLeft (128));
        buttons.removeFromLeft (6);
        stopButton.setBounds (buttons.removeFromLeft (70));
        buttons.removeFromLeft (6);
        skipButton.setBounds (buttons.removeFromLeft (104));
        summaryLabel.setBounds (buttons);
    }

    //== TableListBoxModel =================================================================
    int getNumRows() override { return (int) rows.size(); }

    void paintRowBackground (Graphics& g, int row, int width, int height, bool rowIsSelected) override
    {
        g.setColour (rowIsSelected ? theme::accentSoft : (row % 2 != 0 ? theme::laneB : theme::laneA));
        g.fillRect (0, 0, width, height);
    }

    void paintCell (Graphics& g, int row, int columnId, int width, int height, bool) override
    {
        if (row < 0 || row >= (int) rows.size()) return;
        const auto& item = rows[(size_t) row];

        String text;
        switch (columnId)
        {
            case nameCol:         text = item.name; break;
            case formatCol:       text = item.format; break;
            case categoryCol:     text = item.category.isNotEmpty() ? item.category : "-"; break;
            case manufacturerCol: text = item.manufacturer; break;
            case descriptionCol:  text = item.description; break;
            default: break;
        }

        if (text.isEmpty()) return;

        g.setColour (item.blacklisted ? theme::record : (columnId == nameCol ? theme::text : theme::textDim));
        g.setFont (theme::font (12.5f, columnId == nameCol));
        g.drawFittedText (text, 5, 0, width - 8, height, Justification::centredLeft, 1, 0.85f);
    }

    void cellClicked (int rowNumber, int columnId, const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && rowNumber >= 0 && rowNumber < (int) rows.size())
            showRowMenu (rowNumber, e.getScreenPosition());
    }

    void deleteKeyPressed (int) override { removeSelected(); }

    void sortOrderChanged (int newSortColumnId, bool forwards) override
    {
        switch (newSortColumnId)
        {
            case nameCol:         plugins.known.sort (KnownPluginList::sortAlphabetically, forwards); break;
            case formatCol:       plugins.known.sort (KnownPluginList::sortByFormat, forwards); break;
            case categoryCol:     plugins.known.sort (KnownPluginList::sortByCategory, forwards); break;
            case manufacturerCol: plugins.known.sort (KnownPluginList::sortByManufacturer, forwards); break;
            default: break;
        }
    }

    //== FileDragAndDropTarget =============================================================
    bool isInterestedInFileDrag (const StringArray& files) override { return ! files.isEmpty(); }

    void filesDropped (const StringArray& dropped, int, int) override
    {
        // a dropped folder is scanned (and remembered for next time); dropped plug-in files are
        // scanned exactly as they are
        FileSearchPath folders;
        StringArray files;

        for (auto& name : dropped)
        {
            const File f (name);
            if (f.isDirectory()) folders.addIfNotAlreadyThere (f);
            else                 files.add (name);
        }

        if (folders.getNumPaths() == 0 && files.isEmpty()) return;

        if (! plugins.scanner().scan (folders, files, true)) { announceBusy(); return; }
        beginScanUI();
    }

private:
    struct Row
    {
        String name, format, category, manufacturer, description;
        PluginDescription descriptionForRemoval;
        String file;
        bool blacklisted = false;
    };

    //== scan bar ==========================================================================
    void announceBusy()
    {
        statusLabel.setText (TRANS ("A scan is already running."), dontSendNotification);
    }

    void beginScanUI()
    {
        progressValue = 0.0;
        progressBar.setVisible (true);
        stopButton.setVisible (true);
        skipButton.setVisible (true);
        skipButton.setEnabled (false);       // nothing is loading yet while the folders are walked
        skippedFlashUntil = 0;
        scanButton.setEnabled (false);
        statusLabel.setText (TRANS ("Looking for plug-in files..."), dontSendNotification);
        startTimerHz (10);
    }

    void endScanUI (const String& status)
    {
        stopTimer();
        progressBar.setVisible (false);
        stopButton.setVisible (false);
        skipButton.setVisible (false);
        scanButton.setEnabled (true);
        statusLabel.setText (status, dontSendNotification);
    }

    /** Drops the plug-in that is loading right now; the scan itself carries on. */
    void skipCurrentPlugin()
    {
        const auto dropped = plugins.scanner().skipCurrent();

        if (dropped.isEmpty())
        {
            // Either the scan is between two files, or that file is already in the list and never
            // got a helper process of its own: nothing is loading, so nothing was skipped.
            statusLabel.setText (TRANS ("Nothing is loading right now - nothing was skipped."),
                                 dontSendNotification);
            return;
        }

        skippedFlash = TRANS ("Skipped") + " " + File (dropped).getFileName();
        skippedFlashUntil = Time::getMillisecondCounter() + 2000;
        statusLabel.setText (skippedFlash, dontSendNotification);
    }

    void timerCallback() override
    {
        auto& scanner = plugins.scanner();

        if (scanner.isScanning())
        {
            // a skip is only possible while a plug-in is actually being loaded: during the folder
            // walk there is nothing to drop (and Stop is the button for that)
            skipButton.setEnabled (scanner.getCurrentFile().isNotEmpty());

            if (scanner.isLookingForFiles())
            {
                progressValue = 0.0;
                statusLabel.setText (TRANS ("Looking for plug-in files..."), dontSendNotification);
            }
            else
            {
                const int total = jmax (1, scanner.getNumTotal());
                progressValue = jlimit (0.0, 1.0, (double) scanner.getNumScanned() / (double) total);

                String text = TRANS ("Scanning") + " " + String (scanner.getNumScanned()) + "/"
                                  + String (scanner.getNumTotal()) + "  " + scanner.getCurrentFile();

                if (const int skipped = scanner.getNumSkipped(); skipped > 0)
                    text << "   " << TRANS ("Skipped: ") << String (skipped);

                // say which file was just dropped, before the line goes back to plain progress
                if (Time::getMillisecondCounter() < skippedFlashUntil)
                    text = skippedFlash;

                statusLabel.setText (text, dontSendNotification);
            }

            // the list grows while the scan runs. Only the plug-in array is safe to read then, so
            // the deactivated entries only show up once the scan is over.
            if (plugins.known.getNumTypes() != knownTypes)
                refreshRows (false);

            return;
        }

        finishScan();
    }

    void finishScan()
    {
        const bool cancelled = plugins.scanner().wasCancelled();
        const auto failures = plugins.scanner().getFailures();
        const int numSkipped = plugins.scanner().getNumSkipped();

        refreshRows (true);
        plugins.save();              // a finished scan should survive a crash right after it

        // No message box here: the plug-ins that failed are now in the list (in red, with the
        // reason in the Description column), and the full report is one menu click away. A dialog
        // after every scan with one broken plug-in would just be in the way.
        auto status = cancelled ? TRANS ("Scan stopped.") : TRANS ("Scan complete.");
        status << "   " << TRANS ("Plug-ins: ") << String ((int) plugins.known.getNumTypes());
        if (numSkipped > 0)
            status << "   " << TRANS ("Skipped: ") << String (numSkipped);
        if (! failures.isEmpty())
            status << "   " << TRANS ("Failed: ") << String (failures.size())
                   << "  (" << TRANS ("Scan report") << ")";

        endScanUI (status);
        lastFailures = failures;
        lastSkipped = plugins.scanner().getSkippedFiles();

        // the deactivated entries are at the end of the (alphabetical) list: scroll to them, so a
        // scan that skipped something does not look like it just worked
        if (! failures.isEmpty())
        {
            const int firstFailed = (int) plugins.known.getNumTypes();
            if (firstFailed < (int) rows.size())
            {
                table.selectRow (firstFailed);
                table.scrollToEnsureRowIsOnscreen (firstFailed);
            }
        }
    }

    void showReport()
    {
        if (lastFailures.isEmpty() && lastSkipped.isEmpty()) return;

        StringArray lines;

        if (! lastFailures.isEmpty())
        {
            lines.add (TRANS ("These plug-ins could not be scanned and are deactivated:"));
            lines.add ("");

            const int shown = jmin (lastFailures.size(), 12);
            for (int i = 0; i < shown; ++i)
            {
                auto& f = lastFailures.getReference (i);
                lines.add (File (f.file).getFileName() + "\n    " + f.reason);
            }
            if (lastFailures.size() > shown)
                lines.add ("...");
        }

        // files the user dropped mid-scan are not failures, but they are worth naming: the list
        // does not show them anywhere, and "why is this plug-in missing" is exactly the question
        if (! lastSkipped.isEmpty())
        {
            if (lines.size() > 0) lines.add ("");
            lines.add (TRANS ("Skipped at your request (nothing about them was recorded):"));
            lines.add ("");
            for (auto& file : lastSkipped)
                lines.add (File (file).getFileName());
        }

        lines.add ("");
        lines.add (TRANS ("The full report is in the scan log."));

        // a scan that only dropped what the user asked it to drop is not a warning
        AlertWindow::showMessageBoxAsync (lastFailures.isEmpty() ? MessageBoxIconType::InfoIcon
                                                                 : MessageBoxIconType::WarningIcon,
                                          TRANS ("Scan report"),
                                          lines.joinIntoString ("\n"));
    }

    //== options menu ======================================================================
    void showOptions()
    {
        PopupMenu menu;
        menu.addItem (kScanEverything, TRANS ("Scan all plug-in folders"));
        menu.addItem (kScanFolder, TRANS ("Scan a folder... (and remember it)"));
        menu.addSeparator();
        menu.addItem (kRescanFailed, TRANS ("Re-scan the plug-ins that could not be scanned"),
                      ! plugins.known.getBlacklistedFiles().isEmpty());
        menu.addItem (kRemoveMissing, TRANS ("Remove plug-ins whose files no longer exist"));
        menu.addSeparator();
        menu.addItem (kShowReport, TRANS ("Scan report"),
                      ! lastFailures.isEmpty() || ! lastSkipped.isEmpty());
        menu.addItem (kShowLog, TRANS ("Open the scan log"), OutOfProcessScanner::getLogFile().existsAsFile());

        menu.showMenuAsync (PopupMenu::Options().withTargetComponent (&scanButton).withMinimumWidth (250),
                            [this] (int result)
        {
            switch (result)
            {
                case kScanEverything: startEverything(); break;
                case kScanFolder:     chooseFolder(); break;
                case kRescanFailed:   rescanFailed(); break;
                case kRemoveMissing:  removeMissingFiles(); break;
                case kShowReport:     showReport(); break;
                case kShowLog:        OutOfProcessScanner::getLogFile().revealToUser(); break;
                default: break;
            }
        });
    }

    void chooseFolder()
    {
        folderChooser = std::make_shared<FileChooser> (TRANS ("Choose a folder with plug-ins"),
                                                       File::getSpecialLocation (File::globalApplicationsDirectory),
                                                       String(), true);
        folderChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                                    [this] (const FileChooser& fc)
        {
            const auto folder = fc.getResult();
            if (folder == File() || ! folder.isDirectory()) return;
            startFolder (folder);
        });
    }

    void rescanFailed()
    {
        const auto blacklisted = plugins.known.getBlacklistedFiles();
        if (blacklisted.isEmpty()) return;

        // give them another chance: un-deactivate first, then load exactly those files again
        for (auto& file : blacklisted)
            plugins.known.removeFromBlacklist (file);

        refreshRows (true);
        startFiles (blacklisted);
    }

    void removeMissingFiles()
    {
        int removed = 0;

        for (auto& d : plugins.known.getTypes())
        {
            if (d.pluginFormatName.equalsIgnoreCase ("VST3") || d.pluginFormatName.equalsIgnoreCase ("VST"))
            {
                const File file (d.fileOrIdentifier);
                if (file.getFullPathName().isNotEmpty() && ! file.exists())
                {
                    plugins.known.removeType (d);
                    ++removed;
                }
            }
        }

        // a deactivated entry can point at a file that is gone as well
        const auto blacklisted = plugins.known.getBlacklistedFiles();   // copy: the loop removes entries
        for (auto& file : blacklisted)
            if (! File (file).exists())
            {
                plugins.known.removeFromBlacklist (file);
                ++removed;
            }

        refreshRows (true);
        plugins.save();
        statusLabel.setText (removed > 0 ? TRANS ("Removed the plug-ins whose files no longer exist.")
                                         : TRANS ("Every scanned plug-in file still exists."),
                             dontSendNotification);
    }

    //== rows ==============================================================================
    void refreshRows (bool rereadBlacklist)
    {
        const ScopedValueSetter<bool> guard (refreshing, true);

        if (rereadBlacklist)
        {
            blacklist = plugins.known.getBlacklistedFiles();
            reasons = OutOfProcessScanner::getFailureReasons();
        }

        rows.clear();

        const auto types = plugins.known.getTypes();
        knownTypes = types.size();
        rows.reserve ((size_t) types.size() + (size_t) blacklist.size());

        for (auto& d : types)
        {
            Row r;
            r.name = d.name;
            r.format = d.pluginFormatName;
            r.category = d.category;
            r.manufacturer = d.manufacturerName;
            r.description = describe (d);
            r.descriptionForRemoval = d;
            rows.push_back (std::move (r));
        }

        for (auto& file : blacklist)
        {
            Row r;
            r.blacklisted = true;
            r.file = file;
            r.name = File (file).getFileName();
            r.format = formatOfFile (File (file));
            r.category = "-";
            r.manufacturer = File (file).getParentDirectory().getFileName();
            r.description = TRANS ("Deactivated: ") + reasons.getValue (file, TRANS ("scan failed"));
            rows.push_back (std::move (r));
        }

        table.updateContent();
        emptyHint.setVisible (rows.empty());
        updateSummary();
    }

    static String describe (const PluginDescription& d)
    {
        StringArray items;
        if (d.descriptiveName.isNotEmpty() && d.descriptiveName != d.name) items.add (d.descriptiveName);
        if (d.version.isNotEmpty()) items.add (d.version);
        if (d.isInstrument) items.add (TRANS ("Instrument"));
        if (PluginManager::isAraCapable (d)) items.add ("ARA");
        items.removeEmptyStrings();
        return items.joinIntoString (" - ");
    }

    void updateSummary()
    {
        auto text = TRANS ("Plug-ins: ") + String ((int) plugins.known.getNumTypes());
        if (const int ara = plugins.araCapableCount(); ara > 0)
            text << "   ARA: " << ara;
        if (blacklist.size() > 0)
            text << "   " << TRANS ("Deactivated") << ": " << (int) blacklist.size();
        summaryLabel.setText (text, dontSendNotification);
    }

    void showRowMenu (int row, Point<int> screenPos)
    {
        const auto item = rows[(size_t) row];
        const File file (item.blacklisted ? item.file : item.descriptionForRemoval.fileOrIdentifier);

        PopupMenu menu;
        if (item.blacklisted)
        {
            menu.addItem (kReenable, TRANS ("Re-enable (scan it again)"));
            menu.addItem (kRevealLog, TRANS ("Open the scan log"));
        }
        else
        {
            menu.addItem (kRemoveRow, TRANS ("Remove from the list"));
        }
        menu.addItem (kRevealFile, TRANS ("Show the folder containing the plug-in"),
                      file.getParentDirectory().isDirectory());

        menu.showMenuAsync (PopupMenu::Options().withTargetScreenArea (Rectangle<int> (screenPos.x, screenPos.y, 1, 1)),
                            [this, item, file] (int result)
        {
            if (result == kReenable)
            {
                plugins.known.removeFromBlacklist (item.file);
                refreshRows (true);
                startFiles ({ item.file });
            }
            else if (result == kRemoveRow)
            {
                plugins.known.removeType (item.descriptionForRemoval);
                refreshRows (true);
                plugins.save();
            }
            else if (result == kRevealFile)
            {
                if (file.getParentDirectory().isDirectory()) file.revealToUser();
            }
            else if (result == kRevealLog)
            {
                OutOfProcessScanner::getLogFile().revealToUser();
            }
        });
    }

    void removeSelected()
    {
        const auto selected = table.getSelectedRows();
        if (selected.isEmpty()) return;

        for (int i = selected.size(); --i >= 0;)
        {
            const int row = selected[i];
            if (row < 0 || row >= (int) rows.size()) continue;
            const auto& item = rows[(size_t) row];
            if (item.blacklisted) plugins.known.removeFromBlacklist (item.file);
            else                  plugins.known.removeType (item.descriptionForRemoval);
        }

        refreshRows (true);
        plugins.save();
    }

    void changeListenerCallback (ChangeBroadcaster*) override
    {
        if (refreshing) return;
        // the deactivated array may only be read while nothing is scanning it
        refreshRows (! plugins.scanner().isScanning());
    }

    PluginManager& plugins;
    TableListBox table;
    Label emptyHint, statusLabel, summaryLabel;
    TextButton scanButton, stopButton, skipButton;
    double progressValue = 0.0;
    ProgressBar progressBar { progressValue };
    std::shared_ptr<FileChooser> folderChooser;

    std::vector<Row> rows;
    StringArray blacklist;
    StringPairArray reasons;
    Array<PluginScanner::Failure> lastFailures;
    StringArray lastSkipped;
    String skippedFlash;                    // "Skipped <file>", shown for a moment after a click
    uint32 skippedFlashUntil = 0;
    int knownTypes = 0;
    bool refreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ListPanel)
};

//==============================================================================
// The window is embedded into the main window's overlay (removeFromDesktop +
// addAndMakeVisible in PluginManager::showScannerWindow), so it never becomes the active
// TopLevelWindow and DocumentWindow::activeWindowStatusChanged keeps the title-bar buttons
// disabled - and a disabled close button silently swallows the click, which is why the red
// X appeared dead.
void PluginManagerWindow::refreshTitleBarButtons()
{
    for (auto* child : getChildren())
        if (auto* b = dynamic_cast<Button*> (child))
            b->setEnabled (true);
}

void PluginManagerWindow::activeWindowStatusChanged()
{
    DocumentWindow::activeWindowStatusChanged();
    refreshTitleBarButtons();
}

PluginManagerWindow::PluginManagerWindow (PluginManager& pm, std::function<void()> onClose)
    : DocumentWindow (TRANS ("Plug-in Manager"), theme::panel, DocumentWindow::closeButton, false),
      closeCallback (std::move (onClose))
{
    setUsingNativeTitleBar (false);

    auto* list = new ListPanel (pm);
    panel = list;
    list->setSize (860, 540);
    setContentOwned (list, true);
    setResizable (true, false);
    setResizeLimits (620, 400, 1800, 1400);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
    removePlatformWindowBorder (*this);
    refreshTitleBarButtons();           // embedded windows start with the buttons disabled
}

PluginManagerWindow::~PluginManagerWindow() {}

void PluginManagerWindow::refreshTheme()
{
    setColour (DocumentWindow::backgroundColourId, theme::panel);
    if (panel != nullptr) panel->refreshTheme();
    repaint();
}

void PluginManagerWindow::closeButtonPressed()
{
    if (closeCallback != nullptr) closeCallback();
}

bool PluginManagerWindow::scanFolder (const File& folder, bool rememberForNextSession)
{
    return panel != nullptr && (rememberForNextSession ? panel->startFolder (folder) : panel->startFolderOnce (folder));
}

bool PluginManagerWindow::scanEverything()
{
    return panel != nullptr && panel->startEverything();
}
}
