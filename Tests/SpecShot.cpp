// Visual + numeric check for spectrum smoothness. Feeds pink noise into SpectrumChannel,
// averages many frames, renders the same Catmull-Rom curve the plugin draws to a PNG, and
// prints the longest flat run of near-equal points in the low third (the stair-step signature).
// A blocky (nearest-neighbour) spectrum shows long flat runs + plateaus in the image; the
// interpolated one is a smooth slope with flat-run ~1.
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "Meters.h"
#include <random>
#include <iostream>

static juce::Path catmull (const std::vector<juce::Point<float>>& pts)
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

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String outPath = argc > 1 ? juce::String (argv[1]) : "spec.png";

    SpectrumChannel sp;
    sp.setup (48000.0, 12); // 4096-point FFT

    std::mt19937 rng (20260701u);
    std::uniform_real_distribution<float> d (-1.0f, 1.0f);
    float p0 = 0, p1 = 0, p2 = 0;
    auto pink = [&] (float w) {
        p0 = 0.99765f * p0 + w * 0.0990460f;
        p1 = 0.96300f * p1 + w * 0.2965164f;
        p2 = 0.57000f * p2 + w * 1.0526913f;
        return (p0 + p1 + p2 + w * 0.1848f) * 0.2f;
    };
    for (int f = 0; f < 400; ++f)
    {
        for (int i = 0; i < 4096; ++i) sp.pushSample (pink (d (rng)));
        sp.render (0.0f);
    }

    const int N = SpectrumChannel::numBins;
    const int W = 660, H = 260, pad = 10;
    // Force the software renderer: JUCE 8's default Image on Windows can be a Direct2D image
    // that a headless console app can't render into (comes out fully transparent).
    juce::Image img (juce::SoftwareImageType().create (juce::Image::ARGB, W, H, true));
    juce::Graphics g (img);
    g.fillAll (juce::Colour (0xff0a0c10));
    juce::Rectangle<float> sf ((float) pad, (float) pad, (float) (W - 2 * pad), (float) (H - 2 * pad));

    std::vector<juce::Point<float>> v; v.reserve ((size_t) N);
    for (int i = 0; i < N; ++i)
        v.push_back ({ sf.getX() + sf.getWidth() * ((float) i / (N - 1)),
                       sf.getBottom() - sp.averageScope (i) * sf.getHeight() });

    auto top = catmull (v);
    auto fill = top;
    fill.lineTo (sf.getRight(), sf.getBottom());
    fill.lineTo (sf.getX(),     sf.getBottom());
    fill.closeSubPath();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2bb0c4).withAlpha (0.32f), sf.getX(), sf.getY(),
                                             juce::Colour (0xff2bb0c4).withAlpha (0.02f), sf.getX(), sf.getBottom(), false));
    g.fillPath (fill);
    g.setColour (juce::Colour (0xff35c4e0));
    g.strokePath (top, juce::PathStrokeType (1.6f));

    int maxRun = 1, run = 1;
    for (int i = 1; i < N / 3; ++i)
    {
        if (std::abs (sp.averageScope (i) - sp.averageScope (i - 1)) < 1.0e-4f) { if (++run > maxRun) maxRun = run; }
        else run = 1;
    }

    juce::File out = juce::File::isAbsolutePath (outPath)
                       ? juce::File (outPath)
                       : juce::File::getCurrentWorkingDirectory().getChildFile (outPath);
    out.deleteFile();
    juce::FileOutputStream fos (out);
    juce::PNGImageFormat png;
    png.writeImageToStream (img, fos);

    std::cout << "max flat-run (low third) = " << maxRun
              << "  (interpolated ~1-2; nearest-neighbour would be much larger)\n"
              << "saved " << out.getFullPathName() << std::endl;
    return 0;
}
