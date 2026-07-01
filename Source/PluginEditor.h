#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

class MakeMeterEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit MakeMeterEditor (MakeMeterProcessor&);
    ~MakeMeterEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void reloadProfile();   // re-pull Settings fields from the processor (after state load)

private:
    void timerCallback() override;
    void applyView();
    void refreshChat (juce::String pending = {});   // rebuild bubbles; pending = transient assistant line
    void addMsg (bool user, const juce::String& text,
                 bool hasClip = false, std::vector<float> wavePeaks = {});
    void askAI (const juce::String& userLine, const juce::String& question, bool fifteenSec);

    void drawTopBar          (juce::Graphics&, juce::Rectangle<int>);
    void drawMetersView      (juce::Graphics&, juce::Rectangle<int>);
    void drawCompareBar      (juce::Graphics&, juce::Rectangle<int>);
    void drawVisualisation   (juce::Graphics&, juce::Rectangle<int>);
    void drawLoudnessPanel   (juce::Graphics&, juce::Rectangle<int>);
    void drawLevelsPanel     (juce::Graphics&, juce::Rectangle<int>);
    void drawStereoPanel     (juce::Graphics&, juce::Rectangle<int>);
    void drawSpectrumPanel   (juce::Graphics&, juce::Rectangle<int>);
    void drawScope           (juce::Graphics&, juce::Rectangle<int>, bool orb);
    void drawBottomStrip     (juce::Graphics&, juce::Rectangle<int>);
    void drawAiPanel         (juce::Graphics&, juce::Rectangle<int>);
    void drawSettings        (juce::Graphics&, juce::Rectangle<int>);
    void layoutSettings      (juce::Rectangle<int>);
    void setSettingsVisible  (bool);
    void loadProfileToUI();
    void saveDaws();
    void showChannelMenu();
    void setChannel (const juce::String&);

    MakeMeterProcessor& proc;
    using APVTS = juce::AudioProcessorValueTreeState;

    enum class View { Meters, Visualisation };
    View view = View::Meters;
    View prevView = View::Meters;   // restored when Compare turns off
    bool settingsOpen = false;

    juce::TextButton channelBtn;                  // opens hierarchical channel-type menu
    juce::ComboBox genreBox;                       // editable (preset list + free text)
    std::unique_ptr<juce::AlertWindow> customWin; // "Add Custom…" channel name dialog
    juce::TextButton settingsBtn { "Settings" };
    juce::TextButton recBtn { "Capture" };           // record toggle
    juce::TextButton compareBtn { "Compare" };       // A/B snapshot toggle
    juce::TextButton metersTab { "METERS" }, visTab { "VISUALISATION" };
    bool capturing = false;
    bool compareMode = false;
    int themeIndex = 0;
    int shapeIndex = 0; // 0 Orb, 1 Ring, 2 Helix, 3 Nebula

    // Visualisation bottom-row selector hit-rects ("‹ Shape ›" / "‹ Theme ›"), set in drawVisualisation.
    juce::Rectangle<int> shapePrevHit, shapeNextHit, themePrevHit, themeNextHit;

    // Chat transcript as scrollable bubbles (replaces the old plain-string TextEditor log).
    struct ChatMsg { bool user; juce::String text; bool hasClip = false; std::vector<float> wavePeaks; };
    std::vector<ChatMsg> messages;
    juce::String pendingMsg; // transient assistant line ("аналізую…") shown while a reply is in flight

    // Inner component that lays out the bubbles and reports its own height so the Viewport scrolls.
    struct ChatList : juce::Component
    {
        std::vector<ChatMsg>* msgs = nullptr;
        juce::String* pending = nullptr;
        void paint (juce::Graphics&) override;
        int layoutHeight (int width) const; // total content height for a given width
    };
    ChatList chatList;
    juce::Viewport chatView;

    juce::ComboBox fftBox;
    juce::Slider slopeSlider, targetSlider;
    juce::TextButton captureBtn { "Freeze ref" }, clearBtn { "Clear ref" },
                     loadBtn { "Load ref" }, resetBtn { "Reset" };
    std::unique_ptr<APVTS::ComboBoxAttachment> fftAtt;
    std::unique_ptr<APVTS::SliderAttachment> slopeAtt, targetAtt;

    juce::TextEditor aiQuestion;
    juce::TextButton askBtn { "Send" };
    juce::ComboBox sendModeBox; // "Whole track" / "Last 15s"
    std::unique_ptr<juce::FileChooser> chooser;

    // Settings page (full-screen profile form) — fields persist into the processor's profile tree.
    juce::TextEditor nameEd, monitorsEd, headphonesEd, genresEd, modelEd, endpointEd, roomEd, workflowEd;
    juce::ComboBox experienceBox, chatLangBox;
    static constexpr int kNumDaws = 11;
    std::array<juce::TextButton, kNumDaws> dawBtns;
    juce::TextButton viewAllBtn { "View all" };

    juce::Rectangle<int> mainArea, aiArea;
    int lastEpoch = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MakeMeterEditor)
};
