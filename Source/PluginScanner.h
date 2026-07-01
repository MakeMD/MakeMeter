#pragma once
#include <juce_core/juce_core.h>

// Lists installed plugins so the AI can phrase advice in tools you actually own.
//
// ponytail: scans the standard VST3 folders by *filename*, enriching with Vendor read from the
//           bundle's moduleinfo.json (no instantiation — safe & fast). Single-file .vst3 with no
//           moduleinfo fall back to filename only. AU (macOS) and DAW stock plugins are out of scope.
class PluginScanner
{
public:
    static juce::StringArray scanInstalled()
    {
        juce::StringArray names;
        for (auto& dir : vst3Dirs())
        {
            if (! dir.isDirectory()) continue;
            for (auto& f : dir.findChildFiles (juce::File::findFilesAndDirectories, true, "*.vst3"))
            {
                // Skip the inner binary inside a bundle (…/Name.vst3/Contents/x86_64-win/Name.vst3).
                if (f.getParentDirectory().getFullPathName().containsIgnoreCase (".vst3")) continue;
                const auto nm = f.getFileNameWithoutExtension();
                const auto vendor = vendorOf (f);
                names.addIfNotAlreadyThere (vendor.isNotEmpty() ? nm + " (" + vendor + ")" : nm);
            }
        }
        names.sort (true);
        return names;
    }

private:
    static juce::String vendorOf (const juce::File& vst3)
    {
        auto mi = vst3.getChildFile ("Contents/Resources/moduleinfo.json"); // bundle only
        if (! mi.existsAsFile()) return {};
        const auto v = juce::JSON::parse (mi.loadFileAsString());
        if (auto* o = v.getDynamicObject())
        {
            const auto fi = o->getProperty ("Factory Info");
            if (auto* fo = fi.getDynamicObject()) return fo->getProperty ("Vendor").toString();
        }
        return {};
    }

    static juce::Array<juce::File> vst3Dirs()
    {
        using SL = juce::File::SpecialLocationType;
        return {
            juce::File ("C:/Program Files/Common Files/VST3"),
            juce::File::getSpecialLocation (SL::userApplicationDataDirectory)
                .getParentDirectory().getChildFile ("Local/Programs/Common/VST3"),
            juce::File::getSpecialLocation (SL::globalApplicationsDirectory)
                .getChildFile ("Common Files/VST3")
        };
    }
};
