# MakeMeter

A Windows VST3 / Standalone audio **metering plugin with local‑AI mix feedback**, built with
[JUCE](https://juce.com). It is a pure analyser — it measures the signal and passes audio
through untouched.

The AI side is privacy‑preserving by design: **only the numeric meter readings are sent to a
local model — the audio never leaves your machine.**

## Features

**Loudness (ITU‑R BS.1770‑4, EBU‑calibrated)**
- Momentary / Short‑term / Integrated LUFS with gating, and Loudness Range (LRA)
- True peak, 4× oversampled (dBTP) with max‑hold
- Target‑LUFS line with colour‑coded Integrated read‑out

**Levels & dynamics**
- RMS (L/R), crest factor, DC offset

**Stereo image**
- Correlation, width %, S/M ratio
- Banded correlation (sub &lt;120 Hz / mid 120 Hz–5 kHz / top &gt;5 kHz)
- Goniometer

**Spectrum**
- Runtime FFT size (1024–8192), log frequency axis, adjustable spectral slope
- Smooth curve (Catmull‑Rom), peak‑hold and long‑term average
- Reference overlay: freeze the current spectrum or load a track; A/B compare

**Visualisation view**
- Goniometer shapes — Orb / Ring / Helix / Nebula — with six colour themes

**AI mix feedback (local)**
- Sends a measurement snapshot to a local [Ollama](https://ollama.com) server and shows advice
  in a chat panel (bubbles + a mini waveform of the captured window)
- Whole‑track or last‑15‑seconds snapshot; request runs on a background thread (no GUI freeze)
- Default model `qwen3:14b`; model and endpoint are configurable in Settings
- To use a hosted API instead, see the note in `Source/MixAdvisor.h`

**Settings / context**
- Profile (name, DAWs, experience, monitors, headphones, room, genres, workflow) that tunes the
  AI's advice; scans installed VST3s so it phrases suggestions in tools you actually own

## Build

Requirements: Windows, Visual Studio 2022 (MSVC), CMake ≥ 3.22. JUCE is fetched automatically.

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

Outputs:
- VST3 → `%LOCALAPPDATA%\Programs\Common\VST3\MakeMeter.vst3` (add to your DAW's scan paths)
- Standalone → `build/MakeMeter_artefacts/Release/Standalone/MakeMeter.exe`

Some hosts only scan `C:\Program Files\Common Files\VST3` — copy the bundle there (host closed) if
your DAW doesn't see the per‑user path.

### Checks

```sh
cmake --build build --config Release --target MakeMeterTest MakeMeterCalib MakeMeterSpec
build/MakeMeterTest_artefacts/Release/MakeMeterTest.exe    # DSP self-checks -> "OK"
build/MakeMeterCalib_artefacts/Release/MakeMeterCalib.exe  # EBU LUFS calibration + BS.1770 sweep
build/MakeMeterSpec_artefacts/Release/MakeMeterSpec.exe out.png  # renders the spectrum curve to a PNG
```

## AI prerequisite

For the AI panel: run `ollama serve` and `ollama pull qwen3:14b`. Without it the meter works
fully; only the advice panel reports no connection.

## Layout

| Path | What |
|------|------|
| `Source/Meters.h` | DSP — loudness, true peak, correlation, banded correlation, spectrum |
| `Source/MixAdvisor.h` | Local Ollama client + prompt |
| `Source/PluginScanner.h` | Installed‑plugin listing |
| `Source/PluginProcessor.*` | APVTS, state, snapshot, reference analysis |
| `Source/PluginEditor.*` | GUI (meters, visualisation, settings, chat) |
| `Tests/MeterTest.cpp` | Runnable DSP assertions |
| `Tests/CalibEbu.cpp` | EBU LUFS calibration harness |
| `Tests/SpecShot.cpp` | Renders the spectrum curve to a PNG |

## License

MIT — see [LICENSE](LICENSE).
