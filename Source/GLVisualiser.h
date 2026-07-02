#pragma once
#include <juce_opengl/juce_opengl.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// Snapshot of GUI-thread state handed to the GL render thread: the spectrum scope, normalised
// RMS, shape mode and theme colours. Everything the GL thread consumes travels in this POD —
// it never touches editor or processor members.
struct VizFrame
{
    float scope[200] = {};
    float rmsN = 0.0f;
    int   mode = 0;                                  // shapeIndex: 0 Orb 1 Rhombus 2 Cube 3 Nebula
    float colP[3] = { 0.21f, 0.77f, 0.88f };         // particle colour
    float colC[3] = { 1.0f, 1.0f, 1.0f };            // core colour
    float bg[3]   = { 0.04f, 0.047f, 0.063f };       // background
};

// GPU particle visualiser for the Visualisation view: a CPU-simulated particle pool drawn as
// 3D-perspective point sprites with a bloom pipeline, on JUCE's OpenGL render thread. Data
// arrives only via the seqlock snapshot below. If GL/shaders are unavailable, isReady() stays
// false and the editor keeps its CPU drawScope path, so the plugin never shows a black view.
class GLVisualiser : public juce::Component,
                     public juce::OpenGLRenderer
{
public:
    GLVisualiser();
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
            seq.fetch_add (1, std::memory_order_acq_rel);   // -> odd; acquire half pins the stores below it
            frame = f;
            seq.fetch_add (1, std::memory_order_release);   // -> even (done)
        }
        bool read (VizFrame& out) const noexcept
        {
            const auto s0 = seq.load (std::memory_order_acquire);
            if (s0 & 1u) return false;
            out = frame;
            std::atomic_thread_fence (std::memory_order_acquire);   // copy must not sink below the check
            return s0 == seq.load (std::memory_order_relaxed);
        }
        std::atomic<std::uint32_t> seq { 0 };
        VizFrame frame {};
    };

    bool ensureFbos (int w, int h);                     // GL thread: (re)allocate scene + bloom FBOs; false if any failed
    void drawFullscreen();                              // GL thread: covering triangle (no VBO)
    void updateParticles (float dt, const VizFrame&);   // GL thread: simulate the pool one step
    void respawn (int index, const VizFrame&);          // GL thread: reseed a dead particle by mode

    // CPU-simulated particle pool (uploaded to a VBO each frame). One engine, four modes.
    static constexpr int kParticleCount = 24576;
    struct Particle { float x = 0, y = 0, z = 0, life = 0, ilife = 1, seed = 0, energy = 0,
                            disp = 0, sx = 0, sy = 0, sz = 0, phase = 0; int band = 0; };  // x/y/z: 3D pos, sx/sy/sz: base
    std::vector<Particle> pool;
    juce::Random rng;
    double lastTimeS = -1.0;
    int lastMode = -1;          // re-seed the whole pool when the shape mode changes
    float pulseEnv = 0.0f;      // smoothed mid-band pulse driver -> controls how fast the shell breathes
    // Animation state integrates rate*dt per frame. Never multiply an absolute clock by a
    // time-varying rate: the angle would jump wildly with every rate change at large t, and
    // (float) casts of an absolute clock lose frame-level precision after ~1.5 days of uptime.
    float animTime = 0.0f;      // GL-thread animation clock, starts at 0 on context creation
    float yawAngle = 0.0f;      // accumulated Y rotation for the spinning modes

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
