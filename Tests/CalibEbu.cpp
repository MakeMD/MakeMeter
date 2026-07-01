// EBU R128 / ITU-R BS.1770 absolute-level calibration.
// Generates the canonical compliance signals and prints measured LUFS vs expected.
// Anchor (EBU Tech 3341): a stereo 1 kHz sine at -23 dBFS must read -23.0 LUFS.
#include "Meters.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdio>
#include <cmath>
#include <complex>

static constexpr double SR = 48000.0;

// Exact ITU-R BS.1770-4 K-weighting magnitude (dB) at 48 kHz, from the published
// biquad coefficients. Used as the reference the meter's curve is checked against.
static double idealKdB (double f)
{
    auto mag = [f] (double b0, double b1, double b2, double a1, double a2)
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * f / SR;
        const std::complex<double> e1 = std::exp (std::complex<double> (0, -w));
        const std::complex<double> e2 = std::exp (std::complex<double> (0, -2 * w));
        return std::abs ((b0 + b1 * e1 + b2 * e2) / (1.0 + a1 * e1 + a2 * e2));
    };
    const double s1 = mag (1.53512485958697, -2.69169618940638, 1.19839281085285,  // pre-filter (shelf)
                          -1.69065929318241,  0.73248077421585);
    const double s2 = mag (1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621);    // RLB high-pass
    return 20.0 * std::log10 (s1 * s2);
}

static float measure (double dBFS, double freq, bool leftOnly, float& momentary)
{
    const float a = (float) std::pow (10.0, dBFS / 20.0); // amplitude (full-scale sine = 0 dBFS)
    LoudnessMeter m; m.prepare (SR, 2, 11, 1024);
    const int total = (int) (SR * 4.0);
    int idx = 0;
    while (idx < total)
    {
        const int n = juce::jmin (1024, total - idx);
        juce::AudioBuffer<float> buf (2, n);
        for (int i = 0; i < n; ++i)
        {
            const float s = a * std::sin (2.0 * juce::MathConstants<double>::pi * freq * (idx + i) / SR);
            buf.setSample (0, i, s);
            buf.setSample (1, i, leftOnly ? 0.0f : s);
        }
        m.process (buf);
        idx += n;
    }
    momentary = m.momentaryLufs.load();
    return m.integratedLufs.load();
}

int main()
{
    struct Case { const char* name; double dBFS; double freq; bool left; double expect; };
    const Case cases[] = {
        { "Stereo 1kHz -23 dBFS (EBU anchor)", -23.0, 1000.0, false, -23.00 },
        { "Stereo 1kHz -33 dBFS",              -33.0, 1000.0, false, -33.00 },
        { "Stereo 1kHz -40 dBFS",              -40.0, 1000.0, false, -40.00 },
        { "Left-only 1kHz -23 dBFS",           -23.0, 1000.0, true,  -26.01 },
    };

    std::printf ("%-36s %10s %10s %8s\n", "case", "expect", "integrated", "delta");
    double worst = 0.0;
    for (const auto& c : cases)
    {
        float mom = 0.0f;
        const float i = measure (c.dBFS, c.freq, c.left, mom);
        const double d = i - c.expect;
        worst = juce::jmax (worst, std::abs (d));
        std::printf ("%-36s %10.2f %10.2f %+8.2f\n", c.name, c.expect, i, d);
    }
    std::printf ("\nworst |delta| = %.2f LU  (EBU tolerance +/-0.1; <=0.5 is fine for a mix meter)\n", worst);

    // --- frequency sweep: meter's K-weighting curve vs exact BS.1770 ---
    std::printf ("\nFrequency sweep (stereo -23 dBFS) — relative to 1 kHz, vs exact BS.1770 K-weighting:\n");
    std::printf ("%8s %10s %10s %8s\n", "freq", "ideal dB", "meter dB", "delta");
    const double freqs[] = { 20, 40, 60, 100, 200, 500, 1000, 2000, 3000, 5000, 8000, 12000, 16000, 20000 };
    const double kRef = idealKdB (1000.0);
    float dummy = 0.0f;
    const float meter1k = measure (-23.0, 1000.0, false, dummy); // meter reading at 1 kHz
    double worstSweep = 0.0;
    for (double f : freqs)
    {
        const double idealRel = idealKdB (f) - kRef;                 // expected gain rel. to 1 kHz
        const double meterRel = measure (-23.0, f, false, dummy) - meter1k;
        const double d = meterRel - idealRel;
        if (f >= 40.0 && f <= 16000.0) worstSweep = juce::jmax (worstSweep, std::abs (d));
        std::printf ("%7.0f %+10.2f %+10.2f %+8.2f\n", f, idealRel, meterRel, d);
    }
    std::printf ("\nworst |delta| over 40 Hz..16 kHz = %.2f dB\n", worstSweep);
    return 0;
}
