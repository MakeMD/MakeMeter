#pragma once
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <atomic>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <array>

// One log-frequency FFT trace. Audio thread fills a fifo (pushSample); GUI thread
// runs the FFT and reads `scope` (render()). One frame is double-buffered via `ready`.
// FFT size is runtime-settable (setup) — change it only while audio is suspended.
class SpectrumChannel
{
public:
    static constexpr int numBins = 200;          // display points (log-spaced)

    void setup (double sr, int fftOrder)
    {
        sampleRate = sr;
        order   = juce::jlimit (10, 13, fftOrder); // 1024 .. 8192
        fftSize = 1 << order;
        fft     = std::make_unique<juce::dsp::FFT> (order);
        window  = std::make_unique<juce::dsp::WindowingFunction<float>> (
                      (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann);
        fifo.assign ((size_t) fftSize, 0.0f);
        fftData.assign ((size_t) fftSize * 2, 0.0f);
        fifoIndex = 0;
        ready.store (false);
        std::fill (scope, scope + numBins, 0.0f);
        resetAverage();
    }

    void pushSample (float s) noexcept           // audio thread
    {
        if (fifoIndex == fftSize)
        {
            if (! ready.load())
            {
                std::copy (fifo.begin(), fifo.end(), fftData.begin());
                std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
                ready.store (true);
            }
            fifoIndex = 0;
        }
        fifo[(size_t) fifoIndex++] = s;
    }

    bool render (float slopeDbPerOct)            // GUI thread; true if scope changed
    {
        if (! ready.load()) return false;

        window->multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
        fft->performFrequencyOnlyForwardTransform (fftData.data());

        const double minHz = 20.0, maxHz = juce::jmin (20000.0, sampleRate * 0.5);
        for (int i = 0; i < numBins; ++i)
        {
            const double prop = (double) i / (numBins - 1);
            const double hz   = minHz * std::pow (maxHz / minHz, prop);
            // Interpolate between adjacent FFT bins. A log display over a linear FFT otherwise lands
            // many low-freq points on the SAME bin -> flat runs + jumps (a stair-stepped curve).
            // ponytail: linear lerp fixes the log-over-linear stair-stepping; high freq still samples
            //           one bin per point (fine for a display) - add band-averaging if it matters.
            //           Low end (20-50 Hz) rides only 1-2 real bins (~sr/fftSize per bin), so any
            //           residual ramp there is the FFT resolution floor - needs a bigger/multi-res
            //           transform, not more interpolation.
            const double fbin = hz * fftSize / sampleRate;
            const int    b0   = juce::jlimit (1, fftSize / 2 - 2, (int) fbin);
            const float  frac = (float) juce::jlimit (0.0, 1.0, fbin - (double) b0);
            const float  mag  = juce::jmap (frac, fftData[(size_t) b0], fftData[(size_t) b0 + 1]);
            float db = juce::Decibels::gainToDecibels (mag / (float) fftSize, -100.0f);
            db += slopeDbPerOct * (float) std::log2 (hz / minHz);       // spectral tilt
            const float norm = juce::jlimit (0.0f, 1.0f, (db + 100.0f) / 100.0f);
            scope[i] = juce::jmax (norm, scope[i] * 0.82f);             // peak-hold decay (display)
            avgAccum[i] += norm;                                        // long-term average (analysis)
        }
        avgCount += 1.0;
        ready.store (false);
        return true;
    }

    // Time-averaged spectrum since the last resetAverage() — whole-track tonal balance.
    float averageScope (int i) const { return avgCount > 0 ? (float) (avgAccum[i] / avgCount) : scope[i]; }
    void  resetAverage()             { std::fill (avgAccum, avgAccum + numBins, 0.0); avgCount = 0.0; }

    double binHz (int i) const
    {
        const double minHz = 20.0, maxHz = juce::jmin (20000.0, sampleRate * 0.5);
        return minHz * std::pow (maxHz / minHz, (double) i / (numBins - 1));
    }

    int  getOrder() const     { return order; }
    void setOrder (int o)     { setup (sampleRate, o); }

    void captureReference() { std::copy (scope, scope + numBins, reference); hasReference = true; }
    void clearReference()   { hasReference = false; }

    // A/B compare overlay — its OWN buffer so it never clobbers the reference (Freeze/Load ref).
    void captureCompare() { for (int i = 0; i < numBins; ++i) compareRef[i] = averageScope (i); hasCompare = true; }
    void clearCompare()   { hasCompare = false; }

    float scope[numBins]     = {};
    float reference[numBins] = {};
    bool  hasReference = false;
    float compareRef[numBins] = {};
    bool  hasCompare = false;
    double avgAccum[numBins] = {};
    double avgCount = 0.0;

private:
    double sampleRate = 48000.0;
    int order = 11, fftSize = 2048, fifoIndex = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window;
    std::vector<float> fifo, fftData;
    std::atomic<bool> ready { false };
};

// K-weighted loudness per ITU-R BS.1770-4 plus true-peak, RMS, crest, stereo correlation,
// width, DC offset, LRA and a goniometer feed. Real-time safe: no allocation/locks in
// process() (scratch buffers and ring buffers are pre-sized in prepare()).
//
// ponytail: K-weighting uses JUCE's RBJ shelf+highpass tuned to the BS.1770 corner
//           frequencies (matches a reference meter to ~0.22 dB across 40 Hz–16 kHz,
//           verified by MakeMeterCalib). kCalibLU trims the 1 kHz anchor to exactly -23.
class LoudnessMeter
{
public:
    void prepare (double sr, int numChannels, int fftOrder, int maxBlock)
    {
        sampleRate = sr;
        channels   = juce::jlimit (1, 2, numChannels);
        maxBlockSamples = juce::jmax (512, maxBlock);

        juce::dsp::ProcessSpec spec { sr, (juce::uint32) maxBlockSamples, (juce::uint32) channels };

        *shelf.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf (
                            sr, 1681.97f, 0.7071f, juce::Decibels::decibelsToGain (3.999f));
        *hpf.state   = *juce::dsp::IIR::Coefficients<float>::makeHighPass (
                            sr, 38.13f, 0.5003f);
        shelf.prepare (spec);
        hpf.prepare (spec);

        // banded-correlation crossovers (single-channel filters)
        juce::dsp::ProcessSpec mspec { sr, (juce::uint32) maxBlockSamples, 1 };
        auto lp120 = juce::dsp::IIR::Coefficients<float>::makeLowPass  (sr, 120.0);
        auto hp120 = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 120.0);
        auto lp5k  = juce::dsp::IIR::Coefficients<float>::makeLowPass  (sr, 5000.0);
        auto hp5k  = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 5000.0);
        for (auto* f : { &bSubL, &bSubR })     { f->coefficients = lp120; f->prepare (mspec); }
        for (auto* f : { &bTopL, &bTopR })     { f->coefficients = hp5k;  f->prepare (mspec); }
        for (auto* f : { &bMidHpL, &bMidHpR }) { f->coefficients = hp120; f->prepare (mspec); }
        for (auto* f : { &bMidLpL, &bMidLpR }) { f->coefficients = lp5k;  f->prepare (mspec); }

        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            (size_t) channels, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR);
        oversampler->initProcessing ((size_t) maxBlockSamples);

        tpScratch.setSize (channels, maxBlockSamples);
        kbScratch.setSize (channels, maxBlockSamples);

        spectrum.setup (sr, fftOrder);
        subBlockLen = juce::jmax (1, (int) std::lround (sr * 0.1)); // 100 ms gating step

        integratedZ.reserve (kIntegratedCap);
        lraScratch.reserve (kLraCap);
        lraLufs.reserve (kLraCap);
        reset();
    }

    void reset()
    {
        subAccum.assign ((size_t) channels, 0.0);
        subCount = 0;
        histCount = histPos = 0;
        integratedZ.clear();
        lraCount = lraPos = lraTick = 0;
        smPeak = smRms = smXY = smXX = smYY = 0.0;
        smRmsLv = smRmsRv = smSideMS = 0.0;
        smPeakLv = smPeakRv = smPeakMono = smTpLv = smTpRv = 0.0;
        smDcL = smDcR = 0.0;
        smXYs = smXXs = smYYs = smXYm = smXXm = smYYm = smXYt = smXXt = smYYt = 0.0;
        for (auto* f : { &bSubL, &bSubR, &bTopL, &bTopR, &bMidHpL, &bMidHpR, &bMidLpL, &bMidLpR }) f->reset();
        corrSub = corrMid = corrTop = 1.0f;
        maxTruePeakDb = -200.0f;

        momentaryLufs = shortTermLufs = integratedLufs = -200.0f;
        truePeakDb = rmsDb = -200.0f;
        crestDb = 0.0f; correlation = 0.0f; lra = 0.0f;
        rmsLdb = rmsRdb = peakLdb = peakRdb = tpLdb = tpRdb = -200.0f;
        widthPct = 0.0f; dcOffset = 0.0f;

        for (int i = 0; i < goniN; ++i) { goniX[i].store (0.0f); goniY[i].store (0.0f); }
        goniPos = goniDecim = 0;
        spectrum.resetAverage(); // long-term average restarts with the measurement
        epoch.fetch_add (1, std::memory_order_relaxed);
    }

    // RT-safe restart of the whole-track accumulation — call from the audio thread on
    // transport rewind. Does NOT touch the GUI-owned average spectrum; the editor resets
    // that when it observes `epoch` change (so the two threads each reset their own data).
    void restartMeasurement()
    {
        std::fill (subAccum.begin(), subAccum.end(), 0.0);
        subCount = 0;
        histCount = histPos = 0;
        integratedZ.clear();                 // keeps capacity — no allocation
        lraCount = lraPos = lraTick = 0;
        maxTruePeakDb = -200.0f;
        integratedLufs = -200.0f; lra = 0.0f;
        epoch.fetch_add (1, std::memory_order_relaxed);
    }

    void process (const juce::AudioBuffer<float>& buffer)
    {
        if (buffer.getNumSamples() > maxBlockSamples)   // slice big blocks (oversampler cap)
        {
            const int total = buffer.getNumSamples();
            auto** ptrs = const_cast<float**> (buffer.getArrayOfReadPointers());
            for (int off = 0; off < total; off += maxBlockSamples)
                process (juce::AudioBuffer<float> (ptrs, buffer.getNumChannels(),
                                                   off, juce::jmin (maxBlockSamples, total - off)));
            return;
        }

        const int n  = buffer.getNumSamples();
        const int ch = juce::jmin (channels, buffer.getNumChannels());
        if (n == 0 || ch == 0) return;

        // --- true peak (oversampled), no per-block allocation ---
        tpScratch.setSize (channels, n, false, false, true); // avoidReallocating: no malloc
        for (int c = 0; c < channels; ++c)
            if (c < ch) tpScratch.copyFrom (c, 0, buffer, c, 0, n);
            else        tpScratch.clear (c, 0, n);
        juce::dsp::AudioBlock<float> block (tpScratch);
        auto up = oversampler->processSamplesUp (block);
        float peak = 0.0f, tpL = 0.0f, tpR = 0.0f;
        for (size_t c = 0; c < up.getNumChannels(); ++c)
        {
            const float* d = up.getChannelPointer (c);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
            {
                const float a = std::abs (d[i]);
                peak = juce::jmax (peak, a);
                if (c == 0) tpL = juce::jmax (tpL, a); else if (c == 1) tpR = juce::jmax (tpR, a);
            }
        }
        if (ch == 1) tpR = tpL;
        const double dec = std::pow (peakDecay, (double) n); // block-rate decay == per-sample rate
        smPeak = juce::jmax (smPeak * dec, (double) peak);
        smTpLv = juce::jmax (smTpLv * dec, (double) tpL);
        smTpRv = juce::jmax (smTpRv * dec, (double) tpR);
        truePeakDb = (float) juce::Decibels::gainToDecibels (smPeak, -200.0);
        tpLdb = (float) juce::Decibels::gainToDecibels (smTpLv, -200.0);
        tpRdb = (float) juce::Decibels::gainToDecibels (smTpRv, -200.0);
        maxTruePeakDb = juce::jmax (maxTruePeakDb.load(),
                                    (float) juce::Decibels::gainToDecibels ((double) peak, -200.0));

        // --- per-sample: RMS/peak, correlation, width, DC, goniometer, spectrum ---
        for (int i = 0; i < n; ++i)
        {
            const double l = buffer.getSample (0, i);
            const double r = ch > 1 ? buffer.getSample (1, i) : l;
            const double mono = 0.5 * (l + r);
            const double side = 0.5 * (l - r);
            spectrum.pushSample ((float) mono);

            smRms    = smRms    * smooth + (1.0 - smooth) * (mono * mono);
            smRmsLv  = smRmsLv  * smooth + (1.0 - smooth) * (l * l);
            smRmsRv  = smRmsRv  * smooth + (1.0 - smooth) * (r * r);
            smSideMS = smSideMS * smooth + (1.0 - smooth) * (side * side);
            smXY = smXY * smooth + (1.0 - smooth) * (l * r);
            smXX = smXX * smooth + (1.0 - smooth) * (l * l);
            smYY = smYY * smooth + (1.0 - smooth) * (r * r);
            smDcL = smDcL * 0.99995 + 0.00005 * l;
            smDcR = smDcR * 0.99995 + 0.00005 * r;

            // banded correlation: split L/R into sub (<120) / mid (120-5k) / top (>5k)
            const float fl = (float) l, fr = (float) r;
            const double sl = bSubL.processSample (fl),  sr2 = bSubR.processSample (fr);
            const double tl = bTopL.processSample (fl),  tr  = bTopR.processSample (fr);
            const double ml = bMidLpL.processSample (bMidHpL.processSample (fl));
            const double mr = bMidLpR.processSample (bMidHpR.processSample (fr));
            accumBand (smXYs, smXXs, smYYs, sl, sr2);
            accumBand (smXYm, smXXm, smYYm, ml, mr);
            accumBand (smXYt, smXXt, smYYt, tl, tr);

            smPeakLv   = juce::jmax (smPeakLv   * peakDecay, std::abs (l));
            smPeakRv   = juce::jmax (smPeakRv   * peakDecay, std::abs (r));
            smPeakMono = juce::jmax (smPeakMono * peakDecay, std::abs (mono));

            if (++goniDecim >= 3)
            {
                goniDecim = 0;
                goniX[goniPos].store ((float) (-side * 1.41421356), std::memory_order_relaxed); // L→upper-left
                goniY[goniPos].store ((float) ( mono * 1.41421356), std::memory_order_relaxed);
                goniPos = (goniPos + 1) % goniN;
            }
        }
        rmsDb  = (float) juce::Decibels::gainToDecibels (std::sqrt (smRms), -200.0);
        rmsLdb = (float) juce::Decibels::gainToDecibels (std::sqrt (smRmsLv), -200.0);
        rmsRdb = (float) juce::Decibels::gainToDecibels (std::sqrt (smRmsRv), -200.0);
        peakLdb = (float) juce::Decibels::gainToDecibels (smPeakLv, -200.0);
        peakRdb = (float) juce::Decibels::gainToDecibels (smPeakRv, -200.0);
        const float peakMonoDb = (float) juce::Decibels::gainToDecibels (smPeakMono, -200.0);
        crestDb = juce::jlimit (0.0f, 60.0f, peakMonoDb - rmsDb); // mono peak vs mono RMS
        const double denom = std::sqrt (smXX * smYY);
        correlation = denom > 1e-12 ? (float) juce::jlimit (-1.0, 1.0, smXY / denom) : 1.0f;
        const double midR = std::sqrt (smRms), sideR = std::sqrt (smSideMS); // smRms already tracks mono mean-square
        widthPct = (float) juce::jlimit (0.0, 200.0, 100.0 * sideR / (midR + 1e-9));
        dcOffset = (float) (juce::jmax (std::abs (smDcL), std::abs (smDcR)) * 1000.0); // larger channel, ~mV
        corrSub = corrFrom (smXYs, smXXs, smYYs);
        corrMid = corrFrom (smXYm, smXXm, smYYm);
        corrTop = corrFrom (smXYt, smXXt, smYYt);

        // --- K-weighting for loudness (scratch, no allocation) ---
        kbScratch.setSize (channels, n, false, false, true);
        for (int c = 0; c < channels; ++c)
            if (c < ch) kbScratch.copyFrom (c, 0, buffer, c, 0, n);
            else        kbScratch.clear (c, 0, n);
        juce::dsp::AudioBlock<float> kblock (kbScratch);
        juce::dsp::ProcessContextReplacing<float> ctx (kblock);
        shelf.process (ctx);
        hpf.process (ctx);

        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < ch; ++c)
            {
                const double s = kbScratch.getSample (c, i);
                subAccum[(size_t) c] += s * s;
            }
            if (++subCount >= subBlockLen)
            {
                double z = 0.0;
                for (int c = 0; c < ch; ++c) z += subAccum[(size_t) c] / subCount;
                histRing[(size_t) histPos] = z;
                histPos = (histPos + 1) % 30;
                histCount = juce::jmin (30, histCount + 1);
                if (integratedZ.size() < integratedZ.capacity()) integratedZ.push_back (z); // never reallocs
                std::fill (subAccum.begin(), subAccum.end(), 0.0);
                subCount = 0;
                updateLoudness();
            }
        }
    }

    // ponytail: kCalibLU trims +0.25 LU so the EBU anchor (stereo 1 kHz @ -23 dBFS) reads
    //           exactly -23.0 — compensates the RBJ K-weighting gain error at 1 kHz.
    static constexpr double kCalibLU = 0.25;
    static float lufsFromZ (double meanZ)
    {
        return meanZ > 1e-12 ? (float) (-0.691 + kCalibLU + 10.0 * std::log10 (meanZ)) : -200.0f;
    }

    std::atomic<float> momentaryLufs { -200.0f }, shortTermLufs { -200.0f }, integratedLufs { -200.0f };
    std::atomic<float> truePeakDb { -200.0f }, maxTruePeakDb { -200.0f };
    std::atomic<float> rmsDb { -200.0f }, crestDb { 0.0f }, correlation { 0.0f };
    std::atomic<float> lra { 0.0f };
    std::atomic<float> rmsLdb { -200.0f }, rmsRdb { -200.0f };
    std::atomic<float> peakLdb { -200.0f }, peakRdb { -200.0f };
    std::atomic<float> tpLdb { -200.0f }, tpRdb { -200.0f };
    std::atomic<float> widthPct { 0.0f }, dcOffset { 0.0f };
    std::atomic<float> corrSub { 1.0f }, corrMid { 1.0f }, corrTop { 1.0f }; // banded phase (<120 / 120-5k / >5k)
    std::atomic<int> epoch { 0 };   // bumped on reset/restart so the GUI re-zeros its average

    // ponytail: single mono (L+R) trace — the UI draws one filled curve. Was 4 channels
    //           (L/R/M/S) but only Mid was ever drawn, so the other 3 FFTs were pure waste.
    SpectrumChannel spectrum;

    // Goniometer ring — audio writes rotated L/R (relaxed atomics), GUI reads (relaxed).
    static constexpr int goniN = 2048;
    std::atomic<float> goniX[goniN];   // -side = (R-L)/sqrt2
    std::atomic<float> goniY[goniN];   //  mid  = (L+R)/sqrt2

private:
    double meanRing (int take) const
    {
        const int k = juce::jmin (take, histCount);
        if (k == 0) return 0.0;
        double s = 0.0;
        for (int j = 0; j < k; ++j) s += histRing[(size_t) ((histPos - 1 - j + 30) % 30)];
        return s / (double) k;
    }

    void updateLoudness()
    {
        momentaryLufs = lufsFromZ (meanRing (4));
        const double stZ = meanRing (30);
        shortTermLufs = lufsFromZ (stZ);

        // Gated integrated loudness (BS.1770: -70 absolute, -10 relative, energy domain).
        double absSum = 0.0; int absN = 0;
        for (double z : integratedZ)
            if (lufsFromZ (z) >= -70.0f) { absSum += z; ++absN; }
        if (absN == 0) integratedLufs = -200.0f;
        else
        {
            const double relThresh = lufsFromZ (absSum / absN) - 10.0;
            double relSum = 0.0; int relN = 0;
            for (double z : integratedZ)
                if (lufsFromZ (z) >= relThresh) { relSum += z; ++relN; }
            integratedLufs = relN > 0 ? lufsFromZ (relSum / relN) : -200.0f;
        }

        if (++lraTick >= 10) // 1 s (updateLoudness runs every 100 ms)
        {
            lraTick = 0;
            if (stZ > 1e-12)
            {
                lraRing[(size_t) lraPos] = stZ; // store energy, gate/percentile in energy domain
                lraPos = (lraPos + 1) % kLraCap;
                lraCount = juce::jmin (kLraCap, lraCount + 1);
            }
            computeLRA();
        }
    }

    // EBU Tech 3342: short-term blocks, abs gate -70, relative gate -20 LU (energy mean), P95-P10.
    void computeLRA()
    {
        lraScratch.clear();
        double sumZ = 0.0;
        for (int i = 0; i < lraCount; ++i)
        {
            const double z = lraRing[(size_t) i];
            if (lufsFromZ (z) >= -70.0f) { sumZ += z; lraScratch.push_back (z); }
        }
        if (lraScratch.size() < 2) { lra = 0.0f; return; }
        const float relThresh = lufsFromZ (sumZ / (double) lraScratch.size()) - 20.0f;
        lraLufs.clear();
        for (double z : lraScratch)
        {
            const float L = lufsFromZ (z);
            if (L >= relThresh) lraLufs.push_back (L);
        }
        if (lraLufs.size() < 2) { lra = 0.0f; return; }
        std::sort (lraLufs.begin(), lraLufs.end());
        const auto pct = [this] (double p) {
            const double idx = p * (lraLufs.size() - 1);
            const int i = (int) idx; const double f = idx - i;
            return i + 1 < (int) lraLufs.size() ? lraLufs[(size_t) i] + (float) f * (lraLufs[(size_t) i + 1] - lraLufs[(size_t) i])
                                                : lraLufs[(size_t) i];
        };
        lra = juce::jmax (0.0f, pct (0.95) - pct (0.10));
    }

    double sampleRate = 48000.0;
    int channels = 2;
    int maxBlockSamples = 512;
    int subBlockLen = 4800, subCount = 0;

    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                   juce::dsp::IIR::Coefficients<float>> shelf, hpf;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::AudioBuffer<float> tpScratch, kbScratch;

    std::vector<double> subAccum;
    double histRing[30] = {};
    int histCount = 0, histPos = 0;

    // ponytail: integratedZ reserves ~5.5 h of 100 ms blocks once; once full it stops
    //           growing (integrated then reflects the captured span) — never reallocs.
    static constexpr size_t kIntegratedCap = 200000;
    std::vector<double> integratedZ;

    static constexpr int kLraCap = 3600; // 1 h of 1 s short-term samples
    double lraRing[kLraCap] = {};
    int lraCount = 0, lraPos = 0, lraTick = 0;
    std::vector<double> lraScratch;  // reused, reserved in prepare (no RT alloc)
    std::vector<float>  lraLufs;

    double smPeak = 0.0, smRms = 0.0, smXY = 0.0, smXX = 0.0, smYY = 0.0;
    double smRmsLv = 0.0, smRmsRv = 0.0, smSideMS = 0.0;
    double smPeakLv = 0.0, smPeakRv = 0.0, smPeakMono = 0.0, smTpLv = 0.0, smTpRv = 0.0;
    double smDcL = 0.0, smDcR = 0.0;
    int goniPos = 0, goniDecim = 0;
    static constexpr double smooth = 0.999;
    static constexpr double peakDecay = 0.9999;

    // banded correlation: per-channel crossovers (sub <120, mid 120-5k, top >5k) + accumulators
    juce::dsp::IIR::Filter<float> bSubL, bSubR, bTopL, bTopR, bMidHpL, bMidHpR, bMidLpL, bMidLpR;
    double smXYs = 0, smXXs = 0, smYYs = 0, smXYm = 0, smXXm = 0, smYYm = 0, smXYt = 0, smXXt = 0, smYYt = 0;

    static void accumBand (double& xy, double& xx, double& yy, double a, double b)
    {
        xy = xy * smooth + (1.0 - smooth) * (a * b);
        xx = xx * smooth + (1.0 - smooth) * (a * a);
        yy = yy * smooth + (1.0 - smooth) * (b * b);
    }
    static float corrFrom (double xy, double xx, double yy)
    {
        const double d = std::sqrt (xx * yy);
        return d > 1e-12 ? (float) juce::jlimit (-1.0, 1.0, xy / d) : 1.0f;
    }
};
