#include "PluginWindows.h"
#include "engine/BuiltInEffects.h"
#include "Theme.h"
#include "UndoKeys.h"
#include "WindowChrome.h"

namespace sx
{
using namespace juce;

#if JUCE_WINDOWS
// Declared locally: <windows.h> defines a global Rectangle, which collides with juce::Rectangle.
extern "C" __declspec(dllimport) int __stdcall EnumChildWindows (void*, int (__stdcall*) (void*, void*), void*);
extern "C" __declspec(dllimport) int __stdcall IsWindowVisible (void*);
extern "C" __declspec(dllimport) int __stdcall GetClientRect (void*, void*);
extern "C" __declspec(dllimport) unsigned int __stdcall GetDpiForWindow (void*);
struct NativeClientRect { int left, top, right, bottom; };
struct NativeChildSearch { void* best = nullptr; int area = 0; };
static int __stdcall findLargestChild (void* hwnd, void* context)
{
    if (IsWindowVisible (hwnd) == 0) return 1;
    NativeClientRect r {};
    if (GetClientRect (hwnd, &r) != 0)
    {
        const int area = (r.right - r.left) * (r.bottom - r.top);
        auto* s = static_cast<NativeChildSearch*> (context);
        if (area > s->area) { s->area = area; s->best = hwnd; }
    }
    return 1;
}
#endif

    //== the three host icons, embedded in the window title bar -----------------------------------
    enum HostIcon { iconPower = 0, iconSidechain = 1, iconBypass = 2 };

    /** Themed placeholder shown while a plug-in's editor is being created. Building a big plug-in
        UI (Kontakt, Ozone) takes hundreds of milliseconds; doing that synchronously from show()
        froze the whole window and left a black rectangle where the native editor window had not
        painted yet. The panel chrome (title bar, close cross) comes up immediately with this
        placeholder and the editor is inserted a couple of frames later. */
    class PluginLoadingContent : public Component
    {
    public:
        explicit PluginLoadingContent (String title) : name (std::move (title)) {}

        void paint (Graphics& g) override
        {
            g.fillAll (theme::panelAlt);
            auto r = getLocalBounds().reduced (16);
            g.setColour (theme::textDim);
            g.setFont (theme::font (12.5f));
            g.drawFittedText (name, r.removeFromTop (18), Justification::centredLeft, 1);
            g.setColour (theme::textFaint);
            g.setFont (theme::font (12.0f));
            g.drawFittedText (TRANS ("Opening plug-in..."), r, Justification::centredTop, 2);
        }

    private:
        String name;
    };

    static void paintHostIcon (Graphics& g, Rectangle<float> b, int icon, bool active,
                               bool hovered, Colour onColour, bool dimmed = false)
    {
        g.saveState();
        if (dimmed) g.setOpacity (0.45f);
        g.setColour (active ? onColour : theme::panelDeep.withAlpha (hovered ? 0.9f : 0.55f));
        g.fillRoundedRectangle (b, 3.0f);
        g.setColour (active ? onColour.brighter (0.35f) : theme::gridBar);
        g.drawRoundedRectangle (b, 3.0f, 1.0f);
        const auto c = b.getCentre();
        const float s = 4.2f, th = 1.3f;
        g.setColour (active ? theme::onAccent : theme::textDim);
        if (icon == iconPower)
        {
            g.drawEllipse (c.x - s, c.y - s + 0.6f, s * 2.0f, s * 2.0f, th);
            g.drawLine (c.x, c.y - s - 1.2f, c.x, c.y - 0.4f, th);
        }
        else if (icon == iconSidechain)
        {
            // two links joined in the middle: the side-chain input
            g.drawRoundedRectangle (c.x - s - 0.8f, c.y - 2.2f, s * 1.15f, 4.4f, 2.0f, th);
            g.drawRoundedRectangle (c.x - s * 0.35f + 0.4f, c.y - 2.2f, s * 1.15f, 4.4f, 2.0f, th);
            g.drawLine (c.x - 1.2f, c.y, c.x + 1.2f, c.y, th);
        }
        else
        {
            Path bend;
            bend.startNewSubPath (c.x - s - 0.6f, c.y + 2.4f);
            bend.lineTo (c.x - 0.8f, c.y + 2.4f);
            bend.quadraticTo (c.x + 2.4f, c.y + 2.4f, c.x + 2.4f, c.y - 1.0f);
            bend.lineTo (c.x + 2.4f, c.y - s + 0.6f);
            g.strokePath (bend, PathStrokeType (th));
            g.drawLine (c.x - s - 0.6f, c.y - 2.4f, c.x + s + 0.6f, c.y - 2.4f, th * 0.8f);
        }
        g.restoreState();
    }

    /** A transparent click target over one of the title bar icons: real children receive the
        mouse before the window, so these always work no matter what the title bar does. */
    class IconHitArea : public Component
    {
    public:
        std::function<void()> onClick;
        std::function<void (bool)> onHover;
        std::function<const String()> tip;

        void mouseEnter (const MouseEvent&) override { if (onHover != nullptr) onHover (true); }
        void mouseExit (const MouseEvent&) override { if (onHover != nullptr) onHover (false); }

        // act on press, not on release: a click on a host button has to feel immediate
        void mouseDown (const MouseEvent& e) override
        {
            if (! getLocalBounds().contains (e.getPosition())) return;
            pressed = true;
            repaint();
            if (onClick != nullptr) onClick();
        }
        void mouseUp (const MouseEvent&) override { pressed = false; repaint(); }
        bool isPressed() const { return pressed; }

    private:
        bool pressed = false;
    };
class PluginWindows::Window : public Component,
                              private KeyListener,
                              private Timer,
                              private ComponentListener
{
public:
    Window (PluginWindows& o, std::shared_ptr<PluginInstance> inst, const String& title)
        : Component (title),
          owner (o), instance (std::move (inst))
    {
        // create the icon click targets first: the title bar calls below trigger resized(), which
        // places them, so they must exist before the title bar is switched
        for (int i = 0; i < 3; ++i)
        {
            auto* hit = new IconHitArea();
            hit->onHover = [this, i] (bool h) { hoverIcon = h ? i : -1; repaint(); };
            hits[i] = hit;
            addAndMakeVisible (hit);
        }
        hits[iconPower]->onClick = [this]
        {
            if (owner.onSetSlotBypass == nullptr) return;
            owner.onSetSlotBypass (instance->ownerTrack, instance->uid, ! instance->bypass.load());
            repaint();
        };
        hits[iconBypass]->onClick = [this]
        {
            instance->editorBypass.store (! instance->editorBypass.load());
            repaint();
        };
        hits[iconSidechain]->onClick = [this]
        {
            if (! supportsSidechain())
            {
                PopupMenu m;
                m.addItem (-1, TRANS ("Side-chain input is only used by SoundX built-in effects"), false);
                m.showMenuAsync (PopupMenu::Options().withTargetComponent (hits[iconSidechain]));
                return;
            }
            if (owner.showSidechainMenu != nullptr)
                owner.showSidechainMenu (instance->uid, *hits[iconSidechain]);
        };
        // all three icons are always shown; the side-chain one is muted for third-party plug-ins

        // The close cross is our own hit target: the DocumentWindow button is not reliable
        // without a native peer, and a panel that cannot be closed is worse than no button.
        closeHit = new IconHitArea();
        closeHit->onClick = [this] { owner.close (instance->uid); };
        closeHit->onHover = [this] (bool h) { closeHover = h; repaint (closeRect()); };
        addAndMakeVisible (closeHit);

        // the panel draws its own title bar so the three host icons can be embedded in it
        setOpaque (true);


        // our own editors draw their toolbar (enable / side-chain / bypass) under the title: they
        // need a way back into the slot, which is what these callbacks are for
        if (auto* fx = dynamic_cast<SxEffectBase*> (instance->proc.get()))
        {
            auto a = std::make_shared<SxEffectActions>();
            auto* inst = instance.get();
            a->getBypass = [inst] { return inst->editorBypass.load(); };
            a->setBypass = [inst] (bool b) { inst->editorBypass.store (b); };
            a->getEnabled = [inst] { return ! inst->bypass.load(); };
            a->setEnabled = [this, inst] (bool on)
            {
                if (owner.onSetSlotBypass != nullptr)
                    owner.onSetSlotBypass (inst->ownerTrack, inst->uid, ! on);
            };
            if (fx->kind != BuiltInEffect::Meter && fx->kind != BuiltInEffect::LoudnessMeter
                && fx->kind != BuiltInEffect::StereoField)
            {
                a->sidechainName = [] { return String(); };
                a->showSidechainMenu = [this] (Component& target)
                {
                    if (owner.showSidechainMenu != nullptr)
                        owner.showSidechainMenu (instance->uid, target);
                };
            }
            fx->actions = a;
        }

        // The panel comes up immediately with a themed placeholder; the plug-in's editor is built
        // one or two frames later (createEditorIfNeeded() can block the message thread for
        // hundreds of milliseconds - Kontakt / Ozone build their whole UI synchronously - which
        // froze the whole window and left a black rectangle where the native editor HWND had not
        // painted yet). See createEditor().
        setContentOwned (new PluginLoadingContent (getName()), true);
        setContentComponentSize (kPlaceholderW, kPlaceholderH);
        scheduleEditorCreation();
        setWantsKeyboardFocus (true);
        setBroughtToFrontOnMouseClick (true);
#if JUCE_WINDOWS
        // with no native peer there is no HWND: the JUCE key listener handles the shortcuts
        nativeHandle = getWindowHandle();
        if (nativeHandle != nullptr) PluginKeyRouter::get().addWindow (nativeHandle);
#endif
        lastPowerActive = ! instance->bypass.load();
        lastBypassActive = instance->editorBypass.load();
        startTimerHz (10);          // follow the size and the bypass state
    }

    ~Window() override
    {
        stopTimer();
#if JUCE_WINDOWS
        PluginKeyRouter::get().removeWindow (nativeHandle);
#endif
        removeKeyListener (this);
        clearContentComponent();
    }

    /** Native VSTs sometimes resize their editor HWND after creation. Follow the actual visible
        child rectangle so the panel does not keep a large empty area at the right / bottom. */
    void syncNativeSize()
    {
#if JUCE_WINDOWS
        if (! nativeChild || content == nullptr) return;
        auto* hwnd = getWindowHandle();
        if (hwnd == nullptr) return;
        NativeChildSearch search;
        EnumChildWindows (hwnd, &findLargestChild, &search);
        if (search.best == nullptr || search.area <= 0) return;
        NativeClientRect r {};
        if (GetClientRect (search.best, &r) == 0) return;
        const float approxScale = jlimit (0.5f, 4.0f, Component::getApproximateScaleFactorForComponent (this));
        const auto dpi = GetDpiForWindow (hwnd);
        const float scale = dpi > 0 ? jlimit (0.5f, 4.0f, (float) dpi / 96.0f) : approxScale;
        const int w = (int) std::lround ((r.right - r.left) / (double) scale);
        const int h = (int) std::lround ((r.bottom - r.top) / (double) scale);
        if (w < 80 || h < 40 || userResizing) return;

        // The editor's own size stays authoritative (the measurement is rounded to whole DIPs):
        // when they already agree, only the panel around it can still be wrong. A plug-in that
        // re-scales its UI after opening (Ozone does) resizes itself without touching the host,
        // and the panel used to stay at the larger size it was created with - a wide empty strip
        // to the right of the plug-in's picture.
        if (std::abs (w - content->getWidth()) <= 2 && std::abs (h - content->getHeight()) <= 2)
        {
            fitToContent();
            return;
        }
        setContentComponentSize (w, h);
        if (! userMovedPanel) placeInHost();
        else clampToHost();
#endif
    }

    /** Makes the panel exactly content + title bar. Used when the editor sizes itself (a native
        VST re-scaling for the monitor's DPI, or one that clamps to its own minimum). */
    void fitToContent()
    {
        if (content == nullptr || fittingContent || userResizing) return;
        const int w = content->getWidth(), h = content->getHeight();
        if (w <= 0 || h <= 0) return;
        if (getWidth() == w && getHeight() == h + kTitleBarH) return;

        const ScopedValueSetter<bool> guard (fittingContent, true);
        setSize (w, h + kTitleBarH);
        if (! userMovedPanel) placeInHost();
        else clampToHost();
    }

    void closeButtonPressed() { owner.close (instance->uid); }

    /** The panel paints itself from the theme palette: a repaint is all a theme switch needs. */
    void refreshTheme() { repaint(); }

    static constexpr int kTitleBarH = 28;
    int getTitleBarHeight() const { return kTitleBarH; }

    Component* getContentComponent() const { return content.get(); }

    /** Owned editor / placeholder below the custom title bar (the panel is a plain Component, not
        a TopLevelWindow, so it can be a real WS_CHILD of the host for native VST editors). */
    void setContentOwned (Component* c, bool = true)
    {
        clearContentComponent();
        content.reset (c);
        if (content != nullptr)
        {
            addAndMakeVisible (*content);
            // follow the size the editor decides for itself (see componentMovedOrResized)
            content->addComponentListener (this);
        }
        resized();
    }

    void setContentComponentSize (int w, int h)
    {
        if (content == nullptr) return;
        const ScopedValueSetter<bool> guard (fittingContent, true);
        content->setSize (jmax (1, w), jmax (1, h));
        setSize (jmax (1, w), jmax (1, h) + kTitleBarH);
    }

    /** The editor decided its own size: the panel has to be exactly that, plus the title bar,
        instead of leaving a large empty area around a smaller plug-in picture. */
    void componentMovedOrResized (Component& c, bool wasMoved, bool wasResized) override
    {
        if (&c == content.get() && ! wasMoved && wasResized) fitToContent();
    }

    void setUserResizing (bool busy) { userResizing = busy; }

    void clearContentComponent()
    {
        if (content != nullptr)
        {
            content->removeComponentListener (this);
            removeChildComponent (content.get());
            content.reset();
            // the listeners of the old editor went with it; the panel keeps its own registration
            for (auto it = keyListenerTargets.begin(); it != keyListenerTargets.end();)
                if (*it == this) ++it;
                else             it = keyListenerTargets.erase (it);
        }
    }

    /** Placement inside the host layer (the panel is an ordinary child component now). */
    Rectangle<int> hostArea() const
    {
        auto* h = owner.hostComponent();
        if (h == nullptr) return {};
        // a native child peer uses coordinates relative to the top-level HWND; convert the host
        // layer's local area into that coordinate space (the pluginHost starts below the menus)
        if (nativeChild)
        {
            auto* top = h->getTopLevelComponent();
            const auto offset = top != nullptr ? h->getScreenPosition() - top->getScreenPosition()
                                               : Point<int>();
            return h->getLocalBounds().translated (offset.x, offset.y);
        }
        return h->getLocalBounds();
    }

    void attachNative (void* parentHandle)
    {
        nativeChild = true;
        removeFromDesktop();       // drop any peer created before we knew the native parent
        Component::addToDesktop (0, parentHandle);
#if JUCE_WINDOWS
        if (nativeHandle != nullptr) PluginKeyRouter::get().removeWindow (nativeHandle);
        nativeHandle = getWindowHandle();
        PluginKeyRouter::get().addWindow (nativeHandle);
#endif
    }

    void detachNative()
    {
        nativeChild = false;
#if JUCE_WINDOWS
        if (nativeHandle != nullptr) PluginKeyRouter::get().removeWindow (nativeHandle);
        nativeHandle = nullptr;
#endif
        removeFromDesktop();
    }

    void placeInHost()
    {
        const auto area = hostArea();
        if (area.isEmpty()) return;
        setBounds (area.getCentreX() - getWidth() / 2, area.getCentreY() - getHeight() / 2,
                   getWidth(), getHeight());
        clampToHost();
    }

    void clampToHost()
    {
        if (draggingTitle) return;          // never fight the pointer while the title is being dragged
        const auto area = hostArea();
        if (area.isEmpty()) return;
        const int w = juce::jmin (getWidth(), juce::jmax (kMinW, area.getWidth() - 20));
        const int h = juce::jmin (getHeight(), juce::jmax (kMinH, area.getHeight() - 20));
        const int x = juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - w), getX());
        const int y = juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - h), getY());
        if (w != getWidth() || h != getHeight() || x != getX() || y != getY())
            setBounds (x, y, w, h);
    }

    /** The title bar is the drag handle: it moves the panel inside the host, it never creates a
        native window and clicking the host's empty area does not hide or minimise the panel. */
    void mouseDown (const MouseEvent& e) override
    {
        if (e.y >= getTitleBarHeight()) { toFront (true); return; }
        if (e.mods.isPopupMenu()) return;
        draggingTitle = true;
        dragScreenStart = e.getScreenPosition();
        dragBoundsStart = getBounds();
        dragDelta = {};
        if (! nativeChild)
            setBufferedToImage (true);      // one cached surface: the editor and title move as one
        // keep the current z-order: toFront(true) makes the window active, which can trigger a
        // plug-in editor focus / repaint pass in the middle of a drag
        toFront (false);
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (! draggingTitle) return;
        userMovedPanel = true;
        const auto delta = e.getScreenPosition() - dragScreenStart;
        auto b = dragBoundsStart.translated (delta.x, delta.y);
        const auto area = hostArea();
        b.setX (juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - b.getWidth()), b.getX()));
        b.setY (juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - b.getHeight()), b.getY()));
        dragDelta = { b.getX() - dragBoundsStart.getX(), b.getY() - dragBoundsStart.getY() };
        if (nativeChild)
        {
            // a real child HWND: one SetWindowPos moves the title and the native editor together
            setTopLeftPosition (b.getX(), b.getY());
        }
        else
        {
            // pure JUCE editor: translate the cached surface as one; committing the position only
            // on mouse-up avoids a resize/layout pass for every pointer move
            setTransform (juce::AffineTransform::translation ((float) dragDelta.x, (float) dragDelta.y));
        }
    }

    void mouseUp (const MouseEvent&) override
    {
        if (! draggingTitle) return;
        draggingTitle = false;
        if (nativeChild)
        {
            clampToHost();
        }
        else
        {
            setTransform (juce::AffineTransform());
            setTopLeftPosition (dragBoundsStart.getX() + dragDelta.x,
                                dragBoundsStart.getY() + dragDelta.y);
            clampToHost();
            setBufferedToImage (false);
        }
    }

    /** The editor's own children get the host key listener too, so Space / Ctrl+... typed into a
        plug-in still reach the arrangement even when a knob has the keyboard focus.

        addKeyListener() does not de-duplicate, and this is called again for a few seconds after
        the editor appears (plug-ins can add controls late). Registering the same listener twice
        would run every host shortcut twice - Space toggled the transport on and off again in the
        same key press - so each component is only ever wired up once. */
    void attachHostKeys (juce::Component& c)
    {
        if (keyListenerTargets.insert (&c).second)
            c.addKeyListener (this);
        for (auto* child : c.getChildren()) attachHostKeys (*child);
    }

    void addEditorKeyForwarding()
    {
        if (auto* c = getContentComponent()) attachHostKeys (*c);
        attachHostKeys (*this);
    }

    Component* editorComponent() const { return editorCreated ? getContentComponent() : nullptr; }

    /** Whether the panel counts as open (visible or hidden by the single-panel mode). Closing it
        with the cross clears this; the editor stays cached so reopening does not rebuild it. */
    bool isOpenState() const { return openState; }
    void setOpenState (bool shouldBeOpen) { openState = shouldBeOpen; }
    bool hasEditor() const { return editorCreated; }

    /** Builds the plug-in editor. Deferred out of the constructor by scheduleEditorCreation() so
        the panel chrome is painted first and a slow plug-in UI never freezes the window without
        anything on screen (that was the "black flash" when a plug-in was opened). */
    void createEditor()
    {
        editorRequested = false;
        if (editorCreated || instance == nullptr || instance->proc == nullptr)
            return;
        editorCreated = true;

        AudioProcessorEditor* editor = nullptr;
        try { editor = instance->proc->createEditorIfNeeded(); }
        catch (...) { editor = nullptr; }

        if (editor == nullptr)
        {
            // no editor, or the plug-in threw while building one: keep a readable panel instead
            // of a blank / black one
            setContentOwned (new PluginLoadingContent (instance->name + "  ("
                                                       + TRANS ("no editor available") + ")"), true);
            setContentComponentSize (jmax (kMinW, 320), 90);
            clampToHost();
            addEditorKeyForwarding();
            return;
        }

        // take the editor's own size before the window touches it: setContentOwned sizes the
        // window to the editor, and from then on the editor is the client area (which is the
        // title bar shorter), so asking it afterwards would shrink the plugin by the title bar
        const int wantW = editor->getWidth();
        const int wantH = editor->getHeight();
        // some editors create their own desktop peer; reparenting alone would leave a native
        // window behind that cannot move with our in-host panel
        if (editor->isOnDesktop()) editor->removeFromDesktop();
        setContentOwned (editor, true);
        if (wantW > 0 && wantH > 0)
            setContentComponentSize (wantW, wantH);

        // The window never resizes from its borders: the only handle is the grip in the bottom
        // right corner, and only for plug-ins that say they can be resized.
        if (editor->isResizable())
            addAndMakeVisible (cornerGrip);
        if (getWidth() < kMinW || getHeight() < kMinH) setSize (kFallbackW, kFallbackH);

        addEditorKeyForwarding();
        // the placeholder was sized/centred for its small size: keep the panel centred now that
        // the real editor size is known, unless the user already dragged it somewhere
        if (! userMovedPanel) placeInHost();
        else clampToHost();
        syncNativeSize();
        repaint();
    }

    void scheduleEditorCreation()
    {
        if (editorCreated || editorRequested || instance == nullptr || instance->proc == nullptr)
            return;
        editorRequested = true;
        juce::Component::SafePointer<Window> safe (this);
        // a couple of frames: the panel body (placeholder) has to be on screen before the
        // blocking editor build starts, otherwise the same freeze / black rectangle is back
        Timer::callAfterDelay (kEditorDelayMs, [safe]
        {
            if (auto* w = safe.getComponent()) w->createEditor();
        });
    }

    /** Self test hook: the same action a real click on an icon would run. */
    void triggerIcon (int i)
    {
        i = jlimit (0, 2, i);
        if (i == iconPower)
        {
            if (owner.onSetSlotBypass != nullptr)
                owner.onSetSlotBypass (instance->ownerTrack, instance->uid, ! instance->bypass.load());
            repaint();
        }
        else if (i == iconBypass)
        {
            instance->editorBypass.store (! instance->editorBypass.load());
            repaint();
        }
        else if (owner.showSidechainMenu != nullptr)
            owner.showSidechainMenu (instance->uid, *hits[iconSidechain]);
    }

    //== the embedded title bar icons ------------------------------------------------------------
    Rectangle<int> iconRect (int i) const
    {
        // left end of the title bar: the right end belongs to our close cross
        return { 8 + i * 27, (getTitleBarHeight() - 18) / 2, 24, 18 };
    }

    Rectangle<int> closeRect() const
    {
        return { getWidth() - 26, (getTitleBarHeight() - 18) / 2, 18, 18 };
    }

    bool supportsSidechain() const
    {
        auto* fx = dynamic_cast<SxEffectBase*> (instance->proc.get());
        if (fx == nullptr) return false;
        return fx->kind != BuiltInEffect::Meter && fx->kind != BuiltInEffect::LoudnessMeter
            && fx->kind != BuiltInEffect::StereoField;
    }

    void resized() override
    {
        Component::resized();
        if (content != nullptr)
            content->setBounds (0, kTitleBarH, getWidth(), jmax (1, getHeight() - kTitleBarH));
        if (hits[0] != nullptr)
            for (int i = 0; i < 3; ++i)
                hits[i]->setBounds (iconRect (i));
        if (closeHit != nullptr) closeHit->setBounds (closeRect());
        if (cornerGrip.isVisible())
            cornerGrip.setBounds (getWidth() - 18, getHeight() - 18, 16, 16);
    }

    // The title bar is drawn completely here: background, title text and the three icons, with
    // the icons embedded at the left end (the right end belongs to the minimise / close buttons).
    void paint (Graphics& g) override
    {
        g.fillAll (theme::panel);

        auto bar = getLocalBounds().removeFromTop (jmax (0, getTitleBarHeight()));
        g.setColour (theme::line);
        g.drawHorizontalLine (bar.getBottom() - 1, 0.0f, (float) getWidth());

        const int stripRight = 8 + 3 * 27 + 4;
        g.setColour (theme::text);
        g.setFont (Font (juce::FontOptions (13.0f)).boldened());
        g.drawFittedText (getName(), stripRight, 0, jmax (0, getWidth() - stripRight - 96), bar.getHeight(),
                          Justification::centredLeft, 1, 0.85f);

        for (int i = 0; i < 3; ++i)
        {
            // The power button is green while the plug-in is enabled and grey while it is
            // bypassed: "on" has to read as on at a glance.
            const bool active = i == iconPower ? ! instance->bypass.load()
                              : (i == iconBypass ? instance->editorBypass.load() : false);
            paintHostIcon (g, iconRect (i).toFloat(), i, active,
                           hoverIcon == i || (hits[i] != nullptr && hits[i]->isPressed()),
                           i == iconPower ? Colour (0xff39c5a8)
                                          : (i == iconSidechain ? theme::accent : Colour (0xffe6a23c)),
                           i == iconSidechain && ! supportsSidechain());
        }

        // the close cross (a real child hit target catches the click)
        auto cr = closeRect().toFloat();
        g.setColour ((closeHover ? theme::record.brighter (0.25f) : theme::record).withAlpha (0.9f));
        g.drawLine (cr.getX() + 3.0f, cr.getY() + 3.0f, cr.getRight() - 3.0f, cr.getBottom() - 3.0f, 1.6f);
        g.drawLine (cr.getX() + 3.0f, cr.getBottom() - 3.0f, cr.getRight() - 3.0f, cr.getY() + 3.0f, 1.6f);
    }

    /** The only resize handle: a small grip drawn in the bottom right corner. */
    class CornerResizer : public Component
    {
    public:
        CornerResizer() { setMouseCursor (MouseCursor::BottomRightCornerResizeCursor); }

        void paint (Graphics& g) override
        {
            g.setColour (Colour (0x00000000));
            g.fillAll();
            for (int i = 0; i < 3; ++i)
            {
                const float off = 4.0f + (float) i * 4.0f;
                g.setColour (Colour (0xff9aa4b2).withAlpha (0.85f));
                g.drawLine (4.0f + off, 15.0f, 15.0f, 4.0f + off, 1.2f);
            }
        }

        void mouseDown (const MouseEvent& e) override
        {
            if (auto* w = findParentComponentOfClass<Window>())
            {
                dragWindow = w;
                w->setUserResizing (true);
                startSize = { w->getWidth(), w->getHeight() };
                startPos = e.getScreenPosition();
            }
        }
        void mouseDrag (const MouseEvent& e) override
        {
            if (dragWindow == nullptr) return;
            auto* editor = dragWindow->getContentComponent() != nullptr
                             ? dynamic_cast<AudioProcessorEditor*> (dragWindow->getContentComponent()) : nullptr;
            const auto& delta = e.getScreenPosition() - startPos;
            int w = jmax (120, startSize.x + delta.x);
            int h = jmax (80, startSize.y + delta.y);

            // our own panels are drawn at a fixed aspect: keep the ratio while dragging
            double ratio = 0.0;
            if (editor != nullptr && editor->getAudioProcessor() != nullptr
                && dynamic_cast<SxEffectBase*> (editor->getAudioProcessor()) != nullptr
                && startSize.y > 0)
                ratio = (double) startSize.x / (double) startSize.y;

            if (ratio > 0.0)
            {
                if (std::abs (delta.x) >= std::abs (delta.y)) h = (int) std::lround ((double) w / ratio);
                else                                          w = (int) std::lround ((double) h * ratio);
            }
            if (editor != nullptr)
                if (auto* c = editor->getConstrainer())
                {
                    w = jlimit (c->getMinimumWidth(), c->getMaximumWidth(), w);
                    h = jlimit (c->getMinimumHeight(), c->getMaximumHeight(), h);
                    if (ratio > 0.0)
                        h = (int) std::lround ((double) w / ratio);
                }
            dragWindow->setSize (w, h);
        }
        void mouseUp (const MouseEvent&) override
        {
            if (dragWindow != nullptr) dragWindow->setUserResizing (false);
            dragWindow = nullptr;
        }

    private:
        Window* dragWindow = nullptr;
        Point<int> startSize, startPos;
    };

    PluginWindows& owner;
    std::shared_ptr<PluginInstance> instance;
    std::unique_ptr<Component> content;   // editor / placeholder below the title bar
    std::set<Component*> keyListenerTargets;   // components already given this as a key listener
    CornerResizer cornerGrip;
    IconHitArea* hits[3] {};   // owned by the window (added with addAndMakeVisible)
    IconHitArea* closeHit = nullptr;
    int hoverIcon = -1;
    bool closeHover = false;



private:
    static constexpr int kMinW = 200, kMinH = 120;
    static constexpr int kFallbackW = 820, kFallbackH = 560;
    static constexpr int kPlaceholderW = 360, kPlaceholderH = 120;
    static constexpr int kEditorDelayMs = 40;  // a couple of frames: paint the placeholder first
    static constexpr int kSettleTicks = 60;   // 6 s at 10 Hz
    static constexpr int kNativeSyncEvery = 10;   // after settling, poll native size once a second
    int lastW = 0, lastH = 0, settleTicks = 0;
    int nativeSyncTick = 0;
    bool lastPowerActive = true, lastBypassActive = false;
    bool editorCreated = false;    // createEditorIfNeeded() has run (the editor may be null)
    bool editorRequested = false;  // a deferred createEditor() is queued
    bool openState = true;         // false after the user closed the panel (the editor is cached)
    bool userMovedPanel = false;   // the user dragged the title bar: never re-centre after that
    bool draggingTitle = false;
    bool nativeChild = false;      // a real WS_CHILD peer over the host (native VST editors)
    bool fittingContent = false;   // the panel itself is resizing the editor: ignore its callback
    bool userResizing = false;     // the corner grip is being dragged: never fight it
    Point<int> dragScreenStart;
    Rectangle<int> dragBoundsStart;
    Point<int> dragDelta;
    void* nativeHandle = nullptr;

    // Ctrl+Z / Ctrl+Y work while the plug-in panel (or its editor) has the focus, and the host's
    // own shortcuts (space, Ctrl+S, F-keys, ...) are routed back to MainComponent.
    bool keyPressed (const KeyPress& key, Component* origin) override
    {
        // a text field in the plug-in keeps its own typing / undo
        auto* focused = Component::getCurrentlyFocusedComponent();
        if (dynamic_cast<TextEditor*> (focused) != nullptr) return false;

        if (owner.onGlobalKey != nullptr && owner.onGlobalKey (key)) return true;

        bool redo = false;
        if (! undoRedoKey (key, redo)) return false;
        if (owner.onUndoRedo != nullptr) owner.onUndoRedo (redo);
        return true;
    }

    // some plug-ins only size their editor after the first paint or when their UI is ready:
    // keep the window centred while that happens (about six seconds), so the panel does not open
    // half off-screen. The timer itself keeps running afterwards: it only watches the bypass
    // state then, which costs almost nothing and keeps the power icon truthful.
    void timerCallback() override
    {
        // the deferred editor build may have been dropped (e.g. the message that carried it was
        // cancelled while the window was hidden): retry instead of leaving the placeholder up
        if (! editorCreated && ! editorRequested)
            scheduleEditorCreation();

        // The bypass can also be changed from the slot menu or by undo; keep the power icon
        // (green = enabled) truthful even when the click did not originate in this title bar.
        const bool powerActive = ! instance->bypass.load();
        const bool bypassActive = instance->editorBypass.load();
        if (powerActive != lastPowerActive || bypassActive != lastBypassActive)
        {
            lastPowerActive = powerActive;
            lastBypassActive = bypassActive;
            repaint (getLocalBounds().removeFromTop (jmax (1, getTitleBarHeight())));
        }

        if (editorCreated)
        {
            // Follow the editor's self-resize closely while it settles in; after that a native
            // editor (a heavy Ozone-style UI) only needs its size re-measured once a second, which
            // stops EnumChildWindows from running on every message-loop tick.
            if (settleTicks < kSettleTicks || (++nativeSyncTick % kNativeSyncEvery) == 0)
                syncNativeSize();
        }

        if (settleTicks < kSettleTicks)
        {
            ++settleTicks;
            if (getWidth() != lastW || getHeight() != lastH)
            {
                lastW = getWidth();
                lastH = getHeight();
                // Only keep the panel inside the host - never re-centre it. Moving a panel while
                // the plug-in has one of its own popup menus open dismisses that menu (the
                // "dropdown flashes and closes" problem); a re-centre is only done once, right
                // after the editor is created (see createEditor()).
                clampToHost();
            }
            // a plug-in can add controls after it is opened: keep giving them the host listener
            addEditorKeyForwarding();
        }
    }
};

PluginWindows::PluginWindows (Engine& e) : engine (e)
{
#if JUCE_WINDOWS
    PluginKeyRouter::get().setGlobalKeyCallback ([this] (const KeyPress& k)
    {
        return onGlobalKey != nullptr && onGlobalKey (k);
    });
#endif
    // standalone / self-test host: screen-sized so the old "centred on screen" checks still see
    // a sensible rectangle, but the panels are ordinary child components (no native window)
    auto area = Rectangle<int> (0, 0, 1600, 900);
    if (auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay())
        area = { 0, 0, d->userArea.getWidth(), d->userArea.getHeight() };
    fallbackHost.setBounds (area);
    host = &fallbackHost;
}
PluginWindows::~PluginWindows()
{
#if JUCE_WINDOWS
    PluginKeyRouter::get().setGlobalKeyCallback (nullptr);
#endif
    closeAll();
}

void PluginWindows::setHost (Component* newHost)
{
    host = newHost != nullptr ? newHost : &fallbackHost;
    auto* peer = host->getPeer();
    for (auto& w : windows)
    {
        const bool wasVisible = w->isVisible();
        if (peer != nullptr) w->attachNative (peer->getNativeHandle());
        else
        {
            w->detachNative();
            host->addAndMakeVisible (*w);
        }
        w->setVisible (wasVisible);
        w->placeInHost();
        if (wasVisible) w->toFront (false);
    }
}

Rectangle<int> PluginWindows::hostBounds() const
{
    return host != nullptr ? host->getLocalBounds() : Rectangle<int>();
}

void PluginWindows::hostResized()
{
    for (auto& w : windows) w->clampToHost();
}

void PluginWindows::attachToHost (Window& w)
{
    if (auto* peer = host != nullptr ? host->getPeer() : nullptr)
    {
        w.attachNative (peer->getNativeHandle());
        w.placeInHost();
        w.setVisible (true);
        w.toFront (false);
        return;
    }

    w.detachNative();
    host->addAndMakeVisible (w);
    w.placeInHost();
    w.setVisible (true);
    w.toFront (false);
}

Component* PluginWindows::windowContent (int64 uid) const
{
    for (auto& w : windows) if (w->instance->uid == uid) return w->editorComponent();
    return nullptr;
}


bool PluginWindows::isOpen (int64 uid) const
{
    for (auto& w : windows) if (w->instance->uid == uid) return w->isOpenState();
    return false;
}

bool PluginWindows::isVisible (int64 uid) const
{
    for (auto& w : windows) if (w->instance->uid == uid) return w->isVisible();
    return false;
}

bool PluginWindows::hasVisibleWindow() const
{
    for (auto& w : windows)
        if (w->isVisible()) return true;
    return false;
}

void PluginWindows::hideOthers (int64 keepUid)
{
    for (auto& w : windows)
        w->setVisible (w->instance->uid == keepUid);
}

void PluginWindows::setMultipleWindowsAllowed (bool allowed)
{
    if (multipleWindows == allowed) return;
    multipleWindows = allowed;

    // Turning the mode off immediately leaves only the panel the user looked at last. The others
    // stay alive but hidden, so switching a panel back on (or turning multiple panels on again)
    // does not reload a plug-in editor.
    if (! multipleWindows && lastShownUid != 0)
        hideOthers (lastShownUid);
}

Rectangle<int> PluginWindows::windowBounds (int64 uid) const
{
    for (auto& w : windows) if (w->instance->uid == uid) return w->getBounds();
    return {};
}

void PluginWindows::testTriggerIcon (int64 uid, int icon)
{
    for (auto& w : windows)
        if (w->instance->uid == uid) { w->triggerIcon (icon); return; }
}

Component* PluginWindows::windowComponent (int64 uid) const
{
    for (auto& w : windows) if (w->instance->uid == uid) return w.get();
    return nullptr;
}

void PluginWindows::show (int64 uid, const String& title)
{
    for (auto& w : windows)
        if (w->instance->uid == uid)
        {
            if (! multipleWindows) hideOthers (uid);
            w->setOpenState (true);
            w->setVisible (true);
            w->toFront (true);
            w->scheduleEditorCreation();   // in case the first deferred build never ran
            lastShownUid = uid;
            return;
        }

    auto inst = engine.getInstance (uid);
    if (inst == nullptr || ! inst->proc->hasEditor()) return;

    if (! multipleWindows) hideOthers (uid);
    windows.push_back (std::make_unique<Window> (*this, inst, title.isEmpty() ? inst->name : title));
    attachToHost (*windows.back());
    lastShownUid = uid;
    purgeOldClosedWindows();
}

void PluginWindows::purgeOldClosedWindows()
{
    // Cached editors are what make reopening a heavy plug-in instant; the memory has to stay
    // bounded though, so only the last few CLOSED panels keep their editor alive.
    constexpr int maxClosed = 6;
    int closed = 0;
    for (auto& w : windows) if (! w->isOpenState()) ++closed;
    for (size_t i = 0; i < windows.size() && closed > maxClosed;)
    {
        if (! windows[i]->isOpenState())
        {
            windows.erase (windows.begin() + (std::ptrdiff_t) i);
            --closed;
        }
        else
            ++i;
    }
}

void PluginWindows::showAsync (int64 uid, const String& title)
{
    // The plug-in editor may take a while to build (or even open its own files): let the message
    // loop paint the newly inserted slot first. WeakReference covers the case that the app is
    // closed, or the plug-in removed, before the callback runs.
    WeakReference<PluginWindows> safe (this);
    MessageManager::callAsync ([safe, uid, title]
    {
        if (auto* self = safe.get())
            self->show (uid, title);
    });
}

void PluginWindows::close (int64 uid)
{
    // Hide the panel right away (the click came from inside the window, so it must not be deleted
    // from its own event), then KEEP the window and its editor cached: reopening the same plug-in
    // is what used to rebuild the whole plug-in UI synchronously (the "stutter when opening a
    // plug-in"). The cache is bounded by purgeOldClosedWindows(); removing the slot destroys the
    // window immediately through closeNow().
    for (auto& w : windows)
        if (w->instance->uid == uid)
        {
            w->setOpenState (false);
            w->setVisible (false);
        }
    if (lastShownUid == uid) lastShownUid = 0;

    WeakReference<PluginWindows> safe (this);
    MessageManager::callAsync ([safe]
    {
        if (auto* self = safe.get()) self->purgeOldClosedWindows();
    });
}

void PluginWindows::closeNow (int64 uid)
{
    if (lastShownUid == uid) lastShownUid = 0;
    windows.erase (std::remove_if (windows.begin(), windows.end(),
                                   [uid] (const std::unique_ptr<Window>& w) { return w->instance->uid == uid; }),
                   windows.end());
}

void PluginWindows::closeAll()
{
    lastShownUid = 0;
    windows.clear();
}

void PluginWindows::refreshAll()
{
    for (auto& w : windows)
    {
        w->refreshTheme();
        w->repaint();
    }
}
}
