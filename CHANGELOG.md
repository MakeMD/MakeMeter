# Changelog

## 1.0.0

First public release.

- Loudness: Momentary / Short‑term / Integrated LUFS (ITU‑R BS.1770‑4, gated, EBU‑calibrated),
  LRA, true peak (4× oversampled dBTP + max‑hold), target‑LUFS line with colour‑coded read‑out
- Levels: RMS (L/R), crest factor, DC offset
- Stereo: correlation, width %, S/M ratio, banded correlation (sub / mid / top), goniometer
- Spectrum: runtime FFT (1024–8192), log axis, spectral slope, smooth Catmull‑Rom curve,
  peak‑hold, long‑term average, reference overlay (freeze / load track), A/B compare
- Visualisation: Orb / Ring / Helix / Nebula goniometer shapes, six themes
- Local‑AI mix feedback via Ollama (chat bubbles + captured‑window waveform); only meter
  readings are sent, audio never leaves the machine
- Settings profile (gear, room, genres, workflow) feeding the AI context; installed‑VST3 scan
- Verification: DSP self‑checks, EBU LUFS calibration + BS.1770 sweep (≤ 0.22 dB, 40 Hz–16 kHz),
  pluginval strictness‑level 10 pass
