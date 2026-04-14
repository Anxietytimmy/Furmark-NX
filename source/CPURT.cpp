#include <stdio.h>
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

#include <glm/mat4x4.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <EGL/egl.h>    // EGL library
#include <EGL/eglext.h> // EGL extensions
#include <glad/glad.h>  // glad library (OpenGL loader)


#include "stb_image.h"
#include "sates.h"

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
    float* vertexData = (float*)malloc(100 * 64 * 6 * 5 * sizeof(float));
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
    
    free(vertexData);
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
}

static void setMesaConfig()
{
    // Uncomment below to disable error checking and save CPU time (useful for production):
    //setenv("MESA_NO_ERROR", "1", 1);

    // Uncomment below to enable Mesa logging:
    setenv("EGL_LOG_LEVEL", "debug", 1);
    setenv("MESA_VERBOSE", "all", 1);
    setenv("NOUVEAU_MESA_DEBUG", "1", 1);

    // Uncomment below to enable shader debugging in Nouveau:
    setenv("NV50_PROG_OPTIMIZE", "0", 1);
    setenv("NV50_PROG_DEBUG", "1", 1);
    setenv("NV50_PROG_CHIPSET", "0x120", 1);
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


// Accumulation variables
static GLuint tex[2], fbo[2];
static GLuint frameLoc;


static GLuint createAndCompileShader(GLenum type, const char* source)
{
    GLint success;
    GLchar msg[512];

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


// CPUPT fun
// FUCKING KILL ME GOD GDAMMIT

int width = 1280;
int height = 720;

static GLuint screenTex;


// needed vars
float u_time;

// Vars for multithreading
static const int THREAD_COUNT = 3;

static std::thread workers[THREAD_COUNT];
static std::atomic<bool> running(true);
static std::atomic<int> tilesDone(0);

static std::mutex workMutex;
static std::condition_variable workCV;

static bool workReady = false;
static int currentFrame = 0;

std::atomic<bool> cpuRenderRunning(false);

enum Material{
    WHITE,
    RED,
    GREEN,
    LIGHT,
    MIRROR
};


// Random
inline float randFloat(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;

    return (state & 0xFFFFFF) / float(0xFFFFFF);
}

// Vec2 replacement, because I like spaget
struct vec2f {
    float x, y;

    inline vec2f() {}
    inline vec2f(float x_, float y_) : x(x_), y(y_) {}
};

vec2f u_resolution(width, height);

// I FUCKING LOVE XENON BULBS NEON HELL YEAHHHH
// In full non shitpost fashion, NEON needs 128bit SIMD, or 4x  F32 lanes
// V3F below works for scalars, but for this we need more functinos
// -------------------------------
// V E C 3 X 4 N E O N
// -------------------------------
struct vec3x4 {
    float32x4_t x, y, z;
};

// NEON Dot product
inline float32x4_t dot(const vec3x4& a, const vec3x4& b){
    return vmlaq_f32(vmlaq_f32(vmulq_f32(a.x, b.x), a.y, b.y), a.z, b.z);
}

// Neon normalize
inline vec3x4 normalize(vec3x4 v){
    float32x4_t len2 = vmlaq_f32(vmlaq_f32(vmulq_f32(v.x, v.x), v.y, v.y), v.z, v.z);

    float32x4_t invLen = vrsqrteq_f32(len2);

    // NR refinement
    // Physx, quackclulus, quarks and stuff
    // basically, as my undervolted sleep deprived brain matter understands it
    // this is such that it is a way to approximate the root of a function and then check using curvature information to refine the result
    invLen = vmulq_f32(vrsqrtsq_f32(vmulq_f32(len2, invLen), invLen), invLen);

    return {
        vmulq_f32(v.x, invLen),
        vmulq_f32(v.y, invLen),
        vmulq_f32(v.z, invLen)
    };
}

// NEON reflections
// holy fuck the hills are silent
inline vec3x4 reflect(vec3x4 v, vec3x4 n){
    float32x4_t d = dot(v, n);
    float32x4_t two = vdupq_n_f32(2.0f);

    vec3x4 r;
    r.x = vmlsq_f32(v.x, n.x, vmulq_f32(two, d));
    r.y = vmlsq_f32(v.y, n.y, vmulq_f32(two, d));
    r.z = vmlsq_f32(v.z, n.z, vmulq_f32(two, d));
    return r;
}


// Faster vec3 replacement
// -------------------------------
// V E C 3 F S C A L A R
// -------------------------------
struct vec3f {
    float x, y, z;

    inline vec3f () {}
    inline vec3f(float v) : x(v), y(v), z(v) {}
    inline vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

// Vec3 operations, because A57 asked for adderal
inline vec3f operator+(const vec3f& a, const vec3f& b) {
    return vec3f(a.x + b.x, a.y + b.y, a.z + b.z);
}

inline vec3f operator-(const vec3f& a, const vec3f& b) {
    return vec3f(a.x - b.x, a.y - b.y, a.z - b.z);
}

inline vec3f operator-(const vec3f& v) {
    return vec3f(-v.x, -v.y, -v.z);
}

inline vec3f& operator*=(vec3f& a, const vec3f& b) {
    a.x *= b.x;
    a.y *= b.y;
    a.z *= b.z;
    return a;
}

inline vec3f& operator*=(vec3f& a, float b) {
    a.x *= b;
    a.y *= b;
    a.z *= b;
    return a;
}

inline vec3f operator*(const vec3f& a, float b) {
    return vec3f(a.x * b, a.y * b, a.z * b);
}

inline vec3f operator*(float b, const vec3f& a) {
    return a * b;
}

inline vec3f operator*(const vec3f& a, const vec3f& b) {
    return vec3f(a.x * b.x, a.y * b.y, a.z * b.z);
}

inline vec3f& operator+=(vec3f& a, const vec3f& b) {
    a.x += b.x; a.y += b.y; a.z += b.z;
    return a;
}

inline vec3f operator/(const vec3f& a, float b) {
    float inv = 1.0f / b;
    return vec3f(a.x * inv, a.y * inv, a.z * inv);
}

inline vec3f& operator/=(vec3f& a, float b) {
    float inv = 1.0f / b;
    a.x *= inv;
    a.y *= inv;
    a.z *= inv;
    return a;
}

// Dot product
inline float dot(const vec3f& a, const vec3f& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

// Normalization
// Replace with NEON rsqrt once we know this doesn't explode
inline vec3f normalize(const vec3f& v) {
    float len2 = dot(v, v);
    float invLen = 1.0f / sqrtf(len2 + 1e-20f);
    return v * invLen;
}

// Reflections
// In my restless dreams, I see that town
// The mental hospital
inline vec3f reflect(const vec3f& v, const vec3f& n) {
    return v - n * (2.0f * dot(v, n));
}

// clamp
inline vec3f clamp(const vec3f& v, float minv, float maxv) {
    return vec3f(
        fminf(fmaxf(v.x, minv), maxv),
        fminf(fmaxf(v.y, minv), maxv),
        fminf(fmaxf(v.z, minv), maxv)
    );
}

// Cross (cam)
inline vec3f cross(const vec3f& a, const vec3f& b) {
    return vec3f(
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x
    );
}


// Well this is cursed
// Scalar for neon functions
inline vec3f neon_normalize(const vec3f& v)
{
    float len2 = v.x*v.x + v.y*v.y + v.z*v.z;
    float inv = 1.0f / sqrtf(len2 + 1e-20f);
    return { v.x * inv, v.y * inv, v.z * inv };
}

inline vec3f neon_reflect(const vec3f& v, const vec3f& n)
{
    float d = v.x*n.x + v.y*n.y + v.z*n.z;
    return {
        v.x - 2.0f * d * n.x,
        v.y - 2.0f * d * n.y,
        v.z - 2.0f * d * n.z
    };
}


// Save accumulation as vectors, fallback
// static std::vector<vec3f> cpuAccum;
// static std::vector<vec3f> cpuFrame;

// Use array structures for the accumulation and current frame buffers respectively
// Accumulation buffer
alignas(64) static float* accumR;
alignas(64) static float* accumG;
alignas(64) static float* accumB;

// Final frame buffer
alignas(64) static float* frameR;
alignas(64) static float* frameG;
alignas(64) static float* frameB;

// Since we still use OGL to display our image, a conversion from raw values to RGB is needed
static std::vector<float> interleaved;

struct Hit {
    float t;
    Material mat;
    vec3f normal;
};

struct Sphere{
    vec3f center;
    float radius;
    int material;
};

// Sphere SDF
bool intersectSphere(vec3f ro, vec3f rd, vec3f center, float r, float& t)
{
    vec3f oc = ro - center;

    float b = dot(oc, rd);
    float c = dot(oc, oc) - r*r;
    float h = b*b - c;

    if(h < 0.0f) return false;

    h = sqrtf(h);
    t = -b - h;

    if(t < 0) t = -b + h;

    return t > 0;
}


// Scene
Hit intersectScene(vec3f ro, vec3f rd)
{
    Hit best;
    best.t = 1e30f;
    best.mat = WHITE;

    float t;

    const float eps = 1e-6f;

    // mirror sphere
    if(intersectSphere(ro,rd,vec3f(0,1,-0.5f),1.0f,t))
    {
        if(t < best.t)
        {
            best.t = t;
            best.mat = MIRROR;
            vec3f p = ro + rd*t;
            best.normal = normalize(p - vec3f(0,1,-0.5f));
        }
    }

    // left wall
    if (fabs(rd.x) > eps) {
        t = (-2.0f - ro.x) / rd.x;
        if(t>0 && t<best.t) {
            best.t = t;
            best.mat = RED;
            best.normal = vec3f(1,0,0);
        }
    }

    // right wall
    if (fabs(rd.x) > eps) {
        t = (2.0f - ro.x) / rd.x;
        if(t>0 && t<best.t) {
            best.t = t;
            best.mat = GREEN;
            best.normal = vec3f(-1,0,0);
        }
    }

    // floor
    if (fabs(rd.y) > eps) {
        t = (0.0f - ro.y) / rd.y;
        if(t>0 && t<best.t) {
            best.t = t;
            best.mat = WHITE;
            best.normal = vec3f(0,1,0);
        }
    }

    // ceiling
    if (fabs(rd.y) > eps) {
        t = (4.0f - ro.y) / rd.y;
        if(t>0 && t<best.t) {
            vec3f hit = ro + rd * t;

            if(fabs(hit.x) < 1.0f && fabs(hit.z) < 1.0f)
                best.mat = LIGHT;
            else
                best.mat = WHITE;

            best.t = t;
            best.normal = vec3f(0,-1,0);
        }
    }

    // back wall
    if (fabs(rd.z) > eps) {
        t = (2.0f - ro.z) / rd.z;
        if(t>0 && t<best.t) {
            best.t = t;
            best.mat = WHITE;
            best.normal = vec3f(0,0,-1);
        }
    }

    if(best.t < 1e29f)
        return best;

    best.t = -1.0f;
    return best;
}

struct RayPacket4 {
    vec3f ro[4];
    vec3f rd[4];
};

struct ColorPacket4 {
    vec3f c[4];
};


// Material colors
vec3f getColor(Material m){
    if(m == RED) return vec3f(1, 0.2, 0.2);
    if(m == GREEN) return vec3f(0.2, 1.0, 0.2);
    return vec3f(0.9);
}

bool isLight(int m){ return m == LIGHT; }
bool isMirror(int m){ return m == MIRROR;}

// have this in case this explodes or something
vec3f trace(vec3f ro, vec3f rd, uint32_t& rng)
{
    vec3f color(0.0f);
    vec3f throughput(1.0f);

    // Controls the amount of bounces that the rays are allowed to produce
    for(int bounce=0; bounce<3; bounce++)
    {
        Hit h = intersectScene(ro,rd);

        if(h.t < 0)
        {
            color += throughput * vec3f(0.7,0.8,1.0);
            break;
        }

        vec3f pos = ro + rd*h.t;
        vec3f n = h.normal;

        // ceiling light
        if(h.mat == LIGHT)
        {
            color += throughput * vec3f(12.0f);
            break;
        }

        if(h.mat == MIRROR)
        {
            rd = reflect(rd,n);
        }
        else
        {
            vec3f r = normalize(vec3f(
                randFloat(rng) * 2.0f - 1.0f,
                randFloat(rng) * 2.0f - 1.0f,
                randFloat(rng) * 2.0f - 1.0f
            ));

            if(dot(r, n) < 0.0f) r = -r;

            rd = normalize(n + r);

            throughput *= getColor(h.mat);
        }

        ro = pos + n * 0.001f;
    }

    return color;
}

// N E O N T I E M
void trace4(
    const vec3f ro[4],
    const vec3f rd[4],
    vec3f outColor[4],
    uint32_t rng[4])
{
    vec3f color[4] = {
        vec3f(0), vec3f(0), vec3f(0), vec3f(0)
    };

    vec3f throughput[4] = {
        vec3f(1), vec3f(1), vec3f(1), vec3f(1)
    };

    vec3f ro_l[4], rd_l[4];

    for(int i = 0; i < 4; i++)
    {
        ro_l[i] = ro[i];
        rd_l[i] = rd[i];
    }

    // path bounces
    for(int bounce = 0; bounce < 3; bounce++)
    {
        for(int i = 0; i < 4; i++)
        {
            Hit h = intersectScene(ro_l[i], rd_l[i]);

            if(h.t < 0.0f)
            {
                color[i].x += throughput[i].x * 0.7f;
                color[i].y += throughput[i].y * 0.8f;
                color[i].z += throughput[i].z * 1.0f;
                continue;
            }

            vec3f pos = {
                ro_l[i].x + rd_l[i].x * h.t,
                ro_l[i].y + rd_l[i].y * h.t,
                ro_l[i].z + rd_l[i].z * h.t
            };

            if(h.mat == LIGHT){
                color[i].x += throughput[i].x * 12.0f;
                color[i].y += throughput[i].y * 12.0f;
                color[i].z += throughput[i].z * 12.0f;
                continue;
            }      
            if(h.mat == MIRROR)
            {
                rd_l[i] = neon_reflect(rd_l[i], h.normal);
            }
            else
            {
                uint32_t& s = rng[i];

                vec3f r = {
                    randFloat(s) * 2.0f - 1.0f,
                    randFloat(s) * 2.0f - 1.0f,
                    randFloat(s) * 2.0f - 1.0f
                };

                r = neon_normalize(r);

                rd_l[i] = neon_normalize({
                    h.normal.x + r.x,
                    h.normal.y + r.y,
                    h.normal.z + r.z
                });

                vec3f c = getColor(h.mat);

                throughput[i].x *= c.x;
                throughput[i].y *= c.y;
                throughput[i].z *= c.z;
            }

            ro_l[i] = {
                pos.x + h.normal.x * 0.001f,
                pos.y + h.normal.y * 0.001f,
                pos.z + h.normal.z * 0.001f
            };
        }
    }

    for(int i = 0; i < 4; i++)
        outColor[i] = color[i];
}



static vec3f camForward;
static vec3f camRight;
static vec3f camUp;
static vec3f camPos;

// Scalar ray compute
inline vec3f computeRay(float x, float y)
{
    float uvx = x / float(width);
    float uvy = y / float(height);

    return normalize(
        camForward +
        camRight * (uvx * 2.0f - 1.0f) +
        camUp    * (uvy * 2.0f - 1.0f)
    );
}

//S E E B
inline uint32_t seed(int idx, int frame)
{
    return (uint32_t)(idx * 1973u ^ frame * 9277u ^ 0x9e3779b9u) | 1u;
}

// Denoising sampler
vec3f cpuPathTrace(float uvx, float uvy, float px, float py){
    // convert uv values to screen space
    float pxn = uvx * 2.0f - 1.0f;
    float pyn = uvy * 2.0f - 1.0f;

    pxn *= width / float(height);

    // Camera setup
    vec3f ro = vec3f(0, 2, -6);

    vec3f rd = normalize(camForward + pxn * camRight + pyn * camUp);

    uint32_t rng = uint32_t(px) * 1973u ^ uint32_t(py) * 9277u ^ uint32_t(frame) * 26699u | 1u;

    vec3f  col = trace(ro, rd, rng);

    return col;
}

int frameIndex = frame;


// Multithreading is silly
void renderTile(int startY, int endY, int frameIndex)
{
    float invW = 1.0f / width;
    float invH = 1.0f / height;

    // Average of frames
    float invFrame = 1.0f / float(currentFrame + 1);


    for(int y = startY; y < endY; y++){

    if(!cpuRenderRunning) return;

    for(int x = 0; x < width; x += 4)
    {
        int base = y * width + x;

        vec3f ro[4], rd[4], col[4];
        uint32_t rng[4];

        for(int k = 0; k < 4; k++)
        {
            int idx = base + k;

            ro[k] = camPos;
            rd[k] = computeRay(x + k, y);

            rng[k] = seed(idx, currentFrame);
        }

        trace4(ro, rd, col, rng);

        for(int k = 0; k < 4; k++)
        {
            int i = base + k;

            vec3f sample = col[k];

            sample = clamp(sample, 0.0f, 50.0f);

            float invFrame = 1.0f / float(currentFrame + 1);

            accumR[i] += (sample.x - accumR[i]) * invFrame;
            accumG[i] += (sample.y - accumG[i]) * invFrame;
            accumB[i] += (sample.z - accumB[i]) * invFrame;

            frameR[i] = accumR[i];
            frameG[i] = accumG[i];
            frameB[i] = accumB[i];
        }
    }
}
}

// What the hell is cache
void workerThread(int id)
{
    int tileHeight = height / THREAD_COUNT;

    while(running)
    {
        std::unique_lock<std::mutex> lock(workMutex);
        workCV.wait(lock, []{ return workReady || !running; });

        if(!running) return;

        int startY = id * tileHeight;
        int endY = (id == THREAD_COUNT - 1) ? height : startY + tileHeight;

        lock.unlock();

        renderTile(startY, endY, currentFrame);

        tilesDone.fetch_add(1, std::memory_order_relaxed);
    }
}


void CPURTSceneinit(){
    vec2f u_resolution(width, height);
    GLint vsh = createAndCompileShader(GL_VERTEX_SHADER, rt_vs);
    GLint fsh = createAndCompileShader(GL_FRAGMENT_SHADER, rt_fs);
    if(!vsh || !fsh) {
    TRACE("Shader compile failed — aborting");
    return;
    }

    s_program = glCreateProgram();
    glViewport(0, 0, 1280, 720);
    glAttachShader(s_program, vsh);
    glAttachShader(s_program, fsh);
    glBindFragDataLocation(s_program, 0, "fragColor");
    glLinkProgram(s_program);

    resolutionLoc = glGetUniformLocation(s_program, "u_resolution");
    loc_time = glGetUniformLocation(s_program, "u_time");

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

    loc_mdlvMtx = glGetUniformLocation(s_program, "mdlvMtx");
    loc_projMtx = glGetUniformLocation(s_program, "projMtx");
    loc_time = glGetUniformLocation(s_program, "u_time");

    static float vertices[] = {
        -1.0f, -1.0f,
         3.0f, -1.0f,
        -1.0f,  3.0f,
    };
        
    // Textures used by sampler, effectively, just the previous frame
    glGenTextures(2, tex);
    glGenFramebuffers(2, fbo);
    // Pass samples
    for(int i=0;i<2;i++)
    {
        // Generate texture
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        // Framebuffer setup
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);

        // Clear accumulation textures so we don't add garbage into results
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    // bind the Vertex Array Object first, then bind and set vertex buffer(s), and then configure vertex attributes(s).
    glBindVertexArray(s_vao);

    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    // enable SRGB because god knows I am too lazy to gamma correct at 2am
    glEnable(GL_FRAMEBUFFER_SRGB);

    // note that this is allowed, the call to glVertexAttribPointer registered VBO as the vertex attribute's bound vertex buffer object so afterwards we can safely unbind
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // You can unbind the VAO afterwards so other VAO calls won't accidentally modify this VAO, but this rarely happens. Modifying other
    // VAOs requires a call to glBindVertexArray anyways so we generally don't unbind VAOs (nor VBOs) when it's not directly necessary.
    glBindVertexArray(0);

        // Uniforms
    glUseProgram(s_program);
    auto projMtx = glm::perspective(
        glm::radians(40.0f),
        1280.0f / 720.0f,
        0.01f,
        1000.0f
    );
    glUniformMatrix4fv(loc_projMtx, 1, GL_FALSE, glm::value_ptr(projMtx));

    s_startTicks = armGetSystemTick();
    
    // Initialize FPS counter
    s_lastFrameTime = s_startTicks;
    s_fpsUpdateTime = s_startTicks;
    s_frameCount = 0;
    
    // Initialize text renderer for FPS display
    initTextRenderer();

    // Set starting conditions, this also helps reruns to not explode
    running = true;
    cpuRenderRunning = false;
    tilesDone = 0;
    workReady = false;

    for(int i = 0; i < THREAD_COUNT; i++)
    {
        workers[i] = std::thread(workerThread, i);
    }


    // Setup CPU for output
    int total = width * height;

    // Allocate proper memory for buffers
    accumR = (float*)aligned_alloc(64, total * sizeof(float));
    accumG = (float*)aligned_alloc(64, total * sizeof(float));
    accumB = (float*)aligned_alloc(64, total * sizeof(float));

    frameR = (float*)aligned_alloc(64, total * sizeof(float));
    frameG = (float*)aligned_alloc(64, total * sizeof(float));
    frameB = (float*)aligned_alloc(64, total * sizeof(float));

    // init buffers
    for(int i = 0; i < total; i++) {
        accumR[i] = accumG[i] = accumB[i] = 0.0f;
    }

    for (int i = 0; i < total; i++) {
        frameR[i] = frameG[i] = frameB[i] = 0.0f;
    }

    glGenTextures(1, &screenTex);
    glBindTexture(GL_TEXTURE_2D, screenTex);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

}

vec3f cpuPathTrace(vec2f uv, vec2f fragCoord);


// How did we get here
// Heat Stroke Struck
// Hell yeah death
void renderCPUFrame()
{
    cpuRenderRunning = true;
    tilesDone = 0;
    currentFrame = frame;

    {
        std::lock_guard<std::mutex> lock(workMutex);
        workReady = true;
    }

    workCV.notify_all();

    while (tilesDone.load(std::memory_order_acquire) < THREAD_COUNT)
        std::this_thread::yield();

    workReady = false;
}

float getTime3()
    {
        u64 elapsed = armGetSystemTick() - s_startTicks;
        return (elapsed * 625 / 12) / 2000000000.0;
    }


void CPURTRender(){
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);

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

    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    // We want as much rendered as possible
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    camPos = vec3f(0,2,-6);
    vec3f target = vec3f(0,2,0);

    camForward = normalize(target - camPos);
    camRight = normalize(cross(camForward, vec3f(0,1,0)));
    camUp = cross(camRight, camForward);

    // draw CPU functions
    renderCPUFrame();


    int total = width * height;
    interleaved.resize(total * 3);

    // draw our first triangle
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(s_program);
    glUniform1i(glGetUniformLocation(s_program, "screenTex"), 0);

    
    for(int i = 0; i < total; i ++) {
        interleaved[i * 3 + 0] = frameR[i];
        interleaved[i * 3 + 1] = frameG[i];
        interleaved[i * 3 + 2] = frameB[i];
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, screenTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_FLOAT, interleaved.data());
    glBindVertexArray(0);
    glBindVertexArray(s_vao);
    
    // Triangles, placed in your mind
    // You will never be free
    glDrawArrays(GL_TRIANGLES,0, 3);



    glUniform1f(loc_time, getTime3());
    glUniform2f(resolutionLoc, width, height); 

    
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);



    frame++;

    // Draw FPS counter
    glBindVertexArray(0);
    char fpsText[32];
    snprintf(fpsText, sizeof(fpsText), "%.3f", s_fps);
    drawText(fpsText, -0.95f, 0.90f, 0.02f, 1.0f, 0.0f, 0.0f);

    // Draw number of samples
    char sampleText[64];
    snprintf(sampleText, sizeof(sampleText), "Samples: %d", frame);
    drawText(sampleText, -0.95f, 0.90f, 0.02f, 1.0f, 0.0f, 0.0f);
}

void CPURTExit()
{
    cpuRenderRunning = false;
    running = false;
    workCV.notify_all();

    for(int i = 0; i < THREAD_COUNT; i++){
        if(workers[i].joinable())
            workers[i].join();
    }
    cleanupTextRenderer();
    glDeleteBuffers(1, &s_vbo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_program);

    free(accumR);
    free(accumG);
    free(accumB);

    free(frameR);
    free(frameG);
    free(frameB);
    // cpuAccum.clear();
    // cpuFrame.clear();
    frame = 0; 
}

int CPURTMain(int argc, char* argv[]){
        // Set mesa configuration (useful for debugging)
    setMesaConfig();

    // Initialize EGL on the default window
    if (!initEgl(nwindowGetDefault()))
        return EXIT_FAILURE;

    // Load OpenGL routines using glad
    gladLoadGL();

    // Initialize our scene
    CPURTSceneinit();

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
        CPURTRender();
        eglSwapBuffers(s_display, s_surface);
    }

    // Deinitialize our scene
    CPURTExit();

    // Deinitialize EGL
    deinitEgl();
    return EXIT_SUCCESS;
}
