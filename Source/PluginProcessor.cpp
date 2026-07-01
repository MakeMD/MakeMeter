#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace { const int kOrders[] = { 10, 11, 12, 13 }; } // fftSize choice -> FFT order

MakeMeterProcessor::MakeMeterProcessor()
    : AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    apvts.addParameterListener ("fftSize", this);
    rescanPlugins();
    syncAdvisorFromProfile();
}

bool MakeMeterProcessor::captureCompareA()
{
    if (meter.integratedLufs.load() <= -120.0f) return false; // no signal yet — nothing to snapshot
    compareA = { true, meter.integratedLufs.load(), meter.lra.load(), meter.maxTruePeakDb.load(),
                 meter.widthPct.load(), meter.crestDb.load() };
    meter.spectrum.captureCompare(); // freeze A's tonal balance into its own overlay buffer
    return true;
}

void MakeMeterProcessor::syncAdvisorFromProfile()
{
    auto p = profileTree();
    const auto m = p.getProperty ("aiModel", "").toString();
    const auto e = p.getProperty ("aiEndpoint", "").toString();
    if (m.isNotEmpty()) advisor.model = m;
    if (e.isNotEmpty()) advisor.endpoint = e;
    advisor.replyLang = p.getProperty ("chatLang", "").toString();
}

MakeMeterProcessor::~MakeMeterProcessor()
{
    apvts.removeParameterListener ("fftSize", this);
}

juce::AudioProcessorValueTreeState::ParameterLayout MakeMeterProcessor::makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    // Channel type AND genre are free-form strings in the profile tree (support Add Custom / typing),
    // not APVTS params.
    // ponytail: pre-0.7 projects stored "channel"/"genre" APVTS params — silently dropped on load
    //           (fall back to "Mix Bus"/"EDM"). Acceptable data loss for personal use.
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "fftSize", 1 },
                    "FFT size", StringArray { "1024", "2048", "4096", "8192" }, 1));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "slope", 1 },
                    "Spectrum slope", NormalisableRange<float> (0.0f, 6.0f, 0.1f), 3.0f));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "target", 1 },
                    "Target LUFS", NormalisableRange<float> (-30.0f, -6.0f, 0.1f), -14.0f));
    return layout;
}

void MakeMeterProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentChannels   = getTotalNumInputChannels();
    const int order = kOrders[(int) apvts.getRawParameterValue ("fftSize")->load()];
    meter.prepare (sampleRate, currentChannels, order, samplesPerBlock);

    captureChannels   = juce::jlimit (1, 2, currentChannels);
    captureSampleRate = sampleRate;
    captureLen        = (int) (sampleRate * 15.0); // last 15 s
    captureBuf.setSize (captureChannels, captureLen);
    captureBuf.clear();
    capWritePos = 0;
    captureWritePos.store (0);
}

bool MakeMeterProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void MakeMeterProcessor::parameterChanged (const juce::String& id, float newValue)
{
    if (id == "fftSize")
    {
        // Reallocating FFT buffers races the audio thread — suspend while we swap.
        suspendProcessing (true);
        meter.spectrum.setOrder (kOrders[juce::jlimit (0, 3, (int) newValue)]);
        suspendProcessing (false);
    }
}

void MakeMeterProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    meter.process (buffer); // pure analyser: measure, pass audio through untouched

    // RT-safe rolling capture into the ring (single writer, no allocation).
    if (captureLen > 0)
    {
        const int n  = buffer.getNumSamples();
        const int ch = juce::jmin (captureChannels, buffer.getNumChannels());
        for (int c = 0; c < ch; ++c)
        {
            const float* src = buffer.getReadPointer (c);
            float* dst = captureBuf.getWritePointer (c);
            int p = capWritePos;
            for (int i = 0; i < n; ++i) { dst[p] = src[i]; if (++p >= captureLen) p = 0; }
        }
        capWritePos = (capWritePos + n) % captureLen;
        captureWritePos.store (capWritePos, std::memory_order_relaxed);
    }

    // Seamless whole-track measurement: auto-restart when the DAW transport rewinds to
    // the start (loop-around / play-from-top), so the user never has to press Reset.
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            const bool playing = pos->getIsPlaying();
            const auto ts = pos->getTimeInSamples();
            if (playing && ts.hasValue())
            {
                const juce::int64 t = *ts;
                const juce::int64 nearStart = (juce::int64) buffer.getNumSamples() * 2;
                if (t + 1 < lastPlayTime || (! wasPlaying && t <= nearStart))
                    meter.restartMeasurement();
                lastPlayTime = t;
            }
            wasPlaying = playing;
        }
}

// MESSAGE-THREAD ONLY: reads the profile ValueTree, APVTS params and installedPlugins.
// Snapshotted to a string here so the background analysis thread never touches those.
juce::String MakeMeterProcessor::buildContextText() const
{
    auto pt = apvts.state.getChildWithName ("profile");
    const auto get = [&pt] (const char* k) { return pt.isValid() ? pt.getProperty (k, "").toString() : juce::String(); };
    juce::String s;
    auto add = [&s] (const char* label, juce::String v) { if (v.isNotEmpty()) s << label << v << "\n"; };
    add ("Engineer: ", get ("name"));
    add ("Experience: ", get ("experience"));
    add ("DAWs: ", get ("daws"));
    add ("Monitors: ", get ("monitors"));
    add ("Headphones: ", get ("headphones"));
    add ("Room treatment: ", get ("room"));
    add ("Workflow: ", get ("workflow"));
    add ("Works in genres: ", get ("genres"));

    const juce::String chan  = get ("channel").isNotEmpty() ? get ("channel") : juce::String ("Mix Bus");
    const juce::String genre = get ("genre").isNotEmpty()   ? get ("genre")   : juce::String ("EDM");
    s << "Channel: " << chan << "   Genre: " << genre << "\n"
      << "Target LUFS: " << juce::String (apvts.getRawParameterValue ("target")->load(), 1) << "\n";
    if (! installedPlugins.isEmpty())
        s << "Installed plugins: "
          << installedPlugins.joinIntoString (", ", 0, juce::jmin (60, installedPlugins.size())) << "\n";

    if (compareA.valid) // A/B compare reference the user is looking at
        s << "Compare A snapshot: INT " << juce::String (compareA.lufs, 1) << " LUFS, LRA "
          << juce::String (compareA.lra, 1) << ", max TP " << juce::String (compareA.tp, 1)
          << " dBTP, width " << juce::String (compareA.width, 0) << " %, crest "
          << juce::String (compareA.crest, 1) << " dB (compare the current values against this)\n";
    return s;
}

juce::String MakeMeterProcessor::buildReport (const LoudnessMeter& m, const juce::String& context) const
{
    auto db = [] (float v) { return v <= -120.0f ? juce::String ("-inf") : juce::String (v, 1); };

    // Multi-band tonal balance from the mono spectrum (time-averaged, relative 0..1).
    struct Band { const char* name; double lo, hi; };
    const Band bands[] = {
        { "sub 20-60",     20,    60 }, { "bass 60-120",   60,   120 },
        { "lowbass 120-250", 120, 250 }, { "lowmid 250-500", 250, 500 },
        { "mid 0.5-1k",    500,  1000 }, { "uppermid 1-2k", 1000, 2000 },
        { "presence 2-4k", 2000, 4000 }, { "brilliance 4-8k", 4000, 8000 },
        { "air 8-20k",     8000, 20000 },
    };
    double sum[9] = {}; int cnt[9] = {};
    for (int i = 0; i < SpectrumChannel::numBins; ++i)
    {
        const double hz = m.spectrum.binHz (i);
        const float  v  = m.spectrum.averageScope (i);
        for (int b = 0; b < 9; ++b)
            if (hz >= bands[b].lo && hz < bands[b].hi) { sum[b] += v; ++cnt[b]; }
    }

    juce::String s;
    s << context
      << "Integrated LUFS: " << db (m.integratedLufs.load()) << "\n"
      << "Short-term LUFS: " << db (m.shortTermLufs.load()) << "\n"
      << "Loudness range (LRA): " << juce::String (m.lra.load(), 1) << " LU\n"
      << "Max true peak: " << db (m.maxTruePeakDb.load()) << " dBTP\n"
      << "RMS L/R: " << db (m.rmsLdb.load()) << " / " << db (m.rmsRdb.load()) << " dB\n"
      << "Crest factor: " << juce::String (m.crestDb.load(), 1) << " dB\n"
      << "Stereo correlation: " << juce::String (m.correlation.load(), 2)
      << "   Width: " << juce::String (m.widthPct.load(), 0) << " %\n"
      << "DC offset: " << juce::String (m.dcOffset.load(), 2) << " mV\n"
      << "Banded correlation: sub " << juce::String (m.corrSub.load(), 2)
      << " / mid " << juce::String (m.corrMid.load(), 2)
      << " / top " << juce::String (m.corrTop.load(), 2) << "\n"
      << "Spectral balance (relative 0..1 per band):\n";
    for (int b = 0; b < 9; ++b)
        s << "  " << bands[b].name << ": " << (cnt[b] ? juce::String (sum[b] / cnt[b], 2) : juce::String ("0")) << "\n";

    if (referenceLufs.load() > -120.0f)
        s << "Reference track LUFS: " << db (referenceLufs.load()) << "\n";
    return s;
}

juce::String MakeMeterProcessor::buildSnapshot() const { return buildReport (meter, buildContextText()); }

void MakeMeterProcessor::captureAndAnalyze (std::function<void (juce::String)> done)
{
    const int len = captureLen, ch = captureChannels;
    const double sr = captureSampleRate;
    const int order = meter.spectrum.getOrder();
    if (len == 0) { done (juce::String (juce::CharPointer_UTF8 ("Немає аудіо-потоку для аналізу."))); return; }

    const juce::String context = buildContextText(); // read profile/params on the message thread

    // Downsample the ring to ~120 abs-max buckets for the chat panel's mini waveform (message thread).
    {
        constexpr int kBuckets = 120;
        lastClipPeaks.assign (kBuckets, 0.0f);
        const int wp = captureWritePos.load (std::memory_order_relaxed);
        for (int b = 0; b < kBuckets; ++b)
        {
            const int lo = (int) ((juce::int64) b * len / kBuckets);
            const int hi = (int) ((juce::int64) (b + 1) * len / kBuckets);
            float mx = 0.0f;
            for (int i = lo; i < hi; ++i)
                for (int c = 0; c < ch; ++c)
                    mx = juce::jmax (mx, std::abs (captureBuf.getSample (c, (wp + i) % len)));
            lastClipPeaks[(size_t) b] = juce::jmin (1.0f, mx);
        }
    }

    std::thread ([this, len, ch, sr, order, done, context]
    {
        const int wp = captureWritePos.load (std::memory_order_relaxed);
        juce::AudioBuffer<float> ordered (ch, len);
        for (int c = 0; c < ch; ++c)
        {
            int p = wp;
            for (int i = 0; i < len; ++i) { ordered.setSample (c, i, captureBuf.getSample (c, p)); if (++p >= len) p = 0; }
        }

        LoudnessMeter tmp;
        tmp.prepare (sr, ch, order, 16384);
        const int chunk = 16384;
        for (int pos = 0; pos < len; pos += chunk)
        {
            const int want = juce::jmin (chunk, len - pos);
            juce::AudioBuffer<float> view (ordered.getArrayOfWritePointers(), ch, pos, want);
            tmp.process (view);
            tmp.spectrum.render (0.0f);
        }

        juce::String report = buildReport (tmp, context);
        juce::MessageManager::callAsync ([done, report] { done (report); });
    }).detach();
}

void MakeMeterProcessor::loadReference (const juce::File& file,
                                        std::function<void (juce::String)> done)
{
    std::thread ([this, file, done]
    {
        juce::String msg;
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr)
        {
            juce::MessageManager::callAsync ([done]
                { done (juce::String (juce::CharPointer_UTF8 ("Не вдалося відкрити файл."))); });
            return;
        }

        const double sr = reader->sampleRate;
        const int ch = juce::jlimit (1, 2, (int) reader->numChannels);
        const int chunk = 16384;
        LoudnessMeter tmp;
        tmp.prepare (sr, ch, meter.spectrum.getOrder(), chunk);

        juce::AudioBuffer<float> buf (ch, chunk);
        juce::int64 pos = 0;
        while (pos < reader->lengthInSamples)
        {
            const int want = (int) juce::jmin ((juce::int64) chunk, reader->lengthInSamples - pos);
            reader->read (&buf, 0, want, pos, true, ch > 1);
            juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), ch, want);
            tmp.process (view);
            tmp.spectrum.render (0.0f); // populate scope while feeding
            pos += want;
        }

        const float refL = tmp.integratedLufs.load();
        // Capture the reference curve here, publish it on the message thread (no race
        // with the GUI reading meter.spectrum.reference in paint()).
        std::vector<float> refScope (SpectrumChannel::numBins);
        for (int i = 0; i < SpectrumChannel::numBins; ++i) refScope[(size_t) i] = tmp.spectrum.scope[i];

        msg = file.getFileName() + "  (" + juce::String (refL, 1) + " LUFS)";
        juce::MessageManager::callAsync ([this, done, msg, refL, refScope]
        {
            referenceLufs.store (refL);
            for (int i = 0; i < SpectrumChannel::numBins; ++i)
                meter.spectrum.reference[i] = refScope[(size_t) i];
            meter.spectrum.hasReference = true;
            done (msg);
        });
    }).detach();
}

void MakeMeterProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void MakeMeterProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
        syncAdvisorFromProfile();
        // Refresh an already-open editor so reopened projects show the saved profile.
        juce::MessageManager::callAsync ([this]
        {
            if (auto* ed = dynamic_cast<MakeMeterEditor*> (getActiveEditor()))
                ed->reloadProfile();
        });
    }
}

juce::AudioProcessorEditor* MakeMeterProcessor::createEditor()
{
    return new MakeMeterEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MakeMeterProcessor();
}
