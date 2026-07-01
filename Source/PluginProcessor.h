#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Meters.h"
#include "MixAdvisor.h"
#include "PluginScanner.h"

class MakeMeterProcessor : public juce::AudioProcessor,
                           private juce::AudioProcessorValueTreeState::Listener
{
public:
    MakeMeterProcessor();
    ~MakeMeterProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "MakeMeter"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // --- features used by the editor ---
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    juce::String buildSnapshot() const;                    // live measurements + context -> text
    juce::String buildContextText() const;                 // profile/channel/genre/plugins (MESSAGE thread only)
    juce::String buildReport (const LoudnessMeter&, const juce::String& context) const; // meter -> text (any thread)
    // Analyse the last ~15 s of captured audio on a background thread, then return a rich
    // report for the AI. done() fires on the message thread.
    void captureAndAnalyze (std::function<void (juce::String)> done);
    // Peak buckets (abs-max, 0..1) of the last captured window — filled by captureAndAnalyze
    // on the message thread for the chat panel's mini waveform. Message-thread only, no atomics.
    const std::vector<float>& getLastClipPeaks() const { return lastClipPeaks; }
    void loadReference (const juce::File&,              // analyse a reference track (bg thread)
                        std::function<void (juce::String)> done);
    void rescanPlugins() { installedPlugins = PluginScanner::scanInstalled(); }

    // A/B compare: snapshot the current whole-track metrics + freeze the average spectrum into
    // its own compare overlay. Called from the editor (message thread). Returns false (and does
    // nothing) if there's no signal yet.
    // ponytail: A is transient (per-session) — not persisted across project reloads. Re-capture
    //           after reopening; persisting a stale A would mislead more than help.
    struct CompareSnapshot { bool valid = false; float lufs = 0, lra = 0, tp = 0, width = 0, crest = 0; };
    CompareSnapshot compareA;
    bool captureCompareA();
    void clearCompareA() { compareA.valid = false; meter.spectrum.clearCompare(); }

    juce::AudioProcessorValueTreeState apvts { *this, nullptr, "state", makeLayout() };
    LoudnessMeter meter;
    MixAdvisor advisor;
    juce::StringArray installedPlugins;
    std::atomic<float> referenceLufs { -200.0f };

    // User profile (name, gear, DAWs, AI model…) lives as a child of the APVTS state, so it
    // is persisted by getStateInformation automatically. Edited from the Settings page.
    juce::ValueTree profileTree() { return apvts.state.getOrCreateChildWithName ("profile", nullptr); }
    void syncAdvisorFromProfile();   // push model/endpoint from profile into the advisor

private:
    void parameterChanged (const juce::String& id, float newValue) override;

    double currentSampleRate = 48000.0;
    int currentChannels = 2;

    // Rolling capture of the input stream (RT-safe ring: audio thread writes, bg thread reads).
    juce::AudioBuffer<float> captureBuf;
    std::atomic<int> captureWritePos { 0 };
    int capWritePos = 0, captureLen = 0, captureChannels = 2;
    double captureSampleRate = 48000.0;
    std::vector<float> lastClipPeaks;   // ~120 peak buckets of the last capture (message thread only)

    // Transport tracking for seamless whole-track auto-reset on rewind.
    juce::int64 lastPlayTime = 0;
    bool wasPlaying = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MakeMeterProcessor)
};
