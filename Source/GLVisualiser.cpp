#include "GLVisualiser.h"
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
inline float fracf (float v) { return v - std::floor (v); }   // cheap per-particle pseudo-random

// GLSL 1.50 (OpenGL 3.2 core).

// --- particle point sprite (additive glow); size + brightness carry the fade ---
const char* kVert = R"(#version 150 core
in vec2 aPos;
in float aEnergy;   // energy * fade -> colour brightness + alpha
in float aSize;     // device-independent base size
uniform vec2  uAspect;
uniform float uScale;
uniform float uPulse;
uniform vec3  uColParticle;
uniform vec3  uColCore;
out vec3 vCol;
void main()
{
    gl_Position  = vec4 (aPos * uAspect * uPulse, 0.0, 1.0);
    gl_PointSize = aSize * uScale * uPulse;
    float e = clamp (aEnergy, 0.0, 1.0);
    // e*e keeps most particles at the theme colour; the white-ish core only shows on the hottest.
    // Modest per-particle brightness so dense stacks don't clip G/B to white (theme hue survives).
    vCol = mix (uColParticle, uColCore, e * e) * (e * 0.9);   // -> 0 as the particle fades
}
)";

const char* kFrag = R"(#version 150 core
in vec3 vCol;
out vec4 frag;
void main()
{
    vec2 uv = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot (uv, uv);
    if (r2 > 1.0) discard;
    // tight bright core + faint halo -> crisp sparkle instead of a soft blob
    float a = exp (-r2 * 7.0) + exp (-r2 * 2.2) * 0.3;
    frag = vec4 (vCol * a, a);
}
)";

// --- fullscreen covering triangle (no VBO) ---
const char* kFsVert = R"(#version 150 core
out vec2 vUv;
void main()
{
    float x = (gl_VertexID == 1) ? 3.0 : -1.0;
    float y = (gl_VertexID == 2) ? 3.0 : -1.0;
    vUv = vec2 (0.5 * (x + 1.0), 0.5 * (y + 1.0));
    gl_Position = vec4 (x, y, 0.0, 1.0);
}
)";

const char* kBrightFrag = R"(#version 150 core
in vec2 vUv;
out vec4 frag;
uniform sampler2D uScene;
uniform float uThreshold;
void main()
{
    vec3 c = texture (uScene, vUv).rgb;
    float l = dot (c, vec3 (0.2126, 0.7152, 0.0722));
    frag = vec4 (c * (max (l - uThreshold, 0.0) / max (l, 1e-4)), 1.0);
}
)";

const char* kBlurFrag = R"(#version 150 core
in vec2 vUv;
out vec4 frag;
uniform sampler2D uTex;
uniform vec2 uDir;
void main()
{
    const float o1 = 1.3846153846, o2 = 3.2307692308;
    const float w0 = 0.2270270270, w1 = 0.3162162162, w2 = 0.0702702703;
    vec3 c  = texture (uTex, vUv).rgb * w0;
    c += texture (uTex, vUv + uDir * o1).rgb * w1;
    c += texture (uTex, vUv - uDir * o1).rgb * w1;
    c += texture (uTex, vUv + uDir * o2).rgb * w2;
    c += texture (uTex, vUv - uDir * o2).rgb * w2;
    frag = vec4 (c, 1.0);
}
)";

const char* kCompFrag = R"(#version 150 core
in vec2 vUv;
out vec4 frag;
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uBloomIntensity;
uniform float uExposure;
uniform vec3  uBg;
void main()
{
    vec3 c = texture (uScene, vUv).rgb + texture (uBloom, vUv).rgb * uBloomIntensity;
    c = vec3 (1.0) - exp (-c * uExposure);
    c += uBg * (1.0 - c);
    frag = vec4 (pow (max (c, 0.0), vec3 (1.0 / 2.2)), 1.0);
}
)";

std::unique_ptr<juce::OpenGLShaderProgram> makeProg (juce::OpenGLContext& ctx,
                                                     const char* vert, const char* frag)
{
    auto prog = std::make_unique<juce::OpenGLShaderProgram> (ctx);
    if (! prog->addVertexShader (vert) || ! prog->addFragmentShader (frag) || ! prog->link())
        return nullptr;
    return prog;
}
} // namespace

GLVisualiser::GLVisualiser (MakeMeterProcessor& p) : proc (p)
{
    setInterceptsMouseClicks (false, false);
    setOpaque (true);
    context.setOpenGLVersionRequired (juce::OpenGLContext::openGL3_2);
    context.setComponentPaintingEnabled (false);
    context.setContinuousRepainting (false);
    context.setRenderer (this);
    context.attachTo (*this);
}

GLVisualiser::~GLVisualiser()
{
    context.detach();   // synchronous: joins GL thread and runs openGLContextClosing() first
}

void GLVisualiser::pushFrame (const VizFrame& f) noexcept { snapshot.write (f); }

void GLVisualiser::setActive (bool a)
{
    setVisible (a);
    context.setContinuousRepainting (a);
}

void GLVisualiser::newOpenGLContextCreated()
{
    using namespace juce::gl;
    if (! context.areShadersAvailable()) { shadersOk.store (false, std::memory_order_release); return; }

    particleProg  = makeProg (context, kVert,   kFrag);
    brightProg    = makeProg (context, kFsVert, kBrightFrag);
    blurProg      = makeProg (context, kFsVert, kBlurFrag);
    compositeProg = makeProg (context, kFsVert, kCompFrag);
    if (particleProg == nullptr || brightProg == nullptr || blurProg == nullptr || compositeProg == nullptr)
    {
        shadersOk.store (false, std::memory_order_release);
        return;
    }

    const auto pid = particleProg->getProgramID();
    aPosLoc    = glGetAttribLocation (pid, "aPos");
    aEnergyLoc = glGetAttribLocation (pid, "aEnergy");
    aSizeLoc   = glGetAttribLocation (pid, "aSize");

    glGenVertexArrays (1, &vao);
    glGenVertexArrays (1, &fsVao);
    glGenBuffers (1, &vbo);

    pool.assign (kParticleCount, Particle{});
    lastTimeS = -1.0;
    shadersOk.store (true, std::memory_order_release);
}

void GLVisualiser::respawn (int i, const VizFrame& f)
{
    auto& p = pool[(size_t) i];
    const int mode = juce::jlimit (0, 3, f.mode);
    p.seed = rng.nextFloat();
    p.band = rng.nextInt (200);
    p.energy = 0.0f;
    // Every mode is a stable 3D form: long, staggered life so re-seeds never blink in sync.
    p.life  = 0.2f + rng.nextFloat() * 0.8f;
    p.ilife = 1.0f / 40.0f;

    if (mode == 0) // Orb: a unit direction; the radius is computed per frame (dense reactive cloud)
    {
        const float u = rng.nextFloat(), v = rng.nextFloat();
        const float z = 2.0f * u - 1.0f;
        const float rr = std::sqrt (juce::jmax (0.0f, 1.0f - z * z));
        const float th = kTwoPi * v;
        p.sx = rr * std::cos (th);
        p.sy = rr * std::sin (th);
        p.sz = z;
    }
    else if (mode == 1) // Ring: a thin tilted torus, spun around Y each frame
    {
        const float major = kTwoPi * p.seed;
        const float R = 0.92f, tube = (rng.nextFloat() - 0.5f) * 0.14f;
        const float bx = std::cos (major) * R, by = tube, bz = std::sin (major) * R;
        const float tilt = 1.05f;                       // lean so the ring reads as 3D
        p.sx = bx;
        p.sy = by * std::cos (tilt) - bz * std::sin (tilt);
        p.sz = by * std::sin (tilt) + bz * std::cos (tilt);
    }
    else if (mode == 2) // Helix: x fixed along the axis; y/z are analytic each frame
    {
        p.sx = (p.seed - 0.5f) * 1.7f;
        p.sy = p.sz = 0.0f;
    }
    else // Nebula: uniform inside a 3D ball -> volumetric cloud
    {
        const float u = rng.nextFloat(), v = rng.nextFloat();
        const float z = 2.0f * u - 1.0f;
        const float rr = std::sqrt (juce::jmax (0.0f, 1.0f - z * z));
        const float th = kTwoPi * v;
        const float rad = std::cbrt (rng.nextFloat()) * 0.9f;   // uniform in volume
        p.sx = rr * std::cos (th) * rad;
        p.sy = rr * std::sin (th) * rad;
        p.sz = z * rad;
    }
    p.x = p.sx; p.y = p.sy;
}

void GLVisualiser::updateParticles (float dt, const VizFrame& f)
{
    const int   mode = juce::jlimit (0, 3, f.mode);
    const float rmsN = f.rmsN;
    const float t    = (float) lastTimeS;
    // Frame-constant terms hoisted out of the per-particle loop (matters at 16k particles).
    const float yaw = t * (0.18f + rmsN * 0.5f);
    const float cs = std::cos (yaw), sn = std::sin (yaw);

    // Shapes are long-lived (stable forms), so a mode switch must re-seed every particle's base
    // now rather than waiting ~40 s for natural respawns.
    if (mode != lastMode) { for (int i = 0; i < kParticleCount; ++i) respawn (i, f); lastMode = mode; }

    for (int i = 0; i < kParticleCount; ++i)
    {
        auto& p = pool[(size_t) i];
        p.life -= p.ilife * dt;
        if (p.life <= 0.0f) { respawn (i, f); continue; }

        // Local energy: idle floor keeps a living glow in silence; spectrum band + RMS light it up.
        const float bandE   = std::pow (juce::jlimit (0.0f, 1.0f, f.scope[p.band]), 0.6f);
        const float targetE = juce::jlimit (0.0f, 1.0f, 0.12f + 0.25f * rmsN + 0.8f * bandE);
        p.energy += (targetE - p.energy) * juce::jmin (1.0f, dt * 6.0f);

        // --- mode-specific 3D position ---
        float px3, py3, pz3, bright = 1.0f;
        if (mode == 2) // Helix: dense vertical DNA — thick bead backbones + chunky rungs + drifting dust
        {
            const float H = 0.95f, R = 0.40f, turns = 3.0f, tubeR = 0.085f;
            if (p.seed < 0.25f)        // dust: a continuous stream of beads dissolving off the helix
            {
                const float u = fracf (p.seed * 7.13f);
                const float a = u * turns * kTwoPi + (float) (p.band & 1) * 3.14159f;
                const float ph   = fracf (p.seed * 5.7f + t * 0.13f);   // travel phase 0..1, loops in time
                const float dist = ph * (0.35f + fracf (p.seed * 23.9f) * 0.8f);
                const float da   = fracf (p.seed * 51.3f) * kTwoPi;
                px3 = std::cos (a) * R + std::cos (da) * dist;
                pz3 = std::sin (a) * R + std::sin (da) * dist;
                py3 = (u - 0.5f) * 2.0f * H + ph * 0.5f + (fracf (p.seed * 71.1f) - 0.5f) * 0.15f;
                bright = (1.0f - ph * 0.8f) * 0.75f;    // fade as it drifts out
            }
            else if (p.seed < 0.42f)   // base-pair rung: a chunky bar across the two strands
            {
                const float u  = (float) (p.band % 14) / 13.0f;
                const float a0 = u * turns * kTwoPi;
                const float s  = fracf (p.seed * 43.0f);
                const float x0 = std::cos (a0) * R, z0 = std::sin (a0) * R;
                px3 = x0 - 2.0f * x0 * s + (fracf (p.seed * 13.7f) - 0.5f) * 0.045f;
                pz3 = z0 - 2.0f * z0 * s + (fracf (p.seed * 61.2f) - 0.5f) * 0.045f;
                py3 = (u - 0.5f) * 2.0f * H + (fracf (p.seed * 17.4f) - 0.5f) * 0.045f;
            }
            else                        // backbone strand: a thick tube of beads (two, phase PI apart)
            {
                const float u = fracf (p.seed * 7.13f);
                const float a = u * turns * kTwoPi + (float) (p.band & 1) * 3.14159f;
                px3 = std::cos (a) * R + (fracf (p.seed * 13.1f) - 0.5f) * tubeR;
                pz3 = std::sin (a) * R + (fracf (p.seed * 51.7f) - 0.5f) * tubeR;
                py3 = (u - 0.5f) * 2.0f * H + (fracf (p.seed * 29.3f) - 0.5f) * tubeR;
            }
        }
        else if (mode == 0)   // Orb: small dense cloud that pulses with loudness and scatters on hits
        {
            const float pulse = 0.5f + rmsN * 0.9f;                       // whole cloud grows when loud
            const float r0 = std::pow (fracf (p.seed * 17.3f), 1.6f);     // 0..1, biased to a dense core
            const float rad = (0.12f + r0 * 0.88f) * 0.6f * pulse + p.energy * 0.55f;  // + per-band scatter
            px3 = p.sx * rad; py3 = p.sy * rad; pz3 = p.sz * rad;
        }
        else                  // Ring / Nebula: base shape scaled by energy
        {
            const float rad = (mode == 1) ? (1.0f + p.energy * 0.30f) : (1.0f + p.energy * 0.12f);
            px3 = p.sx * rad; py3 = p.sy * rad; pz3 = p.sz * rad;
        }

        // --- common: rotate around Y, project to 2D, shade by depth (front brighter/larger) ---
        const float rx = px3 * cs + pz3 * sn;
        const float rz = -px3 * sn + pz3 * cs;
        p.x = rx;
        p.y = py3;
        const float depth = 0.4f + 0.6f * (rz * 0.5f + 0.5f);
        p.disp = juce::jlimit (0.0f, 1.0f, (0.5f + p.energy) * depth * bright);
    }
}

bool GLVisualiser::ensureFbos (int w, int h)
{
    using namespace juce::gl;
    if (w == fboW && h == fboH && sceneFbo.isValid()) return true;
    fboW = w; fboH = h;
    sceneFbo.release(); bloomFbo0.release(); bloomFbo1.release();
    sceneFbo.initialise (context, w, h);
    const int bw = juce::jmax (1, w / 2), bh = juce::jmax (1, h / 2);
    bloomFbo0.initialise (context, bw, bh);
    bloomFbo1.initialise (context, bw, bh);
    if (! sceneFbo.isValid() || ! bloomFbo0.isValid() || ! bloomFbo1.isValid())
        return false;
    for (auto* fb : { &sceneFbo, &bloomFbo0, &bloomFbo1 })
    {
        glBindTexture (GL_TEXTURE_2D, fb->getTextureID());
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture (GL_TEXTURE_2D, 0);
    return true;
}

void GLVisualiser::drawFullscreen()
{
    using namespace juce::gl;
    glBindVertexArray (fsVao);
    glDrawArrays (GL_TRIANGLES, 0, 3);
    glBindVertexArray (0);
}

void GLVisualiser::renderOpenGL()
{
    using namespace juce::gl;

    VizFrame f;
    if (snapshot.read (f)) lastFrame = f; else f = lastFrame;

    const auto scale = (float) context.getRenderingScale();
    const int w = juce::roundToInt (getWidth()  * scale);   // physical (screen) px
    const int h = juce::roundToInt (getHeight() * scale);
    // Supersample the scene + bloom FBOs, then downsample at composite -> anti-aliased, crisp sprites.
    float ss = juce::jmin (1.75f, 4000.0f / (float) juce::jmax (1, juce::jmax (w, h)));
    ss = juce::jmax (1.0f, ss);
    const int fw = juce::roundToInt (w * ss);
    const int fh = juce::roundToInt (h * ss);

    if (w <= 0 || h <= 0 || ! shadersOk.load (std::memory_order_acquire) || particleProg == nullptr)
    {
        glBindFramebuffer (GL_FRAMEBUFFER, context.getFrameBufferID());
        glViewport (0, 0, juce::jmax (1, w), juce::jmax (1, h));
        glClearColor (f.bg[0], f.bg[1], f.bg[2], 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);
        return;
    }

    if (! ensureFbos (fw, fh))
    {
        // FBO allocation failed (e.g. size > GL_MAX_TEXTURE_SIZE, unsupported format).
        // Drop out of the GL branch so the grace window flips glFailed -> CPU drawScope.
        shadersOk.store (false, std::memory_order_release);
        glBindFramebuffer (GL_FRAMEBUFFER, context.getFrameBufferID());
        glViewport (0, 0, w, h);
        glClearColor (f.bg[0], f.bg[1], f.bg[2], 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);
        return;
    }

    // simulate the pool on the GL clock (smooth, independent of the 30 Hz data feed)
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    float dt = (lastTimeS < 0.0) ? 0.016f : (float) (now - lastTimeS);
    lastTimeS = now;
    dt = juce::jlimit (0.0f, 0.05f, dt);
    updateParticles (dt, f);

    const int   P      = kParticleCount;
    const float basePx = (f.mode == 0 ? 2.2f : f.mode == 1 ? 2.5f : f.mode == 2 ? 2.5f : 2.8f);
    verts.resize ((size_t) P * 4);
    for (int i = 0; i < P; ++i)
    {
        const auto& p = pool[(size_t) i];   // all modes are stable 3D forms -> no life-fade
        verts[(size_t) i * 4 + 0] = p.x;
        verts[(size_t) i * 4 + 1] = p.y;
        verts[(size_t) i * 4 + 2] = p.disp;
        verts[(size_t) i * 4 + 3] = basePx * (0.4f + p.disp);
    }

    // ---- Pass 1: particles -> sceneFbo (pure light on black) ----
    sceneFbo.makeCurrentRenderingTarget();
    glViewport (0, 0, fw, fh);
    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    glBindVertexArray (vao);
    glBindBuffer (GL_ARRAY_BUFFER, vbo);
    glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (verts.size() * sizeof (float)), verts.data(), GL_DYNAMIC_DRAW);
    glEnable (GL_BLEND);
    glBlendFunc (GL_ONE, GL_ONE);
    glDisable (GL_DEPTH_TEST);
    glEnable (GL_PROGRAM_POINT_SIZE);

    particleProg->use();
    const float fit = 0.66f;
    const float asp = (float) w / (float) h;
    particleProg->setUniform ("uAspect", asp >= 1.0f ? fit / asp : fit, asp >= 1.0f ? fit : fit * asp);
    particleProg->setUniform ("uScale", scale * ss * (float) getHeight() / 700.0f);
    particleProg->setUniform ("uPulse", 0.9f + f.rmsN * 0.25f);
    particleProg->setUniform ("uColParticle", f.colP[0], f.colP[1], f.colP[2]);
    particleProg->setUniform ("uColCore", f.colC[0], f.colC[1], f.colC[2]);

    const int stride = 4 * (int) sizeof (float);
    glVertexAttribPointer ((GLuint) aPosLoc, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray ((GLuint) aPosLoc);
    glVertexAttribPointer ((GLuint) aEnergyLoc, 1, GL_FLOAT, GL_FALSE, stride, (const void*) (2 * sizeof (float)));
    glEnableVertexAttribArray ((GLuint) aEnergyLoc);
    glVertexAttribPointer ((GLuint) aSizeLoc, 1, GL_FLOAT, GL_FALSE, stride, (const void*) (3 * sizeof (float)));
    glEnableVertexAttribArray ((GLuint) aSizeLoc);
    glDrawArrays (GL_POINTS, 0, P);
    glDisableVertexAttribArray ((GLuint) aPosLoc);
    glDisableVertexAttribArray ((GLuint) aEnergyLoc);
    glDisableVertexAttribArray ((GLuint) aSizeLoc);
    glBindVertexArray (0);
    glBindBuffer (GL_ARRAY_BUFFER, 0);
    sceneFbo.releaseAsRenderingTarget();

    // ---- Pass 2: bloom (bright -> blurH -> blurV) at half of the supersampled res ----
    const int bw = juce::jmax (1, fw / 2), bh = juce::jmax (1, fh / 2);
    glDisable (GL_BLEND);

    bloomFbo0.makeCurrentRenderingTarget();
    glViewport (0, 0, bw, bh);
    brightProg->use();
    glActiveTexture (GL_TEXTURE0); glBindTexture (GL_TEXTURE_2D, sceneFbo.getTextureID());
    brightProg->setUniform ("uScene", (GLint) 0);
    brightProg->setUniform ("uThreshold", 0.62f);   // only bright cores bloom, not the whole haze
    drawFullscreen();
    bloomFbo0.releaseAsRenderingTarget();

    bloomFbo1.makeCurrentRenderingTarget();
    glViewport (0, 0, bw, bh);
    blurProg->use();
    glActiveTexture (GL_TEXTURE0); glBindTexture (GL_TEXTURE_2D, bloomFbo0.getTextureID());
    blurProg->setUniform ("uTex", (GLint) 0);
    blurProg->setUniform ("uDir", 1.0f / (float) bw, 0.0f);
    drawFullscreen();
    bloomFbo1.releaseAsRenderingTarget();

    bloomFbo0.makeCurrentRenderingTarget();
    glViewport (0, 0, bw, bh);
    blurProg->use();
    glActiveTexture (GL_TEXTURE0); glBindTexture (GL_TEXTURE_2D, bloomFbo1.getTextureID());
    blurProg->setUniform ("uTex", (GLint) 0);
    blurProg->setUniform ("uDir", 0.0f, 1.0f / (float) bh);
    drawFullscreen();
    bloomFbo0.releaseAsRenderingTarget();

    // ---- Pass 3: composite -> screen ----
    glBindFramebuffer (GL_FRAMEBUFFER, context.getFrameBufferID());
    glViewport (0, 0, w, h);
    compositeProg->use();
    glActiveTexture (GL_TEXTURE0); glBindTexture (GL_TEXTURE_2D, sceneFbo.getTextureID());
    compositeProg->setUniform ("uScene", (GLint) 0);
    glActiveTexture (GL_TEXTURE1); glBindTexture (GL_TEXTURE_2D, bloomFbo0.getTextureID());
    compositeProg->setUniform ("uBloom", (GLint) 1);
    compositeProg->setUniform ("uBloomIntensity", 1.0f);
    compositeProg->setUniform ("uExposure", 0.90f);   // crisp dots + subtle glow (не серпанок)
    compositeProg->setUniform ("uBg", f.bg[0], f.bg[1], f.bg[2]);
    drawFullscreen();
    glActiveTexture (GL_TEXTURE0);
}

void GLVisualiser::openGLContextClosing()
{
    using namespace juce::gl;
    if (vbo != 0)   { glDeleteBuffers (1, &vbo); vbo = 0; }
    if (vao != 0)   { glDeleteVertexArrays (1, &vao); vao = 0; }
    if (fsVao != 0) { glDeleteVertexArrays (1, &fsVao); fsVao = 0; }
    sceneFbo.release(); bloomFbo0.release(); bloomFbo1.release();
    fboW = fboH = 0;
    particleProg.reset(); brightProg.reset(); blurProg.reset(); compositeProg.reset();
    shadersOk.store (false, std::memory_order_release);
}
