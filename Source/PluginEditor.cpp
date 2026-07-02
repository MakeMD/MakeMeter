#include "PluginEditor.h"

// juce::String(const char*) is ASCII-only; non-ASCII literals need explicit UTF-8.
#define U8(s) juce::String (juce::CharPointer_UTF8 (s))

namespace {
// Surfaces (4 levels: page < panel < panel-highlight, inset below all).
const juce::Colour kBg       (0xff0a0c10);
const juce::Colour kPanelHi  (0xff161a21);
const juce::Colour kPanel    (0xff111419);
const juce::Colour kInset    (0xff07090c);
const juce::Colour kBorder   (0xff222934);
const juce::Colour kBorderHi (0xff2d3744);
// Accent / status.
const juce::Colour kCyan     (0xff35c4e0);
const juce::Colour kCyanDim  (0xff1f7d90);
const juce::Colour kGreen    (0xff5ad17e);
const juce::Colour kOrange   (0xfff39c3d);
const juce::Colour kRed      (0xffe35a5a);
const juce::Colour kTeal     (0xff2bb0c4);
// Text.
const juce::Colour kText     (0xfff0f3f6);
const juce::Colour kTextDim  (0xff9aa3ad);
const juce::Colour kTextMut  (0xff5b636d);
const juce::Colour kGrey = kTextDim; // ponytail: alias so existing kGrey call-sites stay put

struct VizTheme { const char* name; juce::Colour particle, core; };
const VizTheme THEMES[] = {
    { "Aurora",  juce::Colour (0xff35c4e0), juce::Colours::white },
    { "Magma",   juce::Colour (0xfff3683d), juce::Colour (0xffffd27d) },
    { "Ice",     juce::Colour (0xff5aa9ff), juce::Colours::white },
    { "Toxic",   juce::Colour (0xff7ddc52), juce::Colours::white },
    { "Mono",    juce::Colour (0xffcdd3da), juce::Colours::white },
    { "Sunset",  juce::Colour (0xffff6f91), juce::Colour (0xffffc978) },
};
constexpr int kNumThemes = (int) (sizeof (THEMES) / sizeof (THEMES[0]));

const char* SHAPE_NAMES[] = { "Orb", "Rhombus", "Cube", "Nebula" };
constexpr int kNumShapes = (int) (sizeof (SHAPE_NAMES) / sizeof (SHAPE_NAMES[0]));

const char* DAW_NAMES[] = { "Logic Pro", "Ableton Live", "FL Studio", "Pro Tools", "Studio One",
                            "Cubase", "Reaper", "Reason", "Bitwig", "GarageBand", "Other" };

// Smooth curve through points (Catmull-Rom -> cubic beziers) — for the spectrum/EQ curve.
juce::Path catmullRomPath (const std::vector<juce::Point<float>>& pts)
{
    juce::Path p;
    if (pts.empty()) return p;
    p.startNewSubPath (pts[0]);
    for (size_t i = 0; i + 1 < pts.size(); ++i)
    {
        const auto p0 = pts[i > 0 ? i - 1 : i];
        const auto p1 = pts[i];
        const auto p2 = pts[i + 1];
        const auto p3 = pts[i + 2 < pts.size() ? i + 2 : i + 1];
        const auto c1 = p1 + (p2 - p0) * (1.0f / 6.0f);
        const auto c2 = p2 - (p3 - p1) * (1.0f / 6.0f);
        p.cubicTo (c1, c2, p2);
    }
    return p;
}

juce::Font font (float h, bool bold = false)
{
    return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
}

juce::String db1 (float v, const char* suffix = "")
{
    if (v <= -120.0f) return juce::String ("-inf") + suffix;
    return juce::String (v, 1) + suffix;
}

// Uppercase caption label: 11px bold, letter-tracked, kTextDim by default.
void drawCaps (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
               juce::Justification just = juce::Justification::centredLeft,
               juce::Colour col = kTextDim, float height = 11.0f)
{
    auto f = font (height, true).withExtraKerningFactor (0.12f);
    g.setFont (f);
    g.setColour (col);
    g.drawText (text.toUpperCase(), r, just);
}

// Section panel: vertical kPanelHi->kPanel fill, hairline border (radius 8), a 3px cyan
// tick before the letter-tracked title. Returns the inner content rect (padding 14).
juce::Rectangle<int> panel (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title)
{
    const auto rf = r.toFloat();
    g.setGradientFill (juce::ColourGradient (kPanelHi, rf.getTopLeft(), kPanel, rf.getBottomLeft(), false));
    g.fillRoundedRectangle (rf, 8.0f);
    g.setColour (kBorder);
    g.drawRoundedRectangle (rf.reduced (0.5f), 8.0f, 1.0f);
    auto inner = r.reduced (14);
    if (title.isNotEmpty())
    {
        auto head = inner.removeFromTop (16);
        auto tick = head.removeFromLeft (3); // 3px cyan tick
        g.setColour (kCyan);
        g.fillRect (tick.withSizeKeepingCentre (3, 11));
        head.removeFromLeft (6);
        drawCaps (g, title, head, juce::Justification::centredLeft);
        inner.removeFromTop (8);
    }
    return inner;
}
} // namespace

MakeMeterEditor::MakeMeterEditor (MakeMeterProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    // Flat, borderless text buttons for the top bar: no fill, cyan-tinted text on hover.
    auto flatBtn = [] (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
        b.setColour (juce::TextButton::textColourOffId,  kText);
        b.setColour (juce::TextButton::textColourOnId,   kCyan);
        b.setColour (juce::ComboBox::outlineColourId,    juce::Colours::transparentBlack);
    };

    addAndMakeVisible (channelBtn);
    channelBtn.onClick = [this] { showChannelMenu(); }; // text set by loadProfileToUI()
    flatBtn (channelBtn);

    genreBox.addItemList ({ "EDM", "House", "Deep House", "Tech House", "Progressive House", "Techno",
                            "Melodic Techno", "Trance", "Dubstep", "Drum & Bass", "Garage", "Trap",
                            "Hip-Hop", "Lo-Fi", "R&B", "Soul", "Funk", "Pop", "Synthpop", "Indie",
                            "Rock", "Metal", "Punk", "Jazz", "Blues", "Country", "Folk", "Reggae",
                            "Afrobeat", "Latin", "Ambient", "Cinematic", "Classical", "Other" }, 1);
    genreBox.setEditableText (true); // free-form genre allowed
    genreBox.setColour (juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
    genreBox.setColour (juce::ComboBox::outlineColourId,    juce::Colours::transparentBlack);
    genreBox.setColour (juce::ComboBox::textColourId,       kText);
    genreBox.setColour (juce::ComboBox::arrowColourId,      kTextDim);
    addAndMakeVisible (genreBox);
    genreBox.onChange = [this] { proc.profileTree().setProperty ("genre", genreBox.getText(), nullptr); };

    addAndMakeVisible (settingsBtn);
    flatBtn (settingsBtn);
    settingsBtn.onClick = [this]
    {
        settingsOpen = ! settingsOpen;
        settingsBtn.setButtonText (settingsOpen ? "Back" : "Settings");
        setSettingsVisible (settingsOpen);
        resized();
        repaint();
    };

    // Capture = record toggle: start fresh measurement, then analyse the captured window.
    addAndMakeVisible (recBtn);
    flatBtn (recBtn);
    recBtn.onClick = [this]
    {
        capturing = ! capturing;
        if (capturing)
        {
            recBtn.setButtonText ("Stop");
            recBtn.setColour (juce::TextButton::textColourOffId, kRed);
            proc.meter.reset(); // begin a fresh capture window
        }
        else
        {
            recBtn.setButtonText ("Capture");
            recBtn.setColour (juce::TextButton::textColourOffId, kText);
            askAI (U8 ("Проаналізуй ") + channelBtn.getButtonText(), {}, false);
        }
    };

    addAndMakeVisible (compareBtn);
    flatBtn (compareBtn);
    compareBtn.onClick = [this]
    {
        if (! compareMode)
        {
            if (! proc.captureCompareA()) // no signal yet
            {
                addMsg (false, U8 ("Спершу програй матеріал — нема що зафіксувати в A."));
                return;
            }
            compareMode = true;
            prevView = view; view = View::Meters; // deltas + overlay live in the Meters view
        }
        else
        {
            compareMode = false;
            proc.clearCompareA();
            view = prevView;
        }
        compareBtn.setColour (juce::TextButton::textColourOffId, compareMode ? kCyan : kText);
        applyView();
        repaint();
    };

    for (auto* t : { &metersTab, &visTab })
    {
        addAndMakeVisible (t);
        t->setClickingTogglesState (false);
        t->setColour (juce::TextButton::buttonColourId,   juce::Colours::transparentBlack);
        t->setColour (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    }
    metersTab.onClick = [this] { view = View::Meters; applyView(); };
    visTab.onClick    = [this] { view = View::Visualisation; applyView(); };

    // settings controls
    fftBox.addItemList ({ "1024", "2048", "4096", "8192" }, 1);
    addAndMakeVisible (fftBox);
    fftAtt  = std::make_unique<APVTS::ComboBoxAttachment> (proc.apvts, "fftSize", fftBox);
    for (auto* s : { &slopeSlider, &targetSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 18);
        addAndMakeVisible (s);
    }
    slopeAtt  = std::make_unique<APVTS::SliderAttachment> (proc.apvts, "slope", slopeSlider);
    targetAtt = std::make_unique<APVTS::SliderAttachment> (proc.apvts, "target", targetSlider);
    for (auto* b : { &captureBtn, &clearBtn, &loadBtn, &resetBtn }) addAndMakeVisible (b);

    resetBtn.onClick   = [this] { proc.meter.reset(); };
    captureBtn.onClick = [this] { proc.meter.spectrum.captureReference(); };
    clearBtn.onClick   = [this] { proc.meter.spectrum.clearReference(); proc.referenceLufs.store (-200.0f); };
    loadBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Reference track", juce::File(),
                      "*.wav;*.aiff;*.flac;*.mp3;*.ogg");
        juce::Component::SafePointer<MakeMeterEditor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe] (const juce::FileChooser& fc)
            {
                if (safe == nullptr) return;
                const auto f = fc.getResult();
                if (f.existsAsFile())
                {
                    safe->refreshChat (U8 ("аналізую референс…"));
                    safe->proc.loadReference (f, [safe] (juce::String m)
                    {
                        if (safe == nullptr) return;
                        safe->pendingMsg.clear();
                        safe->addMsg (false, U8 ("Референс — ") + m);
                    });
                }
            });
    };

    chatList.msgs = &messages;
    chatList.pending = &pendingMsg;
    chatView.setViewedComponent (&chatList, false);
    chatView.setScrollBarsShown (true, false);
    addAndMakeVisible (chatView);
    addMsg (false, U8 ("Постав канал і жанр угорі. Натисни Capture щоб записати фрагмент, або Send із питанням. Аналіз через локальний Ollama."));
    aiQuestion.setColour (juce::TextEditor::backgroundColourId, kInset);
    aiQuestion.setColour (juce::TextEditor::outlineColourId, kBorder);
    aiQuestion.setColour (juce::TextEditor::textColourId, kText);
    aiQuestion.setTextToShowWhenEmpty (U8 ("Ask about your mix…"), kGrey);
    sendModeBox.addItem ("Whole track", 1);
    sendModeBox.addItem ("Last 15s", 2);
    sendModeBox.setSelectedId (1, juce::dontSendNotification);
    addAndMakeVisible (sendModeBox);
    addAndMakeVisible (aiQuestion);
    addAndMakeVisible (askBtn);
    askBtn.onClick = [this]
    {
        juce::String q = aiQuestion.getText().trim();
        aiQuestion.clear();
        askAI (q.isEmpty() ? U8 ("Оціни мій мікс") : q, q, sendModeBox.getSelectedId() == 2);
    };

    // --- Settings page controls ---
    auto styleEd = [this] (juce::TextEditor& ed, const juce::String& placeholder)
    {
        ed.setColour (juce::TextEditor::backgroundColourId, kInset);
        ed.setColour (juce::TextEditor::outlineColourId, kBorder);
        ed.setColour (juce::TextEditor::textColourId, kText);
        ed.setTextToShowWhenEmpty (placeholder, kGrey);
        addAndMakeVisible (ed);
    };
    styleEd (nameEd, U8 ("Ім'я"));
    styleEd (monitorsEd, "e.g. Yamaha HS8");
    styleEd (headphonesEd, "e.g. AKG K52");
    styleEd (genresEd, "e.g. EDM, Melodic Techno");
    styleEd (roomEd, "e.g. treated, some bass trapping");
    styleEd (workflowEd, "e.g. mix in the box, reference-heavy");
    styleEd (modelEd, "qwen3:14b");
    styleEd (endpointEd, "http://localhost:11434/api/generate");

    nameEd.onTextChange       = [this] { proc.profileTree().setProperty ("name", nameEd.getText(), nullptr); };
    monitorsEd.onTextChange   = [this] { proc.profileTree().setProperty ("monitors", monitorsEd.getText(), nullptr); };
    headphonesEd.onTextChange = [this] { proc.profileTree().setProperty ("headphones", headphonesEd.getText(), nullptr); };
    genresEd.onTextChange     = [this] { proc.profileTree().setProperty ("genres", genresEd.getText(), nullptr); };
    roomEd.onTextChange       = [this] { proc.profileTree().setProperty ("room", roomEd.getText(), nullptr); };
    workflowEd.onTextChange   = [this] { proc.profileTree().setProperty ("workflow", workflowEd.getText(), nullptr); };
    modelEd.onTextChange      = [this] { proc.profileTree().setProperty ("aiModel", modelEd.getText(), nullptr); proc.syncAdvisorFromProfile(); };
    endpointEd.onTextChange   = [this] { proc.profileTree().setProperty ("aiEndpoint", endpointEd.getText(), nullptr); proc.syncAdvisorFromProfile(); };

    experienceBox.addItemList ({ "Beginner", "Intermediate - 1-3 years", "Advanced - 3-10 years", "Expert - 10+ years" }, 1);
    chatLangBox.addItemList ({ "Auto (match input)", "Ukrainian", "English", "Chinese (Mandarin)",
                              "Hindi", "Spanish", "French", "Arabic", "Bengali", "Portuguese",
                              "Urdu", "Indonesian" }, 1); // Russian intentionally excluded
    experienceBox.setSelectedItemIndex (0, juce::dontSendNotification);
    chatLangBox.setSelectedItemIndex (0, juce::dontSendNotification);
    addAndMakeVisible (experienceBox);
    addAndMakeVisible (chatLangBox);
    experienceBox.onChange = [this] { proc.profileTree().setProperty ("experience", experienceBox.getText(), nullptr); };
    chatLangBox.onChange   = [this] { proc.profileTree().setProperty ("chatLang", chatLangBox.getText(), nullptr); };

    static_assert ((int) (sizeof (DAW_NAMES) / sizeof (DAW_NAMES[0])) == kNumDaws,
                   "DAW_NAMES count must match kNumDaws");
    for (int i = 0; i < kNumDaws; ++i)
    {
        auto& b = dawBtns[(size_t) i];
        b.setButtonText (DAW_NAMES[i]);
        b.setClickingTogglesState (true);
        b.onClick = [this] { saveDaws(); };
        addAndMakeVisible (b);
    }

    addAndMakeVisible (viewAllBtn);
    viewAllBtn.onClick = [this]
    {
        addMsg (false, U8 ("Встановлені плагіни — ")
                       + proc.installedPlugins.joinIntoString (", "));
    };

    loadProfileToUI();
    setSettingsVisible (false);

    // First-run nudge: if the profile has no channel/genre yet, ask once via a chat message and
    // mark onboarded so it never repeats. Skipping keeps the silent Mix Bus / EDM defaults.
    {
        auto pt = proc.profileTree();
        const bool hasChannel = pt.hasProperty ("channel");
        const bool hasGenre   = pt.hasProperty ("genre");
        const bool onboarded = (bool) pt.getProperty ("onboarded", false);
        if (! onboarded && ! (hasChannel && hasGenre))
        {
            addMsg (false, U8 ("Вітаю! Обери вгорі тип каналу та жанр — тоді поради будуть точнішими. "
                               "Без вибору використаю Mix Bus / EDM за замовчуванням."));
            pt.setProperty ("onboarded", true, nullptr);
        }
    }

    glViz = std::make_unique<GLVisualiser>();
    addChildComponent (*glViz);   // shown only in the Visualisation view (gated in timerCallback)

    applyView();
    setResizable (true, true);
    setResizeLimits (860, 660, 1600, 1100); // min height fits the Settings form without scrolling
    setSize (1000, 720);
    startTimerHz (30);
}

void MakeMeterEditor::applyView()
{
    const bool m = view == View::Meters;
    metersTab.setColour (juce::TextButton::textColourOffId, m ? kCyan : kGrey);
    visTab.setColour    (juce::TextButton::textColourOffId, m ? kGrey : kCyan);
    repaint();
}

void MakeMeterEditor::timerCallback()
{
    // A new measurement epoch (manual Reset or transport rewind) — re-zero the GUI-owned
    // average spectrum so whole-track analysis matches the audio-thread accumulators.
    const int e = proc.meter.epoch.load (std::memory_order_relaxed);
    if (e != lastEpoch) { lastEpoch = e; proc.meter.spectrum.resetAverage(); }

    // Run the FFT once per tick (GUI thread) so paint() only reads the cached scope.
    proc.meter.spectrum.render (proc.apvts.getRawParameterValue ("slope")->load());

    // Drive the GPU visualiser: keep it shown while the Visualisation view is front so the GL
    // context can initialise, then feed it a snapshot. If it never becomes ready within the grace
    // window, mark GL failed and fall back to the CPU drawScope path (never a black view).
    if (glViz != nullptr)
    {
        const bool wantVis = (view == View::Visualisation && ! settingsOpen);
        if (wantVis && ! glFailed)
        {
            glViz->setActive (true);
            if (glViz->isReady()) glProbeTicks = 0;
            else if (++glProbeTicks > 20) { glFailed = true; glViz->setActive (false); }
        }
        else
        {
            glViz->setActive (false);
            glProbeTicks = 0;   // grace window is per show attempt: hiding detaches the GL context,
                                // so leftover ticks must not accumulate across view toggles
        }

        if (glViz->isReady())
        {
            VizFrame vf;
            const auto& sp = proc.meter.spectrum;
            for (int i = 0; i < 200; ++i) vf.scope[i] = sp.scope[i];
            const float rms = proc.meter.rmsDb.load (std::memory_order_relaxed);
            vf.rmsN        = juce::jlimit (0.0f, 1.0f, (rms + 60.0f) / 60.0f);
            vf.mode        = shapeIndex;
            const auto pc = THEMES[themeIndex].particle;
            const auto cc = THEMES[themeIndex].core;
            vf.colP[0] = pc.getFloatRed(); vf.colP[1] = pc.getFloatGreen(); vf.colP[2] = pc.getFloatBlue();
            vf.colC[0] = cc.getFloatRed(); vf.colC[1] = cc.getFloatGreen(); vf.colC[2] = cc.getFloatBlue();
            vf.bg[0]   = pc.getFloatRed() * 0.02f; vf.bg[1] = pc.getFloatGreen() * 0.025f;
            vf.bg[2]   = pc.getFloatBlue() * 0.03f + 0.008f;   // near-black, faint theme tint
            glViz->pushFrame (vf);
        }
    }

    repaint();
}

void MakeMeterEditor::refreshChat (juce::String pending)
{
    pendingMsg = pending;
    const int w = juce::jmax (1, chatView.getMaximumVisibleWidth());
    chatList.setSize (w, chatList.layoutHeight (w));
    chatList.repaint();
    chatView.setViewPosition (0, juce::jmax (0, chatList.getHeight() - chatView.getHeight())); // auto-scroll to newest
}

void MakeMeterEditor::addMsg (bool user, const juce::String& text, bool hasClip, std::vector<float> wavePeaks)
{
    messages.push_back ({ user, text, hasClip, std::move (wavePeaks) });
    refreshChat();
}

void MakeMeterEditor::askAI (const juce::String& userLine, const juce::String& question, bool fifteenSec)
{
    addMsg (true, userLine);
    const int userIdx = (int) messages.size() - 1; // attach the clip to this message once captured
    refreshChat (U8 ("аналізую…"));
    juce::Component::SafePointer<MakeMeterEditor> safe (this); // reply may arrive after the editor closes
    auto onReply = [safe] (juce::String r)
    {
        if (safe == nullptr) return;
        safe->pendingMsg.clear();
        safe->addMsg (false, r);
    };
    if (fifteenSec)
        proc.captureAndAnalyze ([safe, userIdx, question, onReply] (juce::String snap)
        {
            if (safe == nullptr) return;
            // captureAndAnalyze has just filled getLastClipPeaks() — attach the waveform card.
            if (userIdx >= 0 && userIdx < (int) safe->messages.size())
            {
                safe->messages[(size_t) userIdx].hasClip = true;
                safe->messages[(size_t) userIdx].wavePeaks = safe->proc.getLastClipPeaks();
                safe->refreshChat (safe->pendingMsg);
            }
            safe->proc.advisor.ask (snap, question, [onReply] (juce::String r, bool) { onReply (r); });
        });
    else
        proc.advisor.ask (proc.buildSnapshot(), question, [onReply] (juce::String r, bool) { onReply (r); });
}

void MakeMeterEditor::mouseDown (const juce::MouseEvent& e)
{
    if (view != View::Visualisation || settingsOpen) return;
    const auto p = e.getPosition();
    const auto cycle = [] (int& idx, int n, int d) { idx = (idx + d + n) % n; };
    if      (shapePrevHit.contains (p)) cycle (shapeIndex, kNumShapes, -1);
    else if (shapeNextHit.contains (p)) cycle (shapeIndex, kNumShapes, +1);
    else if (themePrevHit.contains (p)) cycle (themeIndex, kNumThemes, -1);
    else if (themeNextHit.contains (p)) cycle (themeIndex, kNumThemes, +1);
    else return;
    repaint();
}

juce::Rectangle<int> MakeMeterEditor::vizArea (juce::Rectangle<int> main)
{
    main.removeFromBottom (44); // bottom stat strip
    main.removeFromBottom (28); // shape/theme selector row
    return main.reduced (10);   // must match drawVisualisation's scope rect
}

void MakeMeterEditor::resized()
{
    auto r = getLocalBounds();
    auto top  = r.removeFromTop (38);
    auto tabs = r.removeFromBottom (30);
    aiArea    = r.removeFromRight (320);

    // top bar
    auto t = top.reduced (10, 7);
    t.removeFromLeft (170); // logo painted
    channelBtn.setBounds (t.removeFromLeft (120)); t.removeFromLeft (6);
    genreBox.setBounds   (t.removeFromLeft (110));
    settingsBtn.setBounds (t.removeFromRight (78)); t.removeFromRight (8);
    compareBtn.setBounds (t.removeFromRight (80));  t.removeFromRight (8);
    recBtn.setBounds (t.removeFromRight (80));

    // bottom tabs
    auto tb = tabs.reduced (8, 4);
    metersTab.setBounds (tb.removeFromLeft (120)); tb.removeFromLeft (6);
    visTab.setBounds    (tb.removeFromLeft (150));

    mainArea = r;
    if (glViz != nullptr) glViz->setBounds (vizArea (mainArea));
    if (settingsOpen) layoutSettings (mainArea);

    // AI panel components
    auto a = aiArea.reduced (12);
    a.removeFromTop (22); // header painted
    auto qrow = a.removeFromBottom (32);
    askBtn.setBounds (qrow.removeFromRight (62));
    qrow.removeFromRight (6);
    aiQuestion.setBounds (qrow);
    a.removeFromBottom (6);
    sendModeBox.setBounds (a.removeFromBottom (24).removeFromLeft (150));
    a.removeFromBottom (8);
    chatView.setBounds (a);
    refreshChat (pendingMsg); // re-wrap bubbles to the new width
}

void MakeMeterEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    drawTopBar (g, { 0, 0, getWidth(), 38 });
    if (settingsOpen)              drawSettings (g, mainArea);
    else if (view == View::Meters) drawMetersView (g, mainArea);
    else                           drawVisualisation (g, mainArea);
    drawAiPanel (g, aiArea);

    if (! settingsOpen) // bottom tab separator + active-tab underline
    {
        g.setColour (kBorder);
        g.drawHorizontalLine (getHeight() - 30, 0.0f, (float) getWidth());

        const auto& active = view == View::Meters ? metersTab : visTab;
        auto ul = active.getBounds();
        g.setColour (kCyan);
        g.fillRect (ul.getX(), ul.getBottom() - 2, ul.getWidth(), 2); // 2px cyan underline
    }
}

void MakeMeterEditor::setSettingsVisible (bool on)
{
    juce::Component* cs[] = { &nameEd, &monitorsEd, &headphonesEd, &genresEd, &roomEd, &workflowEd,
                             &modelEd, &endpointEd, &experienceBox, &chatLangBox, &viewAllBtn,
                             &fftBox, &slopeSlider, &targetSlider,
                             &captureBtn, &clearBtn, &loadBtn, &resetBtn };
    for (auto* c : cs) c->setVisible (on);
    for (auto& b : dawBtns) b.setVisible (on);
    metersTab.setVisible (! on);
    visTab.setVisible (! on);
}

void MakeMeterEditor::loadProfileToUI()
{
    auto p = proc.profileTree();
    nameEd.setText       (p.getProperty ("name", "").toString(),       juce::dontSendNotification);
    monitorsEd.setText   (p.getProperty ("monitors", "").toString(),   juce::dontSendNotification);
    headphonesEd.setText (p.getProperty ("headphones", "").toString(), juce::dontSendNotification);
    genresEd.setText     (p.getProperty ("genres", "").toString(),     juce::dontSendNotification);
    roomEd.setText       (p.getProperty ("room", "").toString(),       juce::dontSendNotification);
    workflowEd.setText   (p.getProperty ("workflow", "").toString(),   juce::dontSendNotification);
    modelEd.setText      (p.getProperty ("aiModel", "").toString(),    juce::dontSendNotification);
    endpointEd.setText   (p.getProperty ("aiEndpoint", "").toString(), juce::dontSendNotification);
    genreBox.setText     (p.getProperty ("genre", "EDM").toString(),   juce::dontSendNotification);

    const auto selByText = [] (juce::ComboBox& b, const juce::String& t)
    {
        for (int i = 0; i < b.getNumItems(); ++i)
            if (b.getItemText (i) == t) { b.setSelectedItemIndex (i, juce::dontSendNotification); return; }
    };
    selByText (experienceBox, p.getProperty ("experience", "").toString());
    selByText (chatLangBox,   p.getProperty ("chatLang", "").toString());

    const auto daws = p.getProperty ("daws", "").toString();
    for (int i = 0; i < kNumDaws; ++i)
        dawBtns[(size_t) i].setToggleState (daws.contains (DAW_NAMES[i]), juce::dontSendNotification);

    channelBtn.setButtonText (p.getProperty ("channel", "Mix Bus").toString());
}

void MakeMeterEditor::reloadProfile() { loadProfileToUI(); }

void MakeMeterEditor::setChannel (const juce::String& ch)
{
    proc.profileTree().setProperty ("channel", ch, nullptr);
    channelBtn.setButtonText (ch);
}

void MakeMeterEditor::showChannelMenu()
{
    struct Cat { const char* name; juce::StringArray items; };
    const Cat cats[] = {
        { "Vocals",         { "Lead Vocal", "Backing Vocals", "Rap / Spoken", "Vocal Bus" } },
        { "Drums",          { "Kick", "Snare", "Hi-Hats", "Cymbals", "Toms", "Percussion", "Drum Bus" } },
        { "Bass",           { "Bass", "Sub Bass", "808", "Bass Bus" } },
        { "Keys & Guitar",  { "Piano", "Keys", "Acoustic Guitar", "Electric Guitar", "Guitar Bus" } },
        { "Synths",         { "Lead Synth", "Pad", "Pluck", "Arp", "Synth Bus" } },
        { "Strings & Brass",{ "Strings", "Brass", "Orchestra" } },
        { "FX & Other",     { "FX", "Riser", "Impact", "Foley", "Other" } },
        { "Buses",          { "Mix Bus", "Master", "Instrument Bus" } },
    };
    const auto cur = channelBtn.getButtonText();
    juce::Component::SafePointer<MakeMeterEditor> safe (this); // async lambdas may outlive the editor

    juce::PopupMenu menu;
    for (const auto& c : cats)
    {
        juce::PopupMenu sub;
        for (const auto& it : c.items)
            sub.addItem (it, true, it == cur, [safe, it] { if (safe != nullptr) safe->setChannel (it); });
        menu.addSubMenu (c.name, sub);
    }
    menu.addSeparator();
    menu.addItem (U8 ("Свій варіант…"), [safe]
    {
        if (safe == nullptr) return;
        safe->customWin = std::make_unique<juce::AlertWindow> (U8 ("Свій канал"), U8 ("Назва каналу:"),
                                                               juce::MessageBoxIconType::NoIcon);
        safe->customWin->addTextEditor ("n", "");
        safe->customWin->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        safe->customWin->addButton (U8 ("Скасувати"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        safe->customWin->enterModalState (true, juce::ModalCallbackFunction::create ([safe] (int r)
        {
            if (safe == nullptr) return;
            if (r == 1 && safe->customWin != nullptr)
            {
                const auto t = safe->customWin->getTextEditorContents ("n").trim();
                if (t.isNotEmpty()) safe->setChannel (t);
            }
            safe->customWin.reset();
        }), false);
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (channelBtn));
}

void MakeMeterEditor::saveDaws()
{
    juce::StringArray sel;
    for (int i = 0; i < kNumDaws; ++i)
        if (dawBtns[(size_t) i].getToggleState()) sel.add (DAW_NAMES[i]);
    proc.profileTree().setProperty ("daws", sel.joinIntoString (", "), nullptr);
}

void MakeMeterEditor::layoutSettings (juce::Rectangle<int> area)
{
    auto r = area.reduced (16);
    r.removeFromTop (24); // title (painted)

    auto field = [&r] (juce::Component& c) { r.removeFromTop (16); c.setBounds (r.removeFromTop (26)); r.removeFromTop (8); };
    auto pair  = [&r] (juce::Component& a, juce::Component& b)
    {
        r.removeFromTop (16);
        auto row = r.removeFromTop (26);
        const int half = (row.getWidth() - 10) / 2;
        a.setBounds (row.removeFromLeft (half)); row.removeFromLeft (10); b.setBounds (row);
        r.removeFromTop (8);
    };

    field (nameEd);

    // DAW grid (3 columns)
    r.removeFromTop (16);
    { const int cols = 3, gap = 6; const int bw = (r.getWidth() - gap * (cols - 1)) / cols; int i = 0;
      while (i < 11) { auto row = r.removeFromTop (26);
          for (int c = 0; c < cols && i < 11; ++c) { auto cell = row.removeFromLeft (bw); if (c < cols - 1) row.removeFromLeft (gap); dawBtns[(size_t) i++].setBounds (cell); }
          r.removeFromTop (6); } }

    pair (experienceBox, chatLangBox);
    pair (monitorsEd, headphonesEd);
    pair (roomEd, workflowEd);
    field (genresEd);
    pair (modelEd, endpointEd);

    // meter settings row
    r.removeFromTop (16);
    { auto row = r.removeFromTop (26);
      fftBox.setBounds (row.removeFromLeft (84)); row.removeFromLeft (8);
      slopeSlider.setBounds (row.removeFromLeft (juce::jmax (140, row.getWidth() / 2 - 6))); row.removeFromLeft (8);
      targetSlider.setBounds (row); r.removeFromTop (8); }
    { auto row = r.removeFromTop (26);
      captureBtn.setBounds (row.removeFromLeft (84)); row.removeFromLeft (4);
      clearBtn.setBounds (row.removeFromLeft (78));   row.removeFromLeft (4);
      loadBtn.setBounds (row.removeFromLeft (70));    row.removeFromLeft (4);
      resetBtn.setBounds (row.removeFromLeft (60));   row.removeFromLeft (12);
      viewAllBtn.setBounds (row.removeFromLeft (70)); }
}

void MakeMeterEditor::drawSettings (juce::Graphics& g, juce::Rectangle<int> area)
{
    g.setColour (kText);
    g.setFont (font (16.0f, true));
    g.drawText ("Settings", area.reduced (16).removeFromTop (22), juce::Justification::topLeft);

    const auto lbl = [&g] (const char* t, juce::Component& c)
    {
        g.setColour (kGrey); g.setFont (font (10.0f, true));
        g.drawText (juce::String (t).toUpperCase(),
                    c.getX(), c.getY() - 15, c.getWidth(), 13, juce::Justification::bottomLeft);
    };
    lbl ("Your name", nameEd);
    lbl ("DAW(s)", dawBtns[0]);
    lbl ("Experience level", experienceBox);
    lbl ("Chat language", chatLangBox);
    lbl ("Main monitors / speakers", monitorsEd);
    lbl ("Headphones", headphonesEd);
    lbl ("Room treatment", roomEd);
    lbl ("Workflow", workflowEd);
    lbl ("Genres you work with", genresEd);
    lbl ("AI model (Ollama)", modelEd);
    lbl ("Ollama endpoint", endpointEd);
    lbl ("Meter settings", fftBox);
    // plugin count lives in the top bar; here just the "View all" button (no overlapping label)
}

void MakeMeterEditor::drawTopBar (juce::Graphics& g, juce::Rectangle<int> r)
{
    g.setColour (kBg);
    g.fillRect (r);
    g.setColour (kBorder);
    g.drawHorizontalLine (r.getBottom() - 1, 0.0f, (float) getWidth()); // single hairline underneath

    auto a = r.reduced (12, 0);
    g.setColour (kText);
    g.setFont (font (17.0f, true));
    g.drawText ("Make", a.removeFromLeft (50).withTrimmedTop (8), juce::Justification::topLeft);
    g.setColour (kCyan);
    g.drawText ("Meter", a.removeFromLeft (52).withTrimmedTop (8), juce::Justification::topLeft);
   #ifdef JucePlugin_VersionString
    g.setColour (kTextMut);
    g.setFont (font (10.0f, true));
    g.drawText ("v" JucePlugin_VersionString, a.removeFromLeft (56).withTrimmedTop (13),
                juce::Justification::topLeft);
   #endif

    // plugin count sits just left of the Capture/Compare/Settings buttons, never over them
    drawCaps (g, juce::String (proc.installedPlugins.size()) + " plugins",
              { recBtn.getX() - 160, r.getY() + 12, 150, 16 },
              juce::Justification::topRight, kTextMut, 10.0f);
}

void MakeMeterEditor::drawMetersView (juce::Graphics& g, juce::Rectangle<int> r)
{
    r.reduce (8, 8);
    if (compareMode && proc.compareA.valid) { drawCompareBar (g, r.removeFromTop (44)); r.removeFromTop (8); }
    drawLoudnessPanel (g, r.removeFromTop (104));
    r.removeFromTop (8);
    auto mid = r.removeFromTop (juce::jmax (170, r.getHeight() * 45 / 100));
    drawLevelsPanel (g, mid.removeFromLeft (mid.getWidth() / 2));
    mid.removeFromLeft (8);
    drawStereoPanel (g, mid);
    r.removeFromTop (8);
    drawSpectrumPanel (g, r);
}

void MakeMeterEditor::drawCompareBar (juce::Graphics& g, juce::Rectangle<int> r)
{
    g.setColour (kPanel);
    g.fillRoundedRectangle (r.toFloat(), 8.0f);
    g.setColour (kCyan.withAlpha (0.5f));
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 8.0f, 1.0f);

    auto area = r.reduced (8, 4);
    drawCaps (g, U8 ("vs A"), area.removeFromLeft (44), juce::Justification::centredLeft, kCyan);

    const auto& a = proc.compareA;
    const auto& m = proc.meter;
    struct Row { const char* label; float aVal, cur; const char* unit; };
    const Row rows[] = {
        { "INT",   a.lufs,  m.integratedLufs.load(), "" },
        { "LRA",   a.lra,   m.lra.load(),            "" },
        { "TP",    a.tp,    m.maxTruePeakDb.load(),  "" },
        { "WIDTH", a.width, m.widthPct.load(),       "%" },
        { "CREST", a.crest, m.crestDb.load(),        "" },
    };
    const int n = (int) (sizeof (rows) / sizeof (rows[0]));
    const int cw = area.getWidth() / n;
    for (const auto& row : rows)
    {
        auto c = area.removeFromLeft (cw);
        drawCaps (g, row.label, c.removeFromTop (13), juce::Justification::centred, kTextMut, 9.0f);
        const float d = row.cur - row.aVal;
        juce::String line = db1 (row.aVal) + juce::String (juce::CharPointer_UTF8 (" \xe2\x86\x92 ")) + db1 (row.cur);
        g.setColour (kText); g.setFont (font (10.0f));
        g.drawText (line, c.removeFromTop (14), juce::Justification::centred);
        // Neutral colour: encodes "changed", not good/bad (higher isn't universally better).
        g.setColour (std::abs (d) < 0.05f ? kGrey : kCyan);
        g.setFont (font (10.0f, true));
        g.drawText ((d >= 0 ? "+" : "") + juce::String (d, 1) + row.unit, c, juce::Justification::centred);
    }
}

void MakeMeterEditor::drawLoudnessPanel (juce::Graphics& g, juce::Rectangle<int> r)
{
    auto in = panel (g, r, "Loudness - EBU R128");
    const float target = proc.apvts.getRawParameterValue ("target")->load();
    const auto& m = proc.meter;

    struct Cell { const char* label; float val; const char* unit; juce::Colour col; };
    const float integ = m.integratedLufs.load();
    // Integrated: green only when it lands within +/-1 LU of target, otherwise neutral text.
    const juce::Colour ic = (integ > -120.0f && std::abs (integ - target) <= 1.0f) ? kGreen : kText;

    const Cell cells[] = {
        { "Momentary",  m.momentaryLufs.load(), "LUFS", kText },
        { "Short Term", m.shortTermLufs.load(), "LUFS", kText },
        { "Integrated", integ,                  "LUFS", ic },
        { "LRA",        m.lra.load(),           "LU",   kText },
    };
    const int cw = in.getWidth() / 4;
    for (int i = 0; i < 4; ++i)
    {
        auto c = in.removeFromLeft (cw);
        drawCaps (g, cells[i].label, c.removeFromTop (16), juce::Justification::centredTop);
        g.setColour (cells[i].col);
        g.setFont (font (34.0f, true));
        const bool lra = i == 3;
        juce::String txt = (lra) ? juce::String (cells[i].val, 1)
                                 : (cells[i].val <= -120.0f ? "-inf" : juce::String (cells[i].val, 1));
        g.drawText (txt, c.removeFromTop (40), juce::Justification::centred);
        drawCaps (g, cells[i].unit, c.removeFromTop (14), juce::Justification::centredTop, kTextMut, 9.0f);
    }
}

void MakeMeterEditor::drawLevelsPanel (juce::Graphics& g, juce::Rectangle<int> r)
{
    auto in = panel (g, r, "Levels");
    const auto& m = proc.meter;

    auto bar = [&] (const char* label, float dbv, juce::Colour col)
    {
        auto row = in.removeFromTop (18);
        in.removeFromTop (3);
        drawCaps (g, label, row.removeFromLeft (42), juce::Justification::centredLeft, kTextDim, 10.0f);
        auto valBox = row.removeFromRight (54);
        auto track = row.reduced (4, 4).toFloat();
        const float pr = track.getHeight() * 0.5f; // pill radius = height/2
        g.setColour (kInset);
        g.fillRoundedRectangle (track, pr);
        const float norm = juce::jlimit (0.0f, 1.0f, (dbv + 60.0f) / 60.0f);
        auto fill = track.withWidth (juce::jmax (track.getHeight(), track.getWidth() * norm));
        g.setColour (col);
        g.fillRoundedRectangle (fill, pr);
        // inner top highlight for a glossy pill
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.fillRoundedRectangle (fill.reduced (1.5f).withHeight (fill.getHeight() * 0.4f), pr);
        g.setColour (kText); g.setFont (font (11.0f));
        g.drawText (db1 (dbv, " dB"), valBox, juce::Justification::centredRight);
    };

    bar ("RMS L",  m.rmsLdb.load(),  kGreen);
    bar ("RMS R",  m.rmsRdb.load(),  kGreen);
    bar ("PEAK L", m.peakLdb.load(), kOrange);
    bar ("PEAK R", m.peakRdb.load(), kOrange);
    bar ("TP L",   m.tpLdb.load(),   kOrange);
    bar ("TP R",   m.tpRdb.load(),   kOrange);

    in.removeFromTop (8);
    auto boxes = in.removeFromTop (52);
    auto box = [&] (juce::Rectangle<int> b, const char* label, juce::String val, juce::Colour col)
    {
        g.setColour (kInset);
        g.fillRoundedRectangle (b.toFloat(), 3.0f);
        g.setColour (kBorder);
        g.drawRoundedRectangle (b.toFloat().reduced (0.5f), 3.0f, 1.0f);
        drawCaps (g, label, b.removeFromTop (16), juce::Justification::centred, kTextDim, 9.0f);
        g.setColour (col); g.setFont (font (20.0f, true));
        g.drawText (val, b, juce::Justification::centred);
    };
    box (boxes.removeFromLeft (boxes.getWidth() / 2).reduced (2), "CREST",
         juce::String (m.crestDb.load(), 1), kOrange);
    box (boxes.reduced (2), "DC OFFSET", juce::String (m.dcOffset.load(), 2), kTextDim);
}

void MakeMeterEditor::drawStereoPanel (juce::Graphics& g, juce::Rectangle<int> r)
{
    auto in = panel (g, r, "Stereo Image");
    const auto& m = proc.meter;

    // WIDTH
    {
        auto row = in.removeFromTop (18); in.removeFromTop (4);
        drawCaps (g, "WIDTH", row.removeFromLeft (44), juce::Justification::centredLeft, kTextDim, 10.0f);
        auto valBox = row.removeFromRight (52);
        auto track = row.reduced (4, 5).toFloat();
        const float pr = track.getHeight() * 0.5f;
        g.setColour (kInset); g.fillRoundedRectangle (track, pr);
        const float w = juce::jlimit (0.0f, 1.0f, m.widthPct.load() / 100.0f);
        auto fill = track.withWidth (juce::jmax (track.getHeight(), track.getWidth() * w));
        g.setColour (kCyan);
        g.fillRoundedRectangle (fill, pr);
        g.setColour (juce::Colours::white.withAlpha (0.14f));
        g.fillRoundedRectangle (fill.reduced (1.5f).withHeight (fill.getHeight() * 0.4f), pr);
        g.setColour (kText); g.setFont (font (11.0f));
        g.drawText (juce::String (m.widthPct.load(), 1) + " %", valBox, juce::Justification::centredRight);
    }
    // CORR (-1..+1)
    {
        auto row = in.removeFromTop (18); in.removeFromTop (4);
        drawCaps (g, "CORR", row.removeFromLeft (44), juce::Justification::centredLeft, kTextDim, 10.0f);
        auto valBox = row.removeFromRight (52);
        auto track = row.reduced (4, 5).toFloat();
        const float pr = track.getHeight() * 0.5f;
        g.setColour (kInset); g.fillRoundedRectangle (track, pr);
        const float corr = juce::jlimit (-1.0f, 1.0f, m.correlation.load());
        const float mid = track.getCentreX();
        juce::Rectangle<float> fill (mid, track.getY(),
                                     corr * track.getWidth() * 0.5f, track.getHeight());
        g.setColour (corr >= 0 ? kGreen : kRed);
        g.fillRoundedRectangle (fill.withX (juce::jmin (mid, fill.getRight())).withWidth (std::abs (fill.getWidth())), pr);
        g.setColour (kText); g.setFont (font (11.0f));
        g.drawText (juce::String (corr, 2), valBox, juce::Justification::centredRight);
    }
    // banded correlation (sub / mid / top)
    {
        auto row = in.removeFromTop (15); in.removeFromTop (2);
        drawCaps (g, "CORR b", row.removeFromLeft (44), juce::Justification::centredLeft, kTextDim, 9.0f);
        g.setColour (kText); g.setFont (font (10.0f));
        g.drawText ("sub " + juce::String (m.corrSub.load(), 2)
                  + "  mid " + juce::String (m.corrMid.load(), 2)
                  + "  top " + juce::String (m.corrTop.load(), 2),
                    row, juce::Justification::centredRight);
    }
    in.removeFromTop (6);
    drawScope (g, in, false);
}

void MakeMeterEditor::drawScope (juce::Graphics& g, juce::Rectangle<int> area, bool orb)
{
    const auto c = area.toFloat().getCentre();
    const float s = juce::jmin (area.getWidth(), area.getHeight()) * 0.46f;

    const auto& th = THEMES[themeIndex]; // theme-tinted for every shape

    // Subtle RMS pulse for the Visualisation shapes only (the goniometer is a calibrated display,
    // so it stays 1:1). -60..0 dB -> 0.9..1.12x. Cheap, paint-only.
    const float rmsN = juce::jlimit (0.0f, 1.0f, (proc.meter.rmsDb.load() + 60.0f) / 60.0f);
    const float pulse = orb ? (0.9f + rmsN * 0.22f) : 1.0f;

    // The goniometer (orb=false) always uses the classic dot cloud (shape 0). The Visualisation
    // view (orb=true) picks one of four shapes from the SAME goniometer points via shapeIndex.
    const int shp = orb ? shapeIndex : 0;
    const int N = LoudnessMeter::goniN;
    const auto tint = th.particle;

    if (! orb) // goniometer axes
    {
        g.setColour (kBorderHi);
        g.drawLine (c.x - s, c.y, c.x + s, c.y, 1.0f);
        g.drawLine (c.x, c.y - s, c.x, c.y + s, 1.0f);
        g.setColour (kBorder);
        g.drawLine (c.x - s * 0.7f, c.y - s * 0.7f, c.x + s * 0.7f, c.y + s * 0.7f, 0.5f);
        g.drawLine (c.x - s * 0.7f, c.y + s * 0.7f, c.x + s * 0.7f, c.y - s * 0.7f, 0.5f);
    }

    if (shp == 1) // Ring: remap points onto a ring, radius modulated by magnitude, smooth closed curve
    {
        const float rBase = s * 0.72f * pulse;
        std::vector<juce::Point<float>> pts; pts.reserve ((size_t) N + 3);
        for (int i = 0; i < N; ++i)
        {
            const float gx = proc.meter.goniX[i].load (std::memory_order_relaxed);
            const float gy = proc.meter.goniY[i].load (std::memory_order_relaxed);
            const float mag = juce::jmin (1.0f, std::sqrt (gx * gx + gy * gy));
            const float ang = juce::MathConstants<float>::twoPi * (i / (float) N);
            const float rad = rBase * (0.55f + mag * 0.6f);
            pts.push_back ({ c.x + std::cos (ang) * rad, c.y + std::sin (ang) * rad });
        }
        // close smoothly by wrapping the first two points
        pts.push_back (pts[0]); pts.push_back (pts[1]); pts.push_back (pts[2]);
        const auto ring = catmullRomPath (pts);
        g.setColour (tint.withAlpha (0.10f));
        g.fillPath (ring);
        g.setColour (tint.withAlpha (0.9f));
        g.strokePath (ring, juce::PathStrokeType (1.6f));
        return;
    }

    if (shp == 2) // Helix: two sine "DNA" ribbons across the width, amplitude modulated by points
    {
        const float w = s * 1.9f, amp = s * 0.6f * pulse;
        for (int ribbon = 0; ribbon < 2; ++ribbon)
        {
            std::vector<juce::Point<float>> pts; pts.reserve ((size_t) N);
            const float phase = ribbon * juce::MathConstants<float>::pi;
            for (int i = 0; i < N; ++i)
            {
                const float t = i / (float) (N - 1);
                const float gy = proc.meter.goniY[i].load (std::memory_order_relaxed);
                const float x = c.x - w * 0.5f + w * t;
                const float y = c.y + std::sin (t * juce::MathConstants<float>::twoPi * 3.0f + phase)
                                        * amp * (0.4f + 0.6f * std::abs (gy));
                pts.push_back ({ x, y });
            }
            g.setColour ((ribbon == 0 ? tint : th.core).withAlpha (0.85f));
            g.strokePath (catmullRomPath (pts), juce::PathStrokeType (1.6f));
        }
        return;
    }

    if (shp == 3) // Nebula: diffuse cloud, dot size/alpha from magnitude
    {
        for (int i = 0; i < N; ++i)
        {
            const float gx = proc.meter.goniX[i].load (std::memory_order_relaxed);
            const float gy = proc.meter.goniY[i].load (std::memory_order_relaxed);
            const float mag = juce::jmin (1.0f, std::sqrt (gx * gx + gy * gy));
            const float x = c.x + gx * s * pulse;
            const float y = c.y - gy * s * pulse;
            const float dr = 1.2f + mag * 4.0f; // bigger dots for louder samples
            g.setColour (tint.withAlpha (0.05f + mag * 0.30f));
            g.fillEllipse (x - dr, y - dr, dr * 2, dr * 2);
        }
        return;
    }

    // shape 0 (Orb) + goniometer: layered dot cloud. Denser (more opaque) for the goniometer.
    const float halo = orb ? 2.4f : 1.5f;
    const float coreR = orb ? 1.0f : 0.7f;
    const auto haloCol = tint.withAlpha (orb ? 0.16f : 0.28f);
    const auto coreCol = th.core.withAlpha (orb ? 0.35f : 0.85f);
    for (int i = 0; i < N; ++i)
    {
        const float gx = proc.meter.goniX[i].load (std::memory_order_relaxed);
        const float gy = proc.meter.goniY[i].load (std::memory_order_relaxed);
        const float x = c.x + gx * s * pulse;
        const float y = c.y - gy * s * pulse;
        g.setColour (haloCol);
        g.fillEllipse (x - halo, y - halo, halo * 2, halo * 2);
        g.setColour (coreCol);
        g.fillEllipse (x - coreR, y - coreR, coreR * 2, coreR * 2);
    }

    // Small centre bloom: a radial tint pooled at the origin where mono content stacks up.
    const float bloom = s * 0.22f;
    g.setGradientFill (juce::ColourGradient (tint.withAlpha (orb ? 0.22f : 0.30f), c.x, c.y,
                                             tint.withAlpha (0.0f), c.x + bloom, c.y, true));
    g.fillEllipse (c.x - bloom, c.y - bloom, bloom * 2, bloom * 2);
}

void MakeMeterEditor::drawSpectrumPanel (juce::Graphics& g, juce::Rectangle<int> r)
{
    auto in = panel (g, r, "Spectrum");
    const auto sf = in.toFloat();
    const int N = SpectrumChannel::numBins;
    const auto& M = proc.meter.spectrum; // render() runs in timerCallback

    const auto pointsOf = [&] (const float* data)
    {
        std::vector<juce::Point<float>> v; v.reserve ((size_t) N);
        for (int i = 0; i < N; ++i)
            v.push_back ({ sf.getX() + sf.getWidth() * ((float) i / (N - 1)),
                           sf.getBottom() - data[i] * sf.getHeight() });
        return v;
    };

    // faint horizontal dB gridlines behind the curve (every ~1/4 of the panel height)
    g.setColour (kBorder.withAlpha (0.5f));
    for (int i = 1; i < 4; ++i)
    {
        const float y = sf.getY() + sf.getHeight() * (i / 4.0f);
        g.drawHorizontalLine ((int) y, sf.getX(), sf.getRight());
    }

    // main spectrum: smooth Catmull-Rom curve, vertical teal gradient fill under it
    juce::Path top = catmullRomPath (pointsOf (M.scope));
    juce::Path fill = top;
    fill.lineTo (sf.getRight(), sf.getBottom());
    fill.lineTo (sf.getX(),     sf.getBottom());
    fill.closeSubPath();
    g.setGradientFill (juce::ColourGradient (kTeal.withAlpha (0.32f), sf.getX(), sf.getY(),
                                             kTeal.withAlpha (0.02f), sf.getX(), sf.getBottom(), false));
    g.fillPath (fill);
    g.setColour (kCyan);
    g.strokePath (top, juce::PathStrokeType (1.4f));

    // Dashed overlay helper (reference = gold, compare A = cyan) with a small legend label.
    auto overlay = [&] (const float* data, juce::Colour col, const char* label, int legendRow)
    {
        const float dashes[] = { 5.0f, 4.0f };
        juce::Path dashed;
        juce::PathStrokeType (1.4f).createDashedStroke (dashed, catmullRomPath (pointsOf (data)), dashes, 2);
        g.setColour (col);
        g.fillPath (dashed);
        g.setFont (font (10.0f, true));
        g.drawText (label, in.getRight() - 96, in.getY() + 2 + legendRow * 14, 92, 12,
                    juce::Justification::topRight);
    };
    if (M.hasReference) overlay (M.reference,  juce::Colour (0xfff5d76e), "-- reference", 0);
    if (M.hasCompare)   overlay (M.compareRef, kCyan,                      "-- A", M.hasReference ? 1 : 0);

    // frequency grid labels
    g.setColour (kGrey); g.setFont (font (9.0f));
    const double minHz = 20.0, maxHz = 20000.0;
    const int marks[] = { 50, 100, 250, 500, 1000, 2000, 5000, 10000, 20000 };
    for (int hz : marks)
    {
        const float prop = (float) (std::log (hz / minHz) / std::log (maxHz / minHz));
        const float x = sf.getX() + sf.getWidth() * prop;
        g.drawText (hz >= 1000 ? juce::String (hz / 1000) + "k" : juce::String (hz),
                    juce::Rectangle<float> (x - 16, sf.getBottom() - 12, 32, 12), juce::Justification::centred);
    }
}

void MakeMeterEditor::drawVisualisation (juce::Graphics& g, juce::Rectangle<int> r)
{
    auto strip = r.removeFromBottom (44);
    auto selRow = r.removeFromBottom (28);
    if (! glReady()) drawScope (g, r.reduced (10), true);   // GL child covers this rect when ready

    // Two "‹ label ›" arrow-group selectors (shape | theme). Hit-rects stored for mouseDown.
    auto selector = [&] (juce::Rectangle<int> box, const juce::String& label,
                         juce::Rectangle<int>& prevHit, juce::Rectangle<int>& nextHit)
    {
        prevHit = box.removeFromLeft (22);
        nextHit = box.removeFromRight (22);
        g.setColour (kCyan); g.setFont (font (15.0f, true));
        g.drawText (U8 ("\xe2\x80\xb9"), prevHit, juce::Justification::centred);
        g.drawText (U8 ("\xe2\x80\xba"), nextHit, juce::Justification::centred);
        drawCaps (g, label, box, juce::Justification::centred, kText, 11.0f);
    };
    const int gw = juce::jmin (150, selRow.getWidth() / 2 - 12);
    auto cluster = selRow.withSizeKeepingCentre (gw * 2 + 24, selRow.getHeight());
    selector (cluster.removeFromLeft (gw),  SHAPE_NAMES[shapeIndex],       shapePrevHit, shapeNextHit);
    cluster.removeFromLeft (24);
    selector (cluster,                       THEMES[themeIndex].name,       themePrevHit, themeNextHit);

    drawBottomStrip (g, strip);
}

void MakeMeterEditor::drawBottomStrip (juce::Graphics& g, juce::Rectangle<int> r)
{
    g.setColour (kPanel);
    g.fillRect (r);
    g.setColour (kBorder);
    g.drawHorizontalLine (r.getY(), 0.0f, (float) getWidth());

    const auto& m = proc.meter;
    struct Cell { const char* label; juce::String val; };
    const Cell cells[] = {
        { "INT",   db1 (m.integratedLufs.load()) },
        { "ST",    db1 (m.shortTermLufs.load()) },
        { "MOM",   db1 (m.momentaryLufs.load()) },
        { "LRA",   juce::String (m.lra.load(), 1) },
        { "RMS",   db1 (m.rmsDb.load()) },
        { "TP L",  db1 (m.tpLdb.load()) },
        { "TP R",  db1 (m.tpRdb.load()) },
        { "CORR",  juce::String (m.correlation.load(), 2) },
        { "WIDTH", juce::String (m.widthPct.load(), 0) },
        { "CREST", juce::String (m.crestDb.load(), 1) },
    };
    const int n = (int) (sizeof (cells) / sizeof (cells[0]));
    const int cw = r.getWidth() / n;
    auto a = r;
    for (int i = 0; i < n; ++i)
    {
        auto c = a.removeFromLeft (cw);
        drawCaps (g, cells[i].label, c.removeFromTop (16), juce::Justification::centredBottom, kTextMut, 9.0f);
        g.setColour (kCyan); g.setFont (font (13.0f, true));
        g.drawText (cells[i].val, c, juce::Justification::centredTop);
    }
}

// --- Chat bubble geometry (shared by ChatList::layoutHeight and ::paint) ---
namespace {
constexpr int kChatPad   = 10;  // list margin
constexpr int kMsgGap    = 10;  // vertical gap between messages
constexpr int kAvatar    = 24;  // avatar diameter
constexpr int kAvGap     = 8;   // avatar<->bubble gap
constexpr int kBubblePad = 10;  // text inset inside a bubble
constexpr int kWaveH     = 46;  // waveform card height
constexpr float kBubbleWidthFrac = 0.78f;

// Wrapped text layout for one bubble body at a fixed text width.
juce::TextLayout bubbleText (const juce::String& text, float textW)
{
    juce::AttributedString as;
    as.append (text, font (13.0f), kText);
    juce::TextLayout tl;
    tl.createLayout (as, textW);
    return tl;
}

// Height a single message occupies (bubble body + optional waveform card).
int msgHeight (const juce::String& text, bool clipCard, int listW)
{
    const int maxBubble = (int) ((listW - kChatPad * 2 - kAvatar - kAvGap) * kBubbleWidthFrac);
    const float textW = (float) juce::jmax (20, maxBubble - kBubblePad * 2);
    int h = kBubblePad * 2 + (int) std::ceil (bubbleText (text, textW).getHeight());
    if (clipCard) h += kBubblePad + kWaveH;
    return h;
}
} // namespace

int MakeMeterEditor::ChatList::layoutHeight (int width) const
{
    int y = kChatPad;
    for (const auto& m : *msgs) y += msgHeight (m.text, m.user && m.hasClip, width) + kMsgGap;
    if (pending != nullptr && pending->isNotEmpty())
        y += msgHeight (*pending, false, width) + kMsgGap;
    return y - kMsgGap + kChatPad; // trim trailing gap, add bottom margin
}

void MakeMeterEditor::ChatList::paint (juce::Graphics& g)
{
    g.fillAll (kInset);
    const int listW = getWidth();
    const int maxBubble = (int) ((listW - kChatPad * 2 - kAvatar - kAvGap) * kBubbleWidthFrac);
    const float textW = (float) juce::jmax (20, maxBubble - kBubblePad * 2);

    auto drawOne = [&] (const ChatMsg& m, int y) -> int
    {
        auto tl = bubbleText (m.text, textW);
        const int bodyH = kBubblePad * 2 + (int) std::ceil (tl.getHeight());
        int bubbleH = bodyH;
        if (m.user && m.hasClip) bubbleH += kBubblePad + kWaveH;

        // Bubble body width = text width + padding, capped at maxBubble.
        int bw = juce::jmin (maxBubble, (int) std::ceil (tl.getWidth()) + kBubblePad * 2);
        if (m.user && m.hasClip) bw = maxBubble; // waveform wants the full width
        bw = juce::jmax (bw, kBubblePad * 2 + 40);

        juce::Rectangle<int> avatar, bubble;
        if (m.user) // right-aligned
        {
            avatar = { listW - kChatPad - kAvatar, y, kAvatar, kAvatar };
            bubble = { avatar.getX() - kAvGap - bw, y, bw, bubbleH };
        }
        else        // left-aligned
        {
            avatar = { kChatPad, y, kAvatar, kAvatar };
            bubble = { avatar.getRight() + kAvGap, y, bw, bubbleH };
        }

        // Avatar disc.
        g.setColour (m.user ? kPanelHi : kCyanDim);
        g.fillEllipse (avatar.toFloat());
        g.setColour (kText); g.setFont (font (10.0f, true));
        g.drawText (m.user ? U8 ("Ти") : juce::String ("M"), avatar, juce::Justification::centred);

        // Bubble background: cyan tint for user, panel for assistant.
        g.setColour (m.user ? kCyan.withAlpha (0.16f) : kPanel);
        g.fillRoundedRectangle (bubble.toFloat(), 8.0f);

        // Text body.
        auto body = bubble.withHeight (bodyH).reduced (kBubblePad);
        tl.draw (g, body.toFloat());

        // Waveform card (user clips only).
        if (m.user && m.hasClip)
        {
            auto card = bubble.withTop (bubble.getY() + bodyH).reduced (kBubblePad, 0)
                              .withHeight (kWaveH);
            g.setColour (kInset);
            g.fillRoundedRectangle (card.toFloat(), 6.0f);

            // Play button: cyan circle + triangle. // ponytail: play stub, wire to transport later
            auto play = card.removeFromLeft (kWaveH).reduced (10).toFloat();
            g.setColour (kCyan);
            g.fillEllipse (play);
            juce::Path tri;
            const auto pc = play.getCentre();
            const float tr = play.getWidth() * 0.24f;
            tri.addTriangle (pc.x - tr * 0.7f, pc.y - tr,
                             pc.x - tr * 0.7f, pc.y + tr,
                             pc.x + tr,        pc.y);
            g.setColour (kInset);
            g.fillPath (tri);

            // Mirrored vertical bars around the card centre line.
            const auto& peaks = m.wavePeaks;
            if (! peaks.empty())
            {
                auto bars = card.reduced (6, 6).toFloat();
                const float cy = bars.getCentreY();
                const float half = bars.getHeight() * 0.5f;
                const int n = (int) peaks.size();
                const float step = bars.getWidth() / (float) n;
                const float bwd = juce::jmax (1.0f, step * 0.6f);
                g.setColour (kCyan.withAlpha (0.75f));
                for (int i = 0; i < n; ++i)
                {
                    const float hbar = juce::jlimit (0.0f, 1.0f, peaks[(size_t) i]) * half;
                    const float x = bars.getX() + i * step;
                    g.fillRect (x, cy - hbar, bwd, hbar * 2.0f); // mirror top+bottom
                }
            }
        }
        return bubbleH;
    };

    int y = kChatPad;
    for (const auto& m : *msgs) y += drawOne (m, y) + kMsgGap;
    if (pending != nullptr && pending->isNotEmpty())
    {
        ChatMsg p { false, *pending };
        drawOne (p, y);
    }
}

void MakeMeterEditor::drawAiPanel (juce::Graphics& g, juce::Rectangle<int> r)
{
    g.setColour (kPanel);
    g.fillRect (r);
    g.setColour (kBorder);
    g.drawVerticalLine (r.getX(), 0.0f, (float) getHeight());

    auto h = r.reduced (12).removeFromTop (18);
    auto tick = h.removeFromLeft (3);
    g.setColour (kCyan);
    g.fillRect (tick.withSizeKeepingCentre (3, 11));
    h.removeFromLeft (6);
    drawCaps (g, "AI ASSISTANT", h, juce::Justification::centredLeft, kCyan);
}
