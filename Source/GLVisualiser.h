#pragma once
#include <juce_opengl/juce_opengl.h>
#include "PluginProcessor.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// Snapshot of GUI-thread state handed to the GL render thread. The goniometer ring and scalar
// meters are already lock-free atomics (read live on the GL thread); only spectrum.scope[] is
// GUI-written, so it travels here. Colours are resolved from the theme on the GUI thread so the
// GL thread never touches editor members.
struct VizFrame
{
    float scope[200] = {};
    float rmsN = 0.0f;
    int   mode = 0;                                  // shapeIndex: 0 Orb 1 Ring 2 Helix 3 Nebula
    float colP[3] = { 0.21f, 0.77f, 0.88f };         // particle colour
    float colC[3] = { 1.0f, 1.0f, 1.0f };            // core colour
    float bg[3]   = { 0.04f, 0.047f, 0.063f };       // background
};

// GPU particle visualiser for the Visualisation view. Renders on its own OpenGL thread, reading
// live meter atomics + a seqlock snapshot — never touches JUCE components from the GL thread.
// If GL/shaders are unavailable, isReady() stays false and the editor keeps its CPU drawScope
// path, so the plugin never ships a black view.
//
// ponytail: one shader, one VBO of the goniometer ring for now (Phase 1). Bloom + particle
//           simulation + shape modes come in later phases; this is the smallest thing that
//           proves the GL context lifecycle + fallback end-to-end.
class GLVisualiser : public juce::Component,
                     public juce::OpenGLRenderer
{
public:
    explicit GLVisualiser (MakeMeterProcessor&);
    ~GLVisualiser() override;

    void pushFrame (const VizFrame&) noexcept;       // GUI thread
    void setActive (bool shouldAnimate);              // GUI thread — gate continuous repaint
    bool isReady() const noexcept { return shadersOk.load (std::memory_order_acquire); }

    void newOpenGLContextCreated() override;          // GL thread
    void renderOpenGL() override;                     // GL thread
    void openGLContextClosing() override;             // GL thread

private:
    // SPSC seqlock: single GUI writer, single GL reader. `frame` is written unlocked; the reader
    // detects a torn copy via the odd/even sequence and keeps its last good frame.
    // ponytail: one reader only — do not add a second consumer of this snapshot.
    struct Snapshot
    {
        void write (const VizFrame& f) noexcept
        {
            seq.fetch_add (1, std::memory_order_release);   // -> odd (writing)
            frame = f;
            seq.fetch_add (1, std::memory_order_release);   // -> even (done)
        }
        bool read (VizFrame& out) const noexcept
        {
            const auto s0 = seq.load (std::memory_order_acquire);
            if (s0 & 1u) return false;
            out = frame;
            return s0 == seq.load (std::memory_order_acquire);
        }
        std::atomic<std::uint32_t> seq { 0 };
        VizFrame frame {};
    };

    bool ensureFbos (int w, int h);                     // GL thread: (re)allocate scene + bloom FBOs; false if any failed
    void drawFullscreen();                              // GL thread: covering triangle (no VBO)
    void updateParticles (float dt, const VizFrame&);   // GL thread: simulate the pool one step
    void respawn (int index, const VizFrame&);          // GL thread: reseed a dead particle by mode

    // CPU-simulated particle pool (uploaded to a VBO each frame). One engine, four modes.
    static constexpr int kParticleCount = 16384;
    struct Particle { float x = 0, y = 0, life = 0, ilife = 1, seed = 0, energy = 0,
                            disp = 0, sx = 0, sy = 0, sz = 0; int band = 0; };  // sx/sy/sz: 3D base
    std::vector<Particle> pool;
    juce::Random rng;
    double lastTimeS = -1.0;
    int lastMode = -1;          // re-seed the whole pool when the shape mode changes

    MakeMeterProcessor& proc;
    juce::OpenGLContext context;                        // declared early -> torn down after GL members
    Snapshot snapshot;
    std::atomic<bool> shadersOk { false };

    std::unique_ptr<juce::OpenGLShaderProgram> particleProg, brightProg, blurProg, compositeProg;
    unsigned int vbo = 0, vao = 0, fsVao = 0;
    int aPosLoc = -1, aEnergyLoc = -1, aSizeLoc = -1;

    // Bloom pipeline: particles -> sceneFbo (full res) -> bright/blur (half res, ping-pong) -> screen.
    juce::OpenGLFrameBuffer sceneFbo, bloomFbo0, bloomFbo1;
    int fboW = 0, fboH = 0;

    VizFrame lastFrame {};                              // GL-thread private (last good snapshot)
    std::vector<float> verts;                           // GL-thread scratch (no per-frame alloc after warmup)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GLVisualiser)
};
