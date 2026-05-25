#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <vector>
#include <cmath>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <arm_neon.h>
#include <cstdlib>
#include <map>
#include <math.h>

#define GLM_FORCE_PURE
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include <EGL/egl.h>    // EGL library
#include <EGL/eglext.h> // EGL extensions
#include <glad/glad.h>  // glad library (OpenGL loader)

#include "sates.h"
#include "vec23.h"
#include "stb_image.h"
#include "colormap_png.h"

// nxlink support
//-----------------------------------------------------------------------------

#ifndef ENABLE_NXLINK
#define TRACE(fmt,...) ((void)0)
#else
#include <unistd.h>
#define TRACE(fmt,...) printf("%s: " fmt "\n", __PRETTY_FUNCTION__, ## __VA_ARGS__)

static int s_nxlinkSock = -1;

static void initNxLink()
{
    if (R_FAILED(socketInitializeDefault()))
        return;

    s_nxlinkSock = nxlinkStdio();
    if (s_nxlinkSock >= 0)
        TRACE("printf output now goes to nxlink server");
    else
        socketExit();
}

static void deinitNxLink()
{
    if (s_nxlinkSock >= 0)
    {
        close(s_nxlinkSock);
        socketExit();
        s_nxlinkSock = -1;
    }
}

extern "C" void userAppInit()
{
    initNxLink();
}

extern "C" void userAppExit()
{
    deinitNxLink();
}

#endif

//-----------------------------------------------------------------------------
// EGL initialization
//-----------------------------------------------------------------------------



static EGLDisplay s_display;
static EGLContext s_context;
static EGLSurface s_surface;

static bool initEgl(NWindow* win)
{
    // Connect to the EGL default display
    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!s_display)
    {
        TRACE("Could not connect to display! error: %d", eglGetError());
        goto _fail0;
    }

    // Initialize the EGL display connection
    eglInitialize(s_display, nullptr, nullptr);

    // Select OpenGL (Core) as the desired graphics API
    if (eglBindAPI(EGL_OPENGL_API) == EGL_FALSE)
    {
        TRACE("Could not set API! error: %d", eglGetError());
        goto _fail1;
    }

    // Get an appropriate EGL framebuffer configuration
    EGLConfig config;
    EGLint numConfigs;
    static const EGLint framebufferAttributeList[] =
    {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,     8,
        EGL_GREEN_SIZE,   8,
        EGL_BLUE_SIZE,    8,
        EGL_ALPHA_SIZE,   8,
        EGL_DEPTH_SIZE,   24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    eglChooseConfig(s_display, framebufferAttributeList, &config, 1, &numConfigs);
    if (numConfigs == 0)
    {
        TRACE("No config found! error: %d", eglGetError());
        goto _fail1;
    }

    // Create an EGL window surface
    s_surface = eglCreateWindowSurface(s_display, config, win, nullptr);
    if (!s_surface)
    {
        TRACE("Surface creation failed! error: %d", eglGetError());
        goto _fail1;
    }

    // Create an EGL rendering context
    static const EGLint contextAttributeList[] =
    {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
        EGL_CONTEXT_MINOR_VERSION_KHR, 3,
        EGL_NONE
    };
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, contextAttributeList);
    if (!s_context)
    {
        TRACE("Context creation failed! error: %d", eglGetError());
        goto _fail2;
    }

    // Connect the context to the surface
    eglMakeCurrent(s_display, s_surface, s_surface, s_context);
    return true;

_fail2:
    eglDestroySurface(s_display, s_surface);
    s_surface = nullptr;
_fail1:
    eglTerminate(s_display);
    s_display = nullptr;
_fail0:
    return false;
}

static void deinitEgl()
{
    if (s_display)
    {
        eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (s_context)
        {
            eglDestroyContext(s_display, s_context);
            s_context = nullptr;
        }
        if (s_surface)
        {
            eglDestroySurface(s_display, s_surface);
            s_surface = nullptr;
        }
        eglTerminate(s_display);
        s_display = nullptr;
    }
}

//-----------------------------------------------------------------------------
// FPS Counter
//-----------------------------------------------------------------------------

// Shaders for text rendering
static const char* const text_vs = R"text(
    #version 330 core
    layout(location=0) in vec2 inPos;
    layout(location=1) in vec3 inColor;
    out vec3 color;
    void main() {
        color = inColor;
        gl_Position = vec4(inPos, 0.0, 1.0);
    }
)text";

static const char* const text_fs = R"text(
    #version 330 core
    in vec3 color;
    out vec4 fragColor;
    void main() {
        fragColor = vec4(color, 1.0);
    }
)text";

static GLuint s_textProgram = 0;
static GLuint s_textVao = 0;
static GLuint s_textVbo = 0;

static float* s_textVertexData = nullptr;
static const int TEXT_BUFFER_FLOATS = 100 * 64 * 6 * 5;

static const unsigned char font8x8[11][8] = {
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 0
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 1
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 2
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 3
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 4
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 5
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 6
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 7
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 8
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 9
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}  // .
};

static GLuint createAndCompileShader(GLenum type, const char* source);

static void initTextRenderer() {
    GLuint vsh = createAndCompileShader(GL_VERTEX_SHADER, text_vs);
    GLuint fsh = createAndCompileShader(GL_FRAGMENT_SHADER, text_fs);
    
    s_textProgram = glCreateProgram();
    glAttachShader(s_textProgram, vsh);
    glAttachShader(s_textProgram, fsh);
    glLinkProgram(s_textProgram);
    glDeleteShader(vsh);
    glDeleteShader(fsh);
    
    glGenVertexArrays(1, &s_textVao);
    glGenBuffers(1, &s_textVbo);

    s_textVertexData = (float*)aligned_alloc(64,
    TEXT_BUFFER_FLOATS * sizeof(float));
}

static void drawTextPixel(float x, float y, float size, float r, float g, float b, float* vertexData, int* offset) {
    float verts[] = {
        x, y, r, g, b,
        x+size, y, r, g, b,
        x+size, y+size, r, g, b,
        x, y, r, g, b,
        x+size, y+size, r, g, b,
        x, y+size, r, g, b
    };
    memcpy(&vertexData[*offset], verts, sizeof(verts));
    *offset += 30;
}

static void drawChar(char c, float x, float y, float scale, float r, float g, float b, float* vertexData, int* offset) {
    int idx = -1;
    if(c >= '0' && c <= '9') idx = c - '0';
    else if(c == '.') idx = 10;
    else return;
    
    const unsigned char* glyph = font8x8[idx];
    for(int row = 0; row < 8; row++) {
        for(int col = 0; col < 8; col++) {
            if(glyph[row] & (1 << col)) {
                float px = x + col * scale;
                float py = y - row * scale;
                drawTextPixel(px, py, scale, r, g, b, vertexData, offset);
            }
        }
    }
}

static void drawText(const char* text, float x, float y, float scale, float r, float g, float b) {
    float* vertexData = s_textVertexData;
    int offset = 0;
    float cx = x;
    
    while(*text) {
        drawChar(*text, cx, y, scale, r, g, b, vertexData, &offset);
        cx += 8 * scale;
        text++;
    }
    
    if(offset > 0) {
        glUseProgram(s_textProgram);
        glBindVertexArray(s_textVao);
        glBindBuffer(GL_ARRAY_BUFFER, s_textVbo);
        glBufferData(GL_ARRAY_BUFFER, offset * sizeof(float), vertexData, GL_DYNAMIC_DRAW);
        
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(2*sizeof(float)));
        glEnableVertexAttribArray(1);
        
        glDrawArrays(GL_TRIANGLES, 0, offset / 5);
    }
    
}

static void cleanupTextRenderer() {
    if(s_textVbo) {
        glDeleteBuffers(1, &s_textVbo);
        s_textVbo = 0;
    }
    if(s_textVao) {
        glDeleteVertexArrays(1, &s_textVao);
        s_textVao = 0;
    }
    if(s_textProgram) {
        glDeleteProgram(s_textProgram);
        s_textProgram = 0;
    }
    if(s_textVertexData)
    {
        free(s_textVertexData);
        s_textVertexData = nullptr;
    }
}

static void setMesaConfig()
{
    // Uncomment below to disable error checking and save CPU time (useful for production):
    setenv("MESA_NO_ERROR", "1", 1);

    // Uncomment below to enable Mesa logging:
    setenv("EGL_LOG_LEVEL", "debug", 1);
    setenv("MESA_VERBOSE", "all", 1);
    // setenv("NOUVEAU_MESA_DEBUG", "1", 1);

    // Uncomment below to enable shader debugging in Nouveau:
    // setenv("NV50_PROG_OPTIMIZE", "0", 1);
    // setenv("NV50_PROG_DEBUG", "1", 1);
    // setenv("NV50_PROG_CHIPSET", "0x120", 1);
}

// FPS counter variables
static u64 s_startTicks = 0;
static u64 s_lastFrameTime = 0;
static float s_fps = 0.0f;
static int s_frameCount = 0;
static u64 s_fpsUpdateTime = 0;

static int frame = 0;



static const char* const rt_vs = R"text(
    #version 330 core
    out vec2 uv;

    void main()
    {
        vec2 pos = vec2(
            (gl_VertexID == 2) ? 3.0 : -1.0,
            (gl_VertexID == 1) ? 3.0 : -1.0
        );

        uv = 0.5 * (pos + 1.0);
        gl_Position = vec4(pos, 0.0, 1.0);
    }
)text";

static const char* const rt_fs = R"text(
#version 330 core

in vec2 uv; 
out vec4 fragColor;

uniform sampler2D screenTex;

void main(){
    fragColor = texture(screenTex, uv);
}
)text";

static GLuint s_program;
static GLuint s_vao, s_vbo;
// needed for shader to render onscreen
static GLint resolutionLoc;

static GLint loc_mdlvMtx, loc_projMtx;
static GLint loc_time;


static GLuint createAndCompileShader(GLenum type, const char* source)
{
    GLint success;
    GLchar msg[4096];

    GLuint handle = glCreateShader(type);
    if (!handle)
    {
        TRACE("%u: cannot create shader", type);
        return 0;
    }
    glShaderSource(handle, 1, &source, nullptr);
    glCompileShader(handle);
    glGetShaderiv(handle, GL_COMPILE_STATUS, &success);

    if (!success)
    {
        glGetShaderInfoLog(handle, sizeof(msg), nullptr, msg);
        TRACE("%u: %s\n", type, msg);
        glDeleteShader(handle);
        return 0;
    }

    return handle;
}

// Alright boys, time for hell
// I love space to bits, but actually making this is going to be a mess
// I am absolutely in for it though.
// The plan is this, GPU will render the Black Hole's rays and the accretion disk
// The CPU will in turn pre compute as much as possible on two threads, while the 3rd will be used for tone mapping and post processing
// If that falls through, a shit verison of FSR might be what I end up making

// Rendering Res
static int RenderX = 1280;
static int RenderY = 720;

/// Output Res
static int OutputX = 1280;
static int OutputY = 720;

// Vars for multithreading
static const int THREAD_COUNT = 2;
static std::thread workers[THREAD_COUNT];
alignas(64) static std::atomic<bool> running(true);
alignas(64) static std::atomic<int> tilesDone(0);

static std::mutex workMutex;
static std::condition_variable workCV;

alignas(64) static bool workReady = false;
alignas(64) static int currentFrame = 0;


//Pin threads to cores so the OS doesn't throw them around everywhere
static void pinThread(int core)
{
    Handle thread = CUR_THREAD_HANDLE;
    svcSetThreadCoreMask(thread, core, 1ULL << core);
}

// Without further ado
// Big bertha time

static const char* const vertexShaderSource = R"text(
    #version 330 core

    layout(location = 0) in vec3 position;

    out vec2 uv;

    void main() {
    uv = (position.xy + 1.0) * 0.5;
    gl_Position = vec4(position, 1.0);
    }
)text";

// Fun time
// The actual black hole shader
// Credit to rossning92's Github project of this
// https://github.com/rossning92/Blackhole/blob/master/shader/blackhole_main.frag
// Tuned for NX
// Without further ado, melting time

static const char* const fragmentShaderSource = R"text(
    #version 330 core
    

    // Constants
    const float PI = 3.14159265359;
    const float EPSILON = 0.0001;
    const float INFINITY = 1000000.0;

    out vec4 fragColor;

    uniform vec2 res;
    uniform float time;
    // textures
    uniform samplerCube galaxy;
    uniform sampler2D colorMap;

    // Rendering params
    uniform float frontView = 0.0;
    uniform float topView = 0.0;
    uniform float cameraRoll = 0.0;

    // checkerboard
    uniform int frameIndex;
    uniform sampler3D noiseTex;

    // Phys params
    uniform float fovScale = 1.0;

    // Accretion disk
    uniform float adiskParticle = 1.0;
    uniform float adiskHeight = 0.1;
    uniform float adiskLit = 0.5;
    uniform float adiskDensityV = 2.0;
    uniform float adiskDensityH = 1.5;
    uniform float adiskNoiseScale = 0.4;
    uniform float adiskNoiseLOD = 5.0;
    uniform float adiskSpeed = 0.12;

    struct Ring {
        vec3 center;
        vec3 normal;
        float innerRadius;
        float outerRadius;
        float rotateSpeed;
    };

    // Simplex 3D Noise
    // by Ian McEwan, Ashima Arts

    vec4 permute(vec4 x) { return mod(((x * 34.0) + 1.0) * x, 289.0); }
    vec4 taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }

    float snoise(vec3 v) {
        const vec2 C = vec2(1.0 / 6.0, 1.0 / 3.0);
        const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);
    
        // 1st corner
        vec3 i = floor(v + dot(v, C.yyy));
        vec3 x0 = v - i + dot(i, C.xxx);

        // Other corners
        vec3 g = step(x0.yzx, x0.xyz);
        vec3 l = 1.0 - g;
        vec3 i1 = min(g.xyz, l.zxy);
        vec3 i2 = max(g.xyz, l.zxy);

        // x0 = x0 - 0. + 0.0 * C
        vec3 x1 = x0 - i1 + 1.0 * C.xxx;
        vec3 x2 = x0 - i2 + 2.0 * C.xxx;
        vec3 x3 = x0 - 1. + 3.0 * C.xxx;

        // Permutations
        i = mod(i, 289.0);
        vec4 p = permute(permute(permute(i.z + vec4(0.0, i1.z, i2.z, 1.0)) + i.y + vec4(0.0, i1.y, i2.y, 1.0)) + i.x + vec4(0.0, i1.x, i2.x, 1.0));

        // Gradients
        // N * N points uniformly over a square, mapped to an octahedron
        float n_ = 1.0 / 7.0; // N=7
        vec3 ns = n_ * D.wyz - D.xzx;

        vec4 j = p - 49.0 * floor(p * ns.z * ns.z); //  mod(p,N*N)

        vec4 x_ = floor(j * ns.z);
        vec4 y_ = floor(j - 7.0 * x_); // mod(j,N)

        vec4 x = x_ * ns.x + ns.yyyy;
        vec4 y = y_ * ns.x + ns.yyyy;
        vec4 h = 1.0 - abs(x) - abs(y);

        vec4 b0 = vec4(x.xy, y.xy);
        vec4 b1 = vec4(x.zw, y.zw);

        vec4 s0 = floor(b0) * 2.0 + 1.0;
        vec4 s1 = floor(b1) * 2.0 + 1.0;
        vec4 sh = -step(h, vec4(0.0));

        vec4 a0 = b0.xzyw + s0.xzyw * sh.xxyy;
        vec4 a1 = b1.xzyw + s1.xzyw * sh.zzww;

        vec3 p0 = vec3(a0.xy, h.x);
        vec3 p1 = vec3(a0.zw, h.y);
        vec3 p2 = vec3(a1.xy, h.z);
        vec3 p3 = vec3(a1.zw, h.w);

        // Normalize Gradients
        vec4 norm =
            taylorInvSqrt(vec4(dot(p0, p0), dot(p1, p1), dot(p2, p2), dot(p3, p3)));
        p0 *= norm.x;
        p1 *= norm.y;
        p2 *= norm.z;
        p3 *= norm.w;

        // Mix final noise val
        vec4 m = max(0.6 - vec4(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0);
        m = m * m;
    return 42.0 * dot(m * m, vec4(dot(p0, x0), dot(p1, x1), dot(p2, x2), dot(p3, x3)));
    }

    // Ring computes
    
    float ringDistance(vec3 rayOrigin, vec3 rayDir, Ring ring){
        float denominator = dot(rayDir, ring.normal);
        float constant = -dot(ring.center, ring.normal);
        if (abs(denominator) < EPSILON) {
            return -1.0;
        } else {
            float t = -(dot(rayOrigin, ring.normal) + constant) / denominator;
            if (t < 0.0) {
                return -1.0;
            }

            vec3 intersection = rayOrigin + t * rayDir;

            // Comp distance to ring center
            float d = length(intersection - ring.center);
            if (d >= ring.innerRadius && d <= ring.outerRadius) {
                return t;
            }
            return -1.0;
        }
    }

    vec3 panoramaColor (sampler2D tex, vec3 dir) {
        vec2 uv = vec2(0.5 - atan(dir.z, dir.x) / PI * 0.5, 0.5 - asin(dir.y) / PI);
        return texture2D(tex, uv).rgb;
    }

    vec3 accel(float h2, vec3 pos) {
        float r2 = dot(pos, pos);
        float r5 = pow(r2, 2.5);
        vec3 acc = -1.5 * h2 * pos / r5 * 1.0;
        return acc;
    }

    vec4 quadFromAxisAngle(vec3 axis, float angle) {
        vec4 qr;
        float half_angle = (angle * 0.5) * 3.14159 / 180.0;
        qr.x = axis.x * sin(half_angle);
        qr.y = axis.y * sin(half_angle);
        qr.z = axis.z * sin(half_angle);
        qr.w = cos(half_angle);
        return qr;
    }

    vec4 quadConj(vec4 q) { return vec4(-q.x, -q.y, -q.z, q.w); }

    vec4 quat_mult(vec4 q1, vec4 q2) {
        vec4 qr;
        qr.x = (q1.w * q2.x) + (q1.x * q2.w) + (q1.y * q2.z) - (q1.z * q2.y);
        qr.y = (q1.w * q2.y) - (q1.x * q2.z) + (q1.y * q2.w) + (q1.z * q2.x);
        qr.z = (q1.w * q2.z) + (q1.x * q2.y) - (q1.y * q2.x) + (q1.z * q2.w);
        qr.w = (q1.w * q2.w) - (q1.x * q2.x) - (q1.y * q2.y) - (q1.z * q2.z);
        return qr;
    }

    vec3 rotateVector(vec3 position, vec3 axis, float angle) {
        vec4 qr = quadFromAxisAngle(axis, angle);
        vec4 qr_conj = quadConj(qr);
        vec4 q_pos = vec4(position.x, position.y, position.z, 0);

        vec4 q_tmp = quat_mult(qr, q_pos);
        qr = quat_mult(q_tmp, qr_conj);

        return vec3(qr.x, qr.y, qr.z);
    }

    #define IN_RANGE(x, a, b) (((x) > (a)) && ((x) < (b)))

    void cartesianToSpherical(in vec3 xyz, out float rho, out float phi, out float theta) {
        rho = sqrt((xyz.x * xyz.x) + (xyz.y * xyz.y) + (xyz.z * xyz.z));
        phi = asin(xyz.y / rho);
        theta = atan(xyz.z, xyz.x);
    }

    // Cartesian -> Spherical coord
    vec3 toSpherical(vec3 p) {
        float rho = sqrt((p.x * p.x) + (p.y * p.y) + (p.z * p.z));
        float theta = atan(p.z, p.x);
        float phi = asin(p.y / rho);
        return vec3(rho, theta, phi);
    }

    vec3 toSpherical2(vec3 pos) {
        vec3 radialCoords;
        radialCoords.x = length(pos) * 1.5 + 0.55;
        radialCoords.y = atan(-pos.x, -pos.z) * 1.5;
        radialCoords.z = abs(pos.y);
        return radialCoords;
    }

    //  ring color
    void ringColor(vec3 rayOrigin, vec3 rayDir, Ring ring, inout float minDistance, inout vec3 color) {
        float distance = ringDistance(rayOrigin, normalize(rayDir), ring);
        if (distance >= EPSILON && distance < minDistance && distance <= length(rayDir) + EPSILON) {
                minDistance = distance;

                vec3 intersection = rayOrigin + normalize(rayDir) * minDistance;
                vec3 ringColor;
            
            {
                float dist = length(intersection);
                
                float v = clamp((dist - ring.innerRadius) / (ring.outerRadius - ring.innerRadius), 0.0, 1.0);
                
                vec3 base = cross(ring.normal, vec3(0.0, 0.0, 1.0));
                float angle = acos(dot(normalize(base), normalize(intersection)));

                if (dot(cross(base, intersection), ring.normal) < 0.0)
                    angle = -angle;
                
                float u = 0.5 - 0.5 * angle / PI;
                //.Hack
                u += time * ring.rotateSpeed;

                vec3 color = vec3(0.0, 0.5, 0.0);
                //.Hack GU
                float alpha = 0.5;
                ringColor = vec3(color);
            }
            color += ringColor;
        }
    }

    mat3 lookAt(vec3 origin, vec3 target, float roll){
        vec3 rr = vec3(sin(roll), cos(roll), 0.0);
        vec3 ww = normalize(target - origin);
        vec3 uu = normalize(cross(ww, rr));
        vec3 vv = normalize(cross(uu, ww));

        return mat3(uu, vv, ww);
    }

    float sqrLength(vec3 a) { return dot(a, a); }

    // Accretion Disc
    // for laughs, I tried running this bullshit on my PC with a 5060, at 1080p this thing was dying
    // I am terrified at how its going to run on NX
    void adiskColor(vec3 pos, inout vec3 color, inout float alpha) {
        float innerRadius = 2.6;
        float outerRadius = 12.0;

        //Accertion disks increase in density the closer we get to the event horizon
        float density = max(0.0, 1.0 - length(pos.xyz / vec3(outerRadius, adiskHeight, outerRadius)));
        if (density < 0.001) {
            return;
        }

        density *= pow(1.0 - abs(pos.y) / adiskHeight, adiskDensityV * 4.0);

        // Set particles to 0 once we go past the innermost circular orbit

        density *= smoothstep(outerRadius, outerRadius * 0.85, length(pos));

        // Dont compute if the density is tiny
        if (density < 0.003) {
            return;
        }

        vec3 sphericalCoord = toSpherical(pos);

        // Scale rho/phi so particles appear at the correct scale
        sphericalCoord.y *= 18.0;
        sphericalCoord.z *= 0.4;
        sphericalCoord.x *= 0.6;

        if (adiskParticle < 0.5){
            color += vec3(0.0, 1.0, 0.0) * density * 0.02;
            return;
        }

        float noise = 1.0;
        float amp = 1.0;
        float freq = 1.0;
        #define NOISE_LOD 5
        for (int i = 0; i < NOISE_LOD; i++) {
            float r = length(pos.xz);
            float theta = atan(pos.z, pos.x);
            float orbitalSpeed = 2.0 / pow(max(r, 0.2), 1.5);

            float turbulence = texture(noiseTex, pos * 0.08).r;
            theta += turbulence * 0.4;

            vec3 flowCoord = vec3(theta * 2.5, pos.y * 1.5, log(r + 1.0) * 2.0);

            flowCoord.x += time * orbitalSpeed;

            vec3 warp = texture(noiseTex, flowCoord * 0.15).rgb;
            warp = warp * 2.0 - 1.0;
            flowCoord += warp * 0.25;

            float n = texture(noiseTex, fract(flowCoord)).r;
            n = n * 2.0 - 1.0;

            noise += n * amp;

            freq *= 2.0;
            amp *= 0.5;
        }
        noise = abs(noise);

        float radialT = clamp((length(pos.xz) - innerRadius) / (outerRadius - innerRadius), 0.0, 1.0);
        
        // accretion disk colors, based on their distance
        vec3 innerColor = vec3(1.0, 0.85, 0.5);
        vec3 midColor = vec3(0.9, 0.35, 0.05);
        vec3 outerColor = vec3(0.3, 0.05, 0.02);

        vec3 dustColor;
        if (radialT < 0.3) {
            dustColor = mix(innerColor, midColor, radialT / 0.3);
        } else {
            dustColor = mix(midColor, outerColor, (radialT - 0.3) / 0.7);
        }

        dustColor *= 1.0 + 4.0 * pow(1.0 - radialT, 3.0);

        color += density * adiskLit * dustColor * alpha * abs(noise);
    }

    
    vec3 traceColor(vec3 pos, vec3 dir) {
        vec3 color = vec3(0.0);
        float alpha = 1.0;

        dir = normalize(dir);

        //Initial val
        vec3 h = cross(pos, dir);
        float h2 = dot(h, h);

        // Ray iterations count
        for (int i = 0; i < 250; i++) {
            // Dynamically change steps taken based on distance from the black hole
            float dist = length(pos);
            float stepSize = clamp(dist * 0.04, 0.02, 0.3);

            // scale lensing by step size
            // Original didn't, and while it worked, dynamic changes break it
            vec3 acc = accel(h2, pos) * stepSize;
            dir += acc;
            h = cross(pos, dir);
            h2 = dot(h, h);

            if (dot(pos, pos) < 1.0) return color;

            adiskColor(pos, color, alpha);
            
            pos += dir * stepSize;
        }

        dir = rotateVector(dir, vec3(0.0, 1.0, 0.0), time);
        color += textureLod(galaxy, dir, 0.0).rgb * alpha;
        return color;
    }

    // COUGH COUGH WHAT THE FUCK AHH
    uniform vec3 camPos;
    uniform mat3 view;

    void main() {
        ivec2 coord = ivec2(gl_FragCoord.xy);
        // Checkerboard rendering, ie skip pixels if they belong to another frame
        if ((coord.x + coord.y + frameIndex) % 2 != 0) {
            discard;
        }

        vec2 uv = gl_FragCoord.xy / res - vec2(0.5);
        uv.x *= res.x / res.y;
        vec3 dir = view * normalize(vec3(-uv.x * fovScale, uv.y * fovScale, 1.0));
        fragColor = vec4(traceColor(camPos, dir), 1.0);
    }
)text";

// Bloom shaders
// Extracts bright areas and downsamples to .25x res
static const char* const bloom_extract_fs = R"text(
    #version 330 core
    in vec2 uv;
    out vec4 fragColor;
    uniform sampler2D sceneTex;
    uniform float threshold;

    void main() {
        vec3 color = texture(sceneTex, uv).rgb;
        // Luminance val
        float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
        // Softer fall off instead of hard edges
        float contrib = smoothstep(threshold, threshold + 0.3, brightness);
        fragColor = vec4(color * contrib, 1.0);
    }
)text";

// Gaussian blur, runs once on X/Y
static const char* const bloom_blur_fs = R"text(
    #version 330 core
    in vec2 uv;
    out vec4 fragColor;
    uniform sampler2D blurTex;
    uniform vec2 direction;   // (1,0) or (0,1)
    uniform vec2 texelSize;   // 1.0 / vec2(BLOOM_W, BLOOM_H)

    // 9-tap Gaussian weights
    const float weight[5] = float[](0.227027, 0.194595, 0.121622, 0.054054, 0.016216);

    void main() {
        vec3 result = texture(blurTex, uv).rgb * weight[0];
        vec2 step = direction * texelSize;
        for (int i = 1; i < 5; i++) {
            result += texture(blurTex, uv + step * float(i)).rgb * weight[i];
            result += texture(blurTex, uv - step * float(i)).rgb * weight[i];
        }
        fragColor = vec4(result, 1.0);
    }
)text";

// Adds blur on top of the final scene
static const char* const bloom_composite_fs = R"text(
    #version 330 core
    in vec2 uv;
    out vec4 fragColor;
    uniform sampler2D sceneTex;
    uniform sampler2D bloomTex;
    uniform float bloomStrength;

    void main() {
        vec3 scene = texture(sceneTex, uv).rgb;
        vec3 bloom  = texture(bloomTex, uv).rgb;

        //Make sure that bloom never darkens
        vec3 result = scene + bloom * bloomStrength;

        // Make sure that the bloom doesn't snap to white
        result = vec3(1.0) - exp(-result * 1.2);

        fragColor = vec4(result, 1.0);
    }
)text";

static const char* const checkerboard_resolve_fs = R"text(
    #version 330 core
    in vec2 uv;
    out vec4 fragColor;

    uniform sampler2D currentTex;
    uniform sampler2D previousTex;
    uniform int frameIndex;
    uniform vec2 texelSize;

    void main() {
        ivec2 coord = ivec2(gl_FragCoord.xy);
        bool thisFramePixel = ((coord.x + coord.y + frameIndex) % 2 == 0);

        if (thisFramePixel) {
            // ie, if pixel X is written, use it directly
            fragColor = texture(currentTex, uv);
        } else {
        // Reconstruct from the previous frame with a 4 average for smoothing
        // Ie, if this pixel was skipped use data from 4 average
        vec3 c = texture(previousTex, uv).rgb;
        c += texture(previousTex, uv + vec2( texelSize.x, 0.0)).rgb;
        c += texture(previousTex, uv + vec2(-texelSize.x, 0.0)).rgb;
        c += texture(previousTex, uv + vec2(0.0,  texelSize.y)).rgb;
        fragColor = vec4(c * 0.25, 1.0);
        }
    }
)text";


static GLint res;
static GLuint tex1;
static GLuint tex2;
static GLint resloc;

static GLint loc_camPos;
static GLint loc_view;

// I find you facinating
static GLuint s_bloomExtractProg;
static GLuint s_bloomBlurProg;
static GLuint s_bloomCompositeProg;

static GLuint s_sceneFbo,   s_sceneTex;
static GLuint s_bloomFboA,  s_bloomTexA;
static GLuint s_bloomFboB,  s_bloomTexB;

// Relax, its over
// Direction/texel sizes 
static GLint  s_blur_dirLoc;
static GLint  s_blur_texelLoc;
static GLint  s_composite_bloomStrengthLoc;

// Lay you down to sleep
// Quarter res bloom because I'm lazy
static const int BLOOM_W = 320;
static const int BLOOM_H = 180;

// Checkerboard rendering
static GLuint s_prevSceneFbo;
static GLuint s_prevSceneTex;
static GLuint s_resolveProg;

static GLint  s_resolve_frameIndexLoc;
static GLint  s_resolve_texelSizeLoc;
static GLint  s_rt_frameIndexLoc;
static int    s_frameIndex = 0;

// noise texture
static GLuint s_noiseTex3D;
static GLint s_noiseTexLoc;

// Ah fuck me the insanity starts 
// I could just keep noise in the shader, but fuck me I want to melt shit today, so here we go
float snoise_cpu(float v_x, float v_y, float v_z)
{
    pinThread(1);
    // Constants
    const float C_x = 1.0f / 6.0f;
    const float C_y = 1.0f / 3.0f;

    // 1st corner
    float dot_v_Cyyy = (v_x + v_y + v_z) * C_y;
    float i_x = std::floor(v_x + dot_v_Cyyy);
    float i_y = std::floor(v_y + dot_v_Cyyy);
    float i_z = std::floor(v_z + dot_v_Cyyy);

    float dot_i_Cxxx = (i_x + i_y + i_z) * C_x;
    float x0_x = v_x - i_x + dot_i_Cxxx;
    float x0_y = v_y - i_y + dot_i_Cxxx;
    float x0_z = v_z - i_z + dot_i_Cxxx;

    // Other corners
    float g_x = (x0_y >= x0_x) ? 1.0f : 0.0f;
    float g_y = (x0_z >= x0_y) ? 1.0f : 0.0f;
    float g_z = (x0_x >= x0_z) ? 1.0f : 0.0f;

    float l_x = 1.0f - g_x;
    float l_y = 1.0f - g_y;
    float l_z = 1.0f - g_z;

    float i1_x = std::min(g_x, l_z);
    float i1_y = std::min(g_y, l_x);
    float i1_z = std::min(g_z, l_y);

    float i2_x = std::max(g_x, l_z);
    float i2_y = std::max(g_y, l_x);
    float i2_z = std::max(g_z, l_y);

    // Vectorize across the 4 corners: [corner0, corner1, corner2, corner3]
    float32x4_t i1 = {0.0f, i1_x, i2_x, 1.0f};
    float32x4_t i2 = {0.0f, i1_y, i2_y, 1.0f};
    float32x4_t i3 = {0.0f, i1_z, i2_z, 1.0f};

    // Constant vectors
    float32x4_t vC_xxx = vdupq_n_f32(C_x);
    float32x4_t v_zero = vdupq_n_f32(0.0f);
    float32x4_t v_one  = vdupq_n_f32(1.0f);

    // x1, x2, x3 offsets
    float32x4_t c_mult = {0.0f, 1.0f, 2.0f, 3.0f};
    float32x4_t c_offset = vmulq_f32(c_mult, vC_xxx);

    // Positions relative to all 4 corners (Transposed Layout)
    float32x4_t dx = vsubq_f32(vdupq_n_f32(x0_x), i1);
    float32x4_t dy = vsubq_f32(vdupq_n_f32(x0_y), i2);
    float32x4_t dz = vsubq_f32(vdupq_n_f32(x0_z), i3);

    dx = vaddq_f32(dx, c_offset);
    dy = vaddq_f32(dy, c_offset);
    dz = vaddq_f32(dz, c_offset);

    // Permutations
    // mod(i, 289.0)
    float i_mod_x = i_x - std::floor(i_x * (1.0f / 289.0f)) * 289.0f;
    float i_mod_y = i_y - std::floor(i_y * (1.0f / 289.0f)) * 289.0f;
    float i_mod_z = i_z - std::floor(i_z * (1.0f / 289.0f)) * 289.0f;

    float32x4_t p_z = vaddq_f32(vdupq_n_f32(i_mod_z), i3);
    float32x4_t p_y = vaddq_f32(vdupq_n_f32(i_mod_y), i2);
    float32x4_t p_x = vaddq_f32(vdupq_n_f32(i_mod_x), i1);

    float32x4_t p = vpermute(vaddq_f32(vpermute(vaddq_f32(vpermute(p_z), p_y)), p_x));

    // Gradients
    float ns_z = 1.0f / 7.0f;
    float ns_x = ns_z * 2.0f;
    float32x4_t vns_z = vdupq_n_f32(ns_z);
    float32x4_t vns_x = vdupq_n_f32(ns_x);

    // j = p - 49.0 * floor(p * ns.z * ns.z)
    float32x4_t j_floor = vrndmq_f32(vmulq_f32(p, vdupq_n_f32(ns_z * ns_z)));
    float32x4_t j = vmlsq_f32(p, j_floor, vdupq_n_f32(49.0f));

    float32x4_t x_ = vrndmq_f32(vmulq_f32(j, vns_z));
    float32x4_t y_ = vrndmq_f32(vmlsq_f32(j, x_, vdupq_n_f32(7.0f)));

    float32x4_t gx = vmlaq_f32(vdupq_n_f32(-1.0f), x_, vns_x);
    float32x4_t gy = vmlaq_f32(vdupq_n_f32(-1.0f), y_, vns_x);
    
    // h = 1.0 - abs(x) - abs(y)
    float32x4_t h = vsubq_f32(vsubq_f32(v_one, vabsq_f32(gx)), vabsq_f32(gy));

    // s = floor(b) * 2.0 + 1.0
    float32x4_t b0_floor = vrndmq_f32(gx);
    float32x4_t b1_floor = vrndmq_f32(gy);
    float32x4_t s0 = vmlaq_f32(v_one, b0_floor, vdupq_n_f32(2.0f));
    float32x4_t s1 = vmlaq_f32(v_one, b1_floor, vdupq_n_f32(2.0f));

    // sh = -step(h, 0.0) -> if h < 0.0 then -1.0 else 0.0
    // vccltq_f32 yields mask (0xFFFFFFFF if true, 0 if false)
    uint32x4_t h_lt_zero = vcltq_f32(h, v_zero);
    float32x4_t sh = vbslq_f32(h_lt_zero, vdupq_n_f32(-1.0f), v_zero);

    // a = b + s * sh
    float32x4_t ax = vmlaq_f32(gx, s0, sh);
    float32x4_t ay = vmlaq_f32(gy, s1, sh);

    // Gradients p0, p1, p2, p3 now sit sequentially in ax, ay, h registers
    // Normalize gradients: taylorInvSqrt(dot(p, p))
    float32x4_t dot_p = vmulq_f32(ax, ax);
    dot_p = vmlaq_f32(dot_p, ay, ay);
    dot_p = vmlaq_f32(dot_p, h, h);

    float32x4_t norm = vtaylorInvSqrt(dot_p);
    ax = vmulq_f32(ax, norm);
    ay = vmulq_f32(ay, norm);
    h  = vmulq_f32(h, norm);

    // Mix final noise val
    // m = max(0.6 - dot(dx, dx)... , 0.0)
    float32x4_t dot_dx = vmulq_f32(dx, dx);
    dot_dx = vmlaq_f32(dot_dx, dy, dy);
    dot_dx = vmlaq_f32(dot_dx, dz, dz);

    float32x4_t m = vmaxq_f32(vsubq_f32(vdupq_n_f32(0.6f), dot_dx), v_zero);
    m = vmulq_f32(m, m); // m^2
    float32x4_t m4 = vmulq_f32(m, m); // m^4

    // dot(p, dx)
    float32x4_t dot_p_dx = vmulq_f32(ax, dx);
    dot_p_dx = vmlaq_f32(dot_p_dx, ay, dy);
    dot_p_dx = vmlaq_f32(dot_p_dx, h, dz);

    // Final combination: 42.0 * dot(m^4, dot_p_dx)
    float32x4_t final_vec = vmulq_f32(m4, dot_p_dx);
    
    // Horizontal addition of the 4 lanes
    float final_sum = vaddvq_f32(final_vec); 

    return 42.0f * final_sum;
}

// Make a 64^3 texture for the accretion disk
static void buildNoise3()
{
    const int N = 64;
    std::vector<uint8_t> data(N * N * N);
    for (int z = 0; z < N; z++)
    for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
        float fx = float(x) / float(N);
        float fy = float(y) / float(N);
        float fz = float(z) / float(N);

        float v = snoise_cpu(fx * 8.0f, fy * 8.0f, fz * 8.0f);
        data[z * N * N + y * N + x] = (uint8_t)((v * 0.5f + 0.5f) * 255.0f);
    }

    glGenTextures(1, &s_noiseTex3D);
    glBindTexture(GL_TEXTURE_3D, s_noiseTex3D);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, N, N, N, 0, GL_RED, GL_UNSIGNED_BYTE, data.data());
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);

}

// Create bloom textures
static void makeFbo(GLuint& fbo, GLuint& tex, int w, int h)
{
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R11F_G11F_B10F, w, h, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Man we are FUCKED

void BHRTSceneInit()
{
    GLint vsh = createAndCompileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLint fsh = createAndCompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);

    s_program = glCreateProgram();
    glAttachShader(s_program, vsh);
    glAttachShader(s_program, fsh);
    glLinkProgram(s_program);
    GLuint tex1loc = glGetUniformLocation(s_program, "galaxy");
    GLuint tex2loc = glGetUniformLocation(s_program, "colorMap");
    loc_time = glGetUniformLocation(s_program, "time");
    resloc = glGetUniformLocation(s_program, "res");
    s_noiseTexLoc = glGetUniformLocation(s_program, "noiseTex");
    buildNoise3();

    // Can you tell most of this is a rehash of old bullshit
    GLint success;
    glGetProgramiv(s_program, GL_LINK_STATUS, &success);
    if (!success)
    {
        char buf[512];
        glGetProgramInfoLog(s_program, sizeof(buf), nullptr, buf);
        TRACE("Link error: %s", buf);
    }
    glDeleteShader(vsh);
    glDeleteShader(fsh);

    float verts[] = { -1,-1,  1,-1,  1,1,  -1,-1,  1,1,  -1,1 };
    glGenVertexArrays(1, &s_vao); glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);      glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);

    int width, height, nchan;
    stbi_set_flip_vertically_on_load(true);

    // Make a dummy cube map for testing
    glGenTextures(1, &tex1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex1);
    unsigned char px[3] = {0, 0, 0}; // Black test
    for (int f = 0; f < 6; f++){
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    // Dummy 2D color (1x1 orange)


    glGenTextures(1, &tex2);
    glBindTexture(GL_TEXTURE_2D, tex2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    stbi_uc* img = stbi_load_from_memory((const stbi_uc*)colormap_png, colormap_png_size, &width, &height, &nchan, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    stbi_image_free(img);


    glUseProgram(s_program);

    loc_camPos = glGetUniformLocation(s_program, "camPos");
    loc_view = glGetUniformLocation(s_program, "view");

    glUniform1i(tex2loc, 1);



    auto projMtx = glm::perspective(
        glm::radians(40.0f),
        1280.0f / 720.0f,
        0.01f,
        1000.0f
    );
    
    s_startTicks = armGetSystemTick();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex1);
    glUniform1i(tex1loc, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex2);
    glUniform1i(tex2loc, 1);



    // Initialize FPS counter
    s_lastFrameTime = s_startTicks;
    s_fpsUpdateTime = s_startTicks;
    s_frameCount = 0;

    
    // Initialize text renderer for FPS display
    initTextRenderer();    

    // Bloom start
    makeFbo(s_sceneFbo, s_sceneTex, 1280, 720);
    makeFbo(s_bloomFboA, s_bloomTexA, BLOOM_W, BLOOM_H);
    makeFbo(s_bloomFboB, s_bloomTexB, BLOOM_W, BLOOM_H);

    // Comp bloom shaders
    GLuint bvsh = createAndCompileShader(GL_VERTEX_SHADER, rt_vs);

    auto linkBloom = [&](const char* fs) -> GLuint {
        GLuint fsh = createAndCompileShader(GL_FRAGMENT_SHADER, fs);
        GLuint prog = glCreateProgram();
        glAttachShader(prog, bvsh);
        glAttachShader(prog, fsh);
        glLinkProgram(prog);
        glDeleteShader(fsh);

        GLint ok; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char buf[512]; 
            glGetProgramInfoLog(prog, sizeof buf, nullptr, buf);
            FILE* f = fopen("/switch/bhrt_err.txt", "a");
            if (f) { fprintf(f, "Bloom Link:\n%s\n", buf); fclose(f); }
        }
        return prog;
    };

    // Link bloom shit
    
    s_bloomExtractProg = linkBloom(bloom_extract_fs);
    s_bloomBlurProg = linkBloom(bloom_blur_fs);
    s_bloomCompositeProg = linkBloom(bloom_composite_fs);
    glDeleteShader(bvsh);

    // Cache locations
    glUseProgram(s_bloomExtractProg);
    glUniform1i(glGetUniformLocation(s_bloomExtractProg, "sceneTex"), 0);
    // Cutoff for brightness
    glUniform1f(glGetUniformLocation(s_bloomExtractProg, "threshold"), 0.7f);

    glUseProgram(s_bloomBlurProg);
    glUniform1i(glGetUniformLocation(s_bloomBlurProg, "blurTex"), 0);
    s_blur_dirLoc = glGetUniformLocation(s_bloomBlurProg, "direction");
    s_blur_texelLoc = glGetUniformLocation(s_bloomBlurProg, "texelSize");
    glUniform2f(s_blur_texelLoc, 1.0f / BLOOM_W, 1.0f / BLOOM_H);

    glUseProgram(s_bloomCompositeProg);
    glUniform1i(glGetUniformLocation(s_bloomCompositeProg, "sceneTex"), 0);
    glUniform1i(glGetUniformLocation(s_bloomCompositeProg, "bloomTex"), 1);
    s_composite_bloomStrengthLoc = glGetUniformLocation(s_bloomCompositeProg, "bloomStrength");
    // global intensity
    glUniform1f(s_composite_bloomStrengthLoc, 2.4f);

    // Checkerboard
    // Previous scene needs the same format
    makeFbo(s_prevSceneFbo, s_prevSceneTex, 1280, 720);

    // Comp resolve
    GLuint resolve_vsh = createAndCompileShader(GL_VERTEX_SHADER, rt_vs);
    GLuint resolve_fsh = createAndCompileShader(GL_FRAGMENT_SHADER, checkerboard_resolve_fs);
    s_resolveProg = glCreateProgram();
    glAttachShader(s_resolveProg, resolve_vsh);
    glAttachShader(s_resolveProg, resolve_fsh);
    glLinkProgram(s_resolveProg);
    glDeleteShader(resolve_vsh);
    glDeleteShader(resolve_fsh);

    // What the fuck we're debugging now?
    {
    GLint ok;
    glGetProgramiv(s_resolveProg, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetProgramInfoLog(s_resolveProg, sizeof(buf), nullptr, buf);
        FILE* f = fopen("/switch/bhrt_err.txt", "a");
        if (f) { fprintf(f, "RESOLVE LINK:\n%s\n", buf); fclose(f); }
    }
    }

    // Setup samplers for resolve
    glUseProgram(s_resolveProg);
    glUniform1i(glGetUniformLocation(s_resolveProg, "currentTex"),  0);
    glUniform1i(glGetUniformLocation(s_resolveProg, "previousTex"), 1);
    glUniform2f(glGetUniformLocation(s_resolveProg, "texelSize"), 1.0f / 1280.0f, 1.0f / 720.0f);
    s_resolve_frameIndexLoc = glGetUniformLocation(s_resolveProg, "frameIndex");

    // Cache frameindexes in raymarcher
    glUseProgram(s_program);
    s_rt_frameIndexLoc = glGetUniformLocation(s_program, "frameIndex");

    // Clear the previous frame so 0 reads something
    glBindFramebuffer(GL_FRAMEBUFFER, s_prevSceneFbo);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glUseProgram(s_program);

}

float getTime5()
    {
        u64 elapsed = armGetSystemTick() - s_startTicks;
        return (elapsed * 625 / 12) / 2000000000.0;
    }

// Compute camera positions on the CPU instead of GPU
static void computeCamera_NEON(float t, float* outCamPos, float* outView)
{
    pinThread(0);

    float s = sinf(t * 0.1f);
    float c = cosf(t * 0.1f);

    // camPos = -cos*15, sin*15, 0
    float32x4_t eye = { -c * 15.f, s * 15.f, s * 15.f, 0.f};
    float32x4_t target = vdupq_n_f32(0.f);
    float32x4_t worldUp = { 0.f, 1.f, 0.f, 0.f};

    // lookat
    float32x4_t fwd = neon_normalize3(vsubq_f32(target, eye));
    float32x4_t right = neon_normalize3(neon_cross3(fwd, worldUp));
    float32x4_t newUp = neon_cross3(right, fwd);

    // Write camPos
    neon_store3(outCamPos, eye);

    // assemble mat3
    // Where lookat col0 -> right, col1 -> newUp, col2 -> -forward
    float32x4_t neg_fwd = vnegq_f32(fwd);
    neon_store3(outView + 0, right);
    neon_store3(outView + 3, newUp);
    neon_store3(outView + 6, fwd);
}

void BHRTRender()
{
    // FPS calculation
    u64 currentTime = armGetSystemTick();
    s_frameCount++;

    u64 timeSinceUpdate = currentTime - s_fpsUpdateTime;
    float secondsSinceUpdate = (timeSinceUpdate * 625.0f / 12.0f) / 1000000000.0f;
    
    if(secondsSinceUpdate >= 0.5f) {
        s_fps = s_frameCount / secondsSinceUpdate;
        s_frameCount = 0;
        s_fpsUpdateTime = currentTime;

    }

    glBindVertexArray(s_vao);

    // Compute camera positions on CPU
    float camPos[3];
    float view[9];
    computeCamera_NEON(getTime5(), camPos, view);


    // RT + Scene
    glBindFramebuffer(GL_FRAMEBUFFER, s_sceneFbo);
    glViewport(0, 0, 1280, 720);
    glUseProgram(s_program);
    glUniform3fv(loc_camPos, 1, camPos);
    glUniformMatrix3fv(loc_view, 1, GL_FALSE, view);
    glUniform1i(s_rt_frameIndexLoc, s_frameIndex);
    glActiveTexture(GL_TEXTURE0); 
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex1);
    glActiveTexture(GL_TEXTURE1); 
    glBindTexture(GL_TEXTURE_2D, tex2);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_3D, s_noiseTex3D);
    glUniform1i(s_noiseTexLoc, 2);
    glUniform1f(loc_time, getTime5());
    glUniform2f(resloc, 1280.0f, 720.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Resolve checkerboard
    glBindFramebuffer(GL_FRAMEBUFFER, s_prevSceneFbo);
    glViewport(0, 0, 1280, 720);
    glUseProgram(s_resolveProg);
    glUniform1i(s_resolve_frameIndexLoc, s_frameIndex);
    glActiveTexture(GL_TEXTURE0); 
    glBindTexture(GL_TEXTURE_2D, s_sceneTex);    // current (half-written)
    glActiveTexture(GL_TEXTURE1); 
    glBindTexture(GL_TEXTURE_2D, s_prevSceneTex); // previous (full)
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Brightness to bloom
    glBindFramebuffer(GL_FRAMEBUFFER, s_bloomFboA);
    glViewport(0, 0, BLOOM_W, BLOOM_H);
    glUseProgram(s_bloomExtractProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_prevSceneTex);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Actual blur, setup iterations
    glUseProgram(s_bloomBlurProg);
    const int BLUR_ITERATIONS = 8;
    for (int i = 0; i < BLUR_ITERATIONS; i++) {
        // A -> B Horizontal
        glBindFramebuffer(GL_FRAMEBUFFER, s_bloomFboB);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_bloomTexA);
        glUniform2f(s_blur_dirLoc, 1.0f, 0.0f);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // B -> A Vertical
        glBindFramebuffer(GL_FRAMEBUFFER, s_bloomFboA);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_bloomTexB);
        glUniform2f(s_blur_dirLoc, 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // compose -> scene
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 1280, 720);
    glUseProgram(s_bloomCompositeProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_prevSceneTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_bloomTexA);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Fps
    glBindVertexArray(0);
    char fpsText[32];
    snprintf(fpsText, sizeof(fpsText), "%.3f", s_fps);
    drawText(fpsText, - 0.95f, 0.90f, 0.02f, 1.0f, 0.0f, 0.0f);

    // flip for next frame
    s_frameIndex ^= 1;

}

void BHRTExit()
{
    cleanupTextRenderer();
    glDeleteFramebuffers(1, &s_prevSceneFbo);
    glDeleteTextures(1, &s_prevSceneTex);
    glDeleteProgram(s_resolveProg);
    glDeleteBuffers(1, &s_vbo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_program);
}

int BHRTMain(int arcg, char* argv[])
{
        // Set mesa configuration (useful for debugging)
    setMesaConfig();

    // Initialize EGL on the default window
    if (!initEgl(nwindowGetDefault()))
        return EXIT_FAILURE;

    // Load OpenGL routines using glad
    gladLoadGL();
    
    // Start our hell
    BHRTSceneInit();

    // Configure our supported input layout: a single player with standard controller styles
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    // Initialize the default gamepad (which reads handheld mode inputs as well as the first connected controller)
    PadState pad;
    padInitializeDefault(&pad);

    // Main graphics loop
    while (appletMainLoop())
    {
        // Get and process input
        padUpdate(&pad);
        u32 kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_B) {
            state = STATE_MENU;
            return 0;
        }
            

        // Render stuff!
        BHRTRender();
        eglSwapBuffers(s_display, s_surface);
    }

    // Deinit scene
    BHRTExit();

    // Deinit EGL
    deinitEgl();
    return EXIT_SUCCESS;
}