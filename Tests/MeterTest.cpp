// Self-check for LoudnessMeter. Builds as a console app: MakeMeterTest.
// Asserts the properties that break loudly if the DSP is wrong.
#include "Meters.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <cassert>
#include <cstdio>

static constexpr double SR = 48000.0;

// Feed `seconds` of a stereo signal; gen(channel, sampleIndex) -> sample.
template <typename Gen>
static void feed (LoudnessMeter& m, double seconds, Gen gen)
{
    const int total = (int) (SR * seconds);
    int idx = 0;
    while (idx < total)
    {
        const int n = juce::jmin (512, total - idx);
        juce::AudioBuffer<float> buf (2, n);
        for (int i = 0; i < n; ++i)
        {
            buf.setSample (0, i, gen (0, idx + i));
            buf.setSample (1, i, gen (1, idx + i));
        }
        m.process (buf);
        idx += n;
    }
}

static float sine (int sampleIndex, double freq, float amp)
{
    return amp * std::sin (2.0 * juce::MathConstants<double>::pi * freq * sampleIndex / SR);
}

#define CHECK(cond, msg) do { if (!(cond)) { std::printf ("FAIL: %s\n", msg); return 1; } } while (0)

int main()
{
    // 1) Silence reads -inf, perfectly correlated by convention.
    {
        LoudnessMeter m; m.prepare (SR, 2, 11, 1024);
        feed (m, 0.5, [] (int, int) { return 0.0f; });
        CHECK (m.momentaryLufs.load() <= -120.0f, "silence should read ~-inf LUFS");
    }

    // 2) Mono-summed sine L==R -> correlation ~ +1, crest ~ 3.01 dB.
    LoudnessMeter mono; mono.prepare (SR, 2, 11, 1024);
    feed (mono, 2.0, [] (int, int n) { return sine (n, 1000.0, 0.5f); });
    CHECK (mono.correlation.load() > 0.99f, "L==R should give correlation ~ +1");
    CHECK (std::abs (mono.crestDb.load() - 3.01f) < 0.5f, "sine crest should be ~3 dB");
    CHECK (mono.momentaryLufs.load() > -40.0f, "audible sine should read finite LUFS");

    // 3) Anti-phase R == -L -> correlation ~ -1.
    LoudnessMeter anti; anti.prepare (SR, 2, 11, 1024);
    feed (anti, 1.0, [] (int ch, int n)
          { return (ch == 0 ? 1.0f : -1.0f) * sine (n, 1000.0, 0.5f); });
    CHECK (anti.correlation.load() < -0.99f, "R==-L should give correlation ~ -1");

    // 4) Loudness is monotonic: +20 dB amplitude -> ~+20 LUFS.
    LoudnessMeter quiet; quiet.prepare (SR, 2, 11, 1024);
    feed (quiet, 2.0, [] (int, int n) { return sine (n, 1000.0, 0.05f); });
    const float dLufs = mono.momentaryLufs.load() - quiet.momentaryLufs.load();
    CHECK (std::abs (dLufs - 20.0f) < 2.0f, "10x amplitude should raise LUFS by ~20");

    // 5) Gated integrated converges to short-term on a steady tone.
    CHECK (mono.integratedLufs.load() > -40.0f
           && std::abs (mono.integratedLufs.load() - mono.shortTermLufs.load()) < 1.5f,
           "steady tone: integrated should track short-term");

    // 6) Banded correlation: broadband anti-phase -> all three bands negative;
    //    mono -> all three near +1. Broadband = 50 Hz (sub) + 1 kHz (mid) + 8 kHz (top).
    {
        auto broadband = [] (int n) {
            return sine (n, 50.0, 0.3f) + sine (n, 1000.0, 0.3f) + sine (n, 8000.0, 0.3f);
        };
        LoudnessMeter bAnti; bAnti.prepare (SR, 2, 11, 1024);
        feed (bAnti, 2.0, [&] (int ch, int n) { return (ch == 0 ? 1.0f : -1.0f) * broadband (n); });
        CHECK (bAnti.corrSub.load() < 0.0f, "anti-phase: corrSub should be < 0");
        CHECK (bAnti.corrMid.load() < 0.0f, "anti-phase: corrMid should be < 0");
        CHECK (bAnti.corrTop.load() < 0.0f, "anti-phase: corrTop should be < 0");

        LoudnessMeter bMono; bMono.prepare (SR, 2, 11, 1024);
        feed (bMono, 2.0, [&] (int, int n) { return broadband (n); });
        CHECK (bMono.corrSub.load() > 0.99f, "mono: corrSub should be ~ +1");
        CHECK (bMono.corrMid.load() > 0.99f, "mono: corrMid should be ~ +1");
        CHECK (bMono.corrTop.load() > 0.99f, "mono: corrTop should be ~ +1");
    }

    // 7) Block larger than the prepared max must not overrun the oversampler (the
    //    bug that crashed FL during reference-track analysis). Feed 8192 into a 512 prep.
    {
        LoudnessMeter big; big.prepare (SR, 2, 11, 512);
        juce::AudioBuffer<float> buf (2, 8192);
        for (int i = 0; i < 8192; ++i)
        {
            const float s = 0.5f * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / SR);
            buf.setSample (0, i, s); buf.setSample (1, i, s);
        }
        big.process (buf); // would crash before the slice guard
        CHECK (big.momentaryLufs.load() > -40.0f, "oversized block should process without crashing");
    }

    std::printf ("OK: all meter checks passed\n");
    return 0;
}
