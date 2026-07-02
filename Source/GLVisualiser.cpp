#include "GLVisualiser.h"
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
inline float fracf (float v) { return v - std::floor (v); }   // cheap per-particle pseudo-random

// GLSL 1.50 (OpenGL 3.2 core).

// --- particle point sprite (additive glow); size + brightness carry the fade ---
const char* kVert = R"(#version 150 core
in vec3 aPos;       // 3D position
in float aEnergy;
in float aSize;
uniform mat4  uProj;
uniform mat4  uMV;
uniform float uScale;
uniform vec3  uColParticle;
uniform vec3  uColCore;
out vec3 vCol;
out float vAlpha;
void main()
{
    vec4 mv = uMV * vec4 (aPos, 1.0);
    gl_Position  = uProj * mv;
    gl_PointSize = aSize * uScale / max (gl_Position.w, 0.1);   // perspective: nearer = bigger
    float e = clamp (aEnergy, 0.0, 1.0);
    vCol   = mix (uColParticle, uColCore, e * e);
    float depth = clamp (1.0 + (mv.z + 5.0) * 0.5, 0.4, 1.0);   // nearer -> brighter
    vAlpha = clamp (0.3 + e * 1.3, 0.0, 1.0) * depth;
}
)";

const char* kFrag = R"(#version 150 core
in vec3 vCol;
in float vAlpha;
out vec4 frag;
void main()
{
    // solid disc with a thin soft rim (crisp bead), drawn with normal alpha blending
    float dist = length (gl_PointCoord - vec2 (0.5));
    float a = (1.0 - smoothstep (0.35, 0.5, dist)) * vAlpha;
    frag = vec4 (vCol, a);
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

    if (mode == 0) // Orb: unit direction on a sphere -> particles form a thin pulsating shell
    {
        const float u = rng.nextFloat(), v = rng.nextFloat();
        const float ct = 2.0f * u - 1.0f;                       // cos(latitude)
        const float st = std::sqrt (juce::jmax (0.0f, 1.0f - ct * ct));
        const float ph = kTwoPi * v;
        p.sx = st * std::cos (ph);
        p.sy = ct;                                              // Y = latitude (rotation axis) -> stable poles
        p.sz = st * std::sin (ph);
        p.band = juce::jlimit (0, 199, (int) ((ct * 0.5f + 0.5f) * 199.0f));  // bottom=bass, top=high
    }
    else if (mode == 1) // Ring: a 3D rhombus (octahedron surface) -> diamond
    {
        const float x = rng.nextFloat() * 2.0f - 1.0f;
        const float y = rng.nextFloat() * 2.0f - 1.0f;
        const float z = rng.nextFloat() * 2.0f - 1.0f;
        const float s = std::abs (x) + std::abs (y) + std::abs (z);       // project onto |x|+|y|+|z| = R
        const float inv = (s > 1.0e-4f) ? (0.95f / s) : 0.0f;
        p.sx = x * inv; p.sy = y * inv; p.sz = z * inv;
    }
    else if (mode == 2) // Helix slot: a cube (particles spread over the 6 faces)
    {
        const int face = rng.nextInt (6);
        const float a = rng.nextFloat() * 2.0f - 1.0f, b = rng.nextFloat() * 2.0f - 1.0f, S = 0.72f;
        float x = a, y = b, z = a;
        switch (face)
        {
            case 0:  x =  1.0f; y = a; z = b; break;
            case 1:  x = -1.0f; y = a; z = b; break;
            case 2:  y =  1.0f; x = a; z = b; break;
            case 3:  y = -1.0f; x = a; z = b; break;
            case 4:  z =  1.0f; x = a; y = b; break;
            default: z = -1.0f; x = a; y = b; break;
        }
        p.sx = x * S; p.sy = y * S; p.sz = z * S;
    }
    else // Nebula: an absolutely random particle cluster (soft, irregular, no clean edge)
    {
        const float gx = rng.nextFloat() + rng.nextFloat() + rng.nextFloat() - 1.5f;   // ~gaussian per axis
        const float gy = rng.nextFloat() + rng.nextFloat() + rng.nextFloat() - 1.5f;
        const float gz = rng.nextFloat() + rng.nextFloat() + rng.nextFloat() - 1.5f;
        p.sx = gx * 0.75f; p.sy = gy * 0.75f; p.sz = gz * 0.75f;
    }
    p.x = p.sx; p.y = p.sy;
}

void GLVisualiser::updateParticles (float dt, const VizFrame& f)
{
    const int   mode = juce::jlimit (0, 3, f.mode);
    const float rmsN = f.rmsN;
    const float t    = (float) lastTimeS;
    // Mid-band average drives the Orb's main pulse (bass sits at the centre, highs at the edges).
    float midSum = 0.0f; for (int b = 66; b < 150; ++b) midSum += f.scope[b];
    const float midRaw = juce::jlimit (0.0f, 1.0f, midSum / 84.0f);
    // Smooth the pulse driver so the shell breathes at half speed instead of snapping to transients.
    pulseEnv += (midRaw - pulseEnv) * juce::jmin (1.0f, dt * 3.0f);   // lower rate = slower pulse
    const float midE = pulseEnv;

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
        if (mode == 0)   // Orb: inward-pulsing dense shell + particles spraying off the edges into a halo
        {
            const float pulse  = 1.15f - midE * 0.85f;                    // dense core shell contracts inward on the beat
            const float thick  = (fracf (p.seed * 17.3f) - 0.5f) * 0.12f; // thin layer -> dense core, not a filled ball
            const float shellR = pulse + thick;
            if (p.seed > 0.70f)   // ~30% break off the shell and scatter outward in RANDOM directions -> diffuse halo
            {
                const float spd   = 0.10f + p.energy * 0.35f + midE * 0.25f;    // fly off faster on the beat
                const float ph    = fracf (p.seed * 37.1f + t * spd);           // 0..1 travel, loops (invisible at wrap)
                const float reach = 0.6f + fracf (p.seed * 91.7f) * 1.4f;       // how far this one flies out
                // per-particle random drift direction (hashed), NOT the radial normal -> no rays from centre
                const float da  = fracf (p.seed * 51.3f) * kTwoPi;
                const float dcz = fracf (p.seed * 73.9f) * 2.0f - 1.0f;
                const float dcr = std::sqrt (juce::jmax (0.0f, 1.0f - dcz * dcz));
                const float dx  = dcr * std::cos (da) + p.sx * 0.6f;            // + slight outward bias so the halo grows outward
                const float dy  = dcr * std::sin (da) + p.sy * 0.6f;
                const float dz  = dcz + p.sz * 0.6f;
                const float dist = ph * reach;
                px3 = p.sx * shellR + dx * dist;
                py3 = p.sy * shellR + dy * dist;
                pz3 = p.sz * shellR + dz * dist;
                bright = (1.0f - ph) * (0.45f + p.energy * 0.8f);               // fade + thin out with distance
            }
            else                  // dense pulsing shell = the bright core
            {
                const float ripple = std::sin (p.seed * 9.0f + t * 1.7f) * 0.05f * (0.6f + p.energy); // organic wobble
                const float rad    = shellR + ripple;
                px3 = p.sx * rad; py3 = p.sy * rad; pz3 = p.sz * rad;
            }
        }
        else                  // Ring rhombus / Helix cube / Nebula cluster: static 3D base scaled by the pulse
        {
            // crisp geometric shapes (rhombus/cube) keep per-particle energy small so faces stay clean;
            // the random cluster can smear a little.
            const float rad = (mode == 1) ? (1.0f + midE * 0.40f + p.energy * 0.15f)   // rhombus breathes
                            : (mode == 2) ? (1.0f + midE * 0.30f + p.energy * 0.15f)   // cube breathes
                                          : (1.0f + midE * 0.35f + p.energy * 0.25f);  // random cluster
            px3 = p.sx * rad; py3 = p.sy * rad; pz3 = p.sz * rad;
        }

        // store the raw 3D position; the shader (uMV / uProj) rotates + projects with perspective
        p.x = px3; p.y = py3; p.z = pz3;
        p.disp = juce::jlimit (0.0f, 1.0f, (0.4f + p.energy) * bright);
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
    const float basePx = (f.mode == 0 ? 1.4f : f.mode == 1 ? 1.6f : f.mode == 2 ? 1.6f : 1.8f);
    verts.resize ((size_t) P * 5);
    for (int i = 0; i < P; ++i)
    {
        const auto& p = pool[(size_t) i];   // all modes are stable 3D forms -> no life-fade
        verts[(size_t) i * 5 + 0] = p.x;
        verts[(size_t) i * 5 + 1] = p.y;
        verts[(size_t) i * 5 + 2] = p.z;
        verts[(size_t) i * 5 + 3] = p.disp;
        verts[(size_t) i * 5 + 4] = basePx * (0.4f + p.disp);
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
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   // solid beads (occluding), not additive glow
    glDisable (GL_DEPTH_TEST);
    glEnable (GL_PROGRAM_POINT_SIZE);

    particleProg->use();
    const float camDist = 5.0f;
    const float yaw = (float) lastTimeS * (0.18f + f.rmsN * 0.5f);
    const float aspect = (float) fw / (float) juce::jmax (1, fh);
    const float hw = 0.7f, hh = hw / juce::jmax (0.001f, aspect);
    const auto proj = juce::Matrix3D<float>::fromFrustum (-hw, hw, -hh, hh, 2.0f, 20.0f);
    const float viewVals[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,-camDist,1 };  // column-major translate
    const auto mv = juce::Matrix3D<float> (viewVals)
                  * juce::Matrix3D<float>::rotation (juce::Vector3D<float> (0.0f, yaw, 0.0f));
    particleProg->setUniformMat4 ("uProj", proj.mat, 1, GL_FALSE);
    particleProg->setUniformMat4 ("uMV",   mv.mat,   1, GL_FALSE);
    particleProg->setUniform ("uScale", scale * ss * (float) getHeight() / 700.0f * camDist);
    particleProg->setUniform ("uColParticle", f.colP[0], f.colP[1], f.colP[2]);
    particleProg->setUniform ("uColCore", f.colC[0], f.colC[1], f.colC[2]);

    const int stride = 5 * (int) sizeof (float);
    glVertexAttribPointer ((GLuint) aPosLoc, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray ((GLuint) aPosLoc);
    glVertexAttribPointer ((GLuint) aEnergyLoc, 1, GL_FLOAT, GL_FALSE, stride, (const void*) (3 * sizeof (float)));
    glEnableVertexAttribArray ((GLuint) aEnergyLoc);
    glVertexAttribPointer ((GLuint) aSizeLoc, 1, GL_FLOAT, GL_FALSE, stride, (const void*) (4 * sizeof (float)));
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
