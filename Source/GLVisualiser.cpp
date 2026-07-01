#include "GLVisualiser.h"
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;

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
    const float life = juce::jmap (f.rmsN, 2.5f, 0.9f);   // louder -> shorter life -> more churn
    p.life  = 1.0f;
    p.ilife = 1.0f / juce::jmax (0.4f, life);
    p.energy = 0.0f;

    if (mode == 0) // Orb: a fixed point on a unit sphere (rotated + projected each frame)
    {
        const float u = rng.nextFloat(), v = rng.nextFloat();
        const float z = 2.0f * u - 1.0f;
        const float rr = std::sqrt (juce::jmax (0.0f, 1.0f - z * z));
        const float th = kTwoPi * v;
        // ~6% sit further out as sparse "dust" in the void around the sphere
        const float mag = (rng.nextFloat() < 0.06f) ? (1.35f + rng.nextFloat() * 0.7f) : 1.0f;
        p.sx = rr * std::cos (th) * mag;
        p.sy = rr * std::sin (th) * mag;
        p.sz = z * mag;
        p.x = p.sx; p.y = p.sy; p.vx = p.vy = 0.0f;
        // long, staggered life so the sphere is stable and respawns never blink in sync
        p.life  = 0.2f + rng.nextFloat() * 0.8f;
        p.ilife = 1.0f / 40.0f;
    }
    else if (mode == 1) // Ring: on the band's circle (seed fills the angle continuously between bands)
    {
        const float ang = kTwoPi * ((float) p.band + p.seed) / 200.0f;
        p.x = std::cos (ang) * 0.28f;
        p.y = std::sin (ang) * 0.28f;
        p.vx = p.vy = 0.0f;
    }
    else if (mode == 2) // Helix: along the width (position is analytic each frame)
    {
        p.x = (p.seed - 0.5f) * 1.7f;
        p.y = 0.0f; p.vx = p.vy = 0.0f;
    }
    else // Nebula: uniform in a disc
    {
        const float ang = rng.nextFloat() * kTwoPi;
        const float r   = std::sqrt (rng.nextFloat()) * 0.6f;
        p.x = std::cos (ang) * r;
        p.y = std::sin (ang) * r;
        p.vx = (rng.nextFloat() - 0.5f) * 0.1f;
        p.vy = (rng.nextFloat() - 0.5f) * 0.1f;
    }
}

void GLVisualiser::updateParticles (float dt, const VizFrame& f)
{
    const int   mode   = juce::jlimit (0, 3, f.mode);
    const float rmsN   = f.rmsN;
    const float spread = juce::jmap (juce::jlimit (0.0f, 1.0f, (1.0f - f.correlation) * 0.5f), 1.0f, 1.6f);
    const float t      = (float) lastTimeS;
    // Frame-constant terms hoisted out of the per-particle loop (matters at 16k particles).
    const float yaw = t * (0.18f + rmsN * 0.5f);
    const float cs = std::cos (yaw), sn = std::sin (yaw);
    const float dRing = std::pow (0.80f, dt), dNeb = std::pow (0.90f, dt);

    for (int i = 0; i < kParticleCount; ++i)
    {
        auto& p = pool[(size_t) i];
        p.life -= p.ilife * dt;
        if (p.life <= 0.0f) { respawn (i, f); continue; }

        // Local energy target: idle floor keeps a living glow in silence; spectrum band + RMS light it up.
        const float bandE   = std::pow (juce::jlimit (0.0f, 1.0f, f.scope[p.band]), 0.6f);
        const float targetE = juce::jlimit (0.0f, 1.0f, 0.12f + 0.25f * rmsN + 0.8f * bandE);
        p.energy += (targetE - p.energy) * juce::jmin (1.0f, dt * 6.0f);
        p.disp = p.energy;   // default display value (drives size + brightness); Orb folds in depth

        if (mode == 0) // Orb: rotate the sphere base around Y, project, shade by depth
        {
            const float rx = p.sx * cs + p.sz * sn;
            const float rz = -p.sx * sn + p.sz * cs;
            const float rad = 1.0f + p.energy * 0.22f;              // spectrum-reactive radial push
            p.x = rx * rad;
            p.y = p.sy * rad;
            const float depth = 0.4f + 0.6f * (rz * 0.5f + 0.5f);  // back .. front
            p.disp = juce::jlimit (0.0f, 1.0f, (0.5f + p.energy) * depth);
        }
        else if (mode == 2) // Helix: analytic travelling double-helix (overwrite position)
        {
            // ponytail: Helix rides an analytic curve, it is not force-integrated.
            const float ribbon = (float) (p.band & 1);
            const float u = p.seed;
            const float scroll = t * (0.25f + 0.5f * rmsN);
            const float amp = 0.5f * (0.4f + 0.6f * p.energy);
            p.x = (u - 0.5f) * 1.7f;
            p.y = std::sin (u * kTwoPi * 3.0f + ribbon * 3.14159f + scroll * kTwoPi) * amp;
        }
        else // Ring / Nebula: force-integrated
        {
            const float d = (mode == 1 ? dRing : dNeb);
            p.vx *= d; p.vy *= d;

            if (mode == 1) // Ring: spring to r(band) at a rotating angle
            {
                const float ang = kTwoPi * ((float) p.band + p.seed) / 200.0f + t * 0.6f * (0.5f + rmsN);
                const float rT  = 0.28f + 0.55f * std::pow (juce::jlimit (0.0f, 1.0f, f.scope[p.band]), 0.6f);
                p.vx += (std::cos (ang) * rT - p.x) * 4.0f * dt;
                p.vy += (std::sin (ang) * rT - p.y) * 4.0f * dt;
            }
            else // Nebula: slow curl drift
            {
                const float a = p.seed * kTwoPi + t * 0.2f;
                p.vx += std::cos (a) * 0.15f * dt;
                p.vy += std::sin (a * 1.3f) * 0.15f * dt;
            }
            p.x += p.vx * dt * spread;
            p.y += p.vy * dt * spread;
        }
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
    const float basePx = (f.mode == 0 ? 3.5f : f.mode == 1 ? 6.0f : f.mode == 2 ? 5.0f : 8.0f);
    verts.resize ((size_t) P * 4);
    for (int i = 0; i < P; ++i)
    {
        const auto& p = pool[(size_t) i];
        const float age  = 1.0f - p.life;
        const float fade = (f.mode == 0) ? 1.0f    // Orb sphere is stable — no life-fade twinkle
                         : juce::jmin (juce::jlimit (0.0f, 1.0f, age / 0.15f),
                                       juce::jlimit (0.0f, 1.0f, p.life / 0.30f));
        verts[(size_t) i * 4 + 0] = p.x;
        verts[(size_t) i * 4 + 1] = p.y;
        verts[(size_t) i * 4 + 2] = p.disp * fade;
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
