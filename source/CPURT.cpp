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
#include "vec23.h"

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
    // setenv("EGL_LOG_LEVEL", "debug", 1);
    // setenv("MESA_VERBOSE", "all", 1);
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


// Accumulation variables
static GLuint tex[2], fbo[2];


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
static const int THREAD_COUNT = 2;
alignas(64) static std::atomic<int> nextTile(0);

static std::thread workers[THREAD_COUNT];
alignas(64) static std::atomic<bool> running(true);
alignas(64) static std::atomic<int> tilesDone(0);

static std::mutex workMutex;
static std::condition_variable workCV;

alignas(64) static bool workReady = false;
alignas(64) static int currentFrame = 0;

alignas(64) std::atomic<bool> cpuRenderRunning(false);


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


// Save accumulation as vectors, fallback
// static std::vector<vec3f> cpuAccum;
// static std::vector<vec3f> cpuFrame;

// Use a single struct for pixel data
struct alignas(16) PixelData { float r, g, b, a; };

alignas(64) static PixelData* frameBuffer = nullptr;

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
    for(int bounce=0; bounce<6; bounce++)
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

// Neon scene intersection
inline void intersectScene4(const vec3x4& ro, const vec3x4& rd, float32x4_t& t, uint32x4_t& mat, vec3x4& normal)
{
    const float32x4_t INF = vdupq_n_f32(1e30f);
    const float32x4_t ZERO = vdupq_n_f32(0.0f);
    const float32x4_t EPS = vdupq_n_f32(1e-4f);

    t = INF;
    mat = vdupq_n_u32(WHITE);
    normal.x = normal.y = normal.z = ZERO;

    // Sphere center (0, 1, -0.5) r = 1
    {
        const float32x4_t cx = vdupq_n_f32(0.0f);
        const float32x4_t cy = vdupq_n_f32(1.0f);
        const float32x4_t cz = vdupq_n_f32(-0.5f);

        vec3x4 oc;
        oc.x = vsubq_f32(ro.x, cx);
        oc.y = vsubq_f32(ro.y, cy);
        oc.z = vsubq_f32(ro.z, cz);

        float32x4_t b = dot(oc, rd);
        float32x4_t c = vsubq_f32(dot(oc, oc), vdupq_n_f32(1.0f));
        float32x4_t h = vsubq_f32(vmulq_f32(b, b), c);

        uint32x4_t  hasHit = vcgtq_f32(h, ZERO);
        float32x4_t sqH = vsqrtq_f32(vmaxq_f32(h, ZERO));

        // Near/Far roots
        float32x4_t t0 = vsubq_f32(vnegq_f32(b), sqH);
        float32x4_t t1 = vaddq_f32(vnegq_f32(b), sqH);   

        // Even if this should never happen as the scene is static,
        // If near root is behind the camera, fall back to far root
        uint32x4_t useNear = vcgtq_f32(t0, EPS);
        float32x4_t tS = vbslq_f32(useNear, t0, t1); 

        uint32x4_t valid = vandq_u32(hasHit, vcgtq_f32(tS, EPS));
        uint32x4_t closer = vcltq_f32(tS, t);
        uint32x4_t mask = vandq_u32(valid, closer);

        // Hit positons/normals, only compute them where the mask is set
        vec3x4 hp;
        hp.x = vmlaq_f32(ro.x, rd.x, tS);
        hp.y = vmlaq_f32(ro.y, rd.y, tS);
        hp.z = vmlaq_f32(ro.z, rd.z, tS);

        vec3x4 n;
        n.x = vsubq_f32(hp.x, cx);
        n.y = vsubq_f32(hp.y, cy);
        n.z = vsubq_f32(hp.z, cz);
        n = normalize(n);

        t = vbslq_f32(mask, tS, t);
        mat = vbslq_u32(mask, vdupq_n_u32(MIRROR), mat);
        normal.x = vbslq_f32(mask, n.x, normal.x);
        normal.y = vbslq_f32(mask, n.y, normal.y);
        normal.z = vbslq_f32(mask, n.z, normal.z);
    }

    // Test the same pattern for every wall, since they are static 
    auto testPlane = [&] (float32x4_t roAxis, float32x4_t rdAxis, float target, float nx, float ny, float nz, uint32_t material)
    {
        float32x4_t denom = rdAxis;
        float32x4_t tP = vdivq_f32(vsubq_f32(vdupq_n_f32(target), roAxis), denom);

        uint32x4_t notParallel = vcgtq_f32(vabsq_f32(denom), EPS);
        uint32x4_t ahead = vcgtq_f32(tP, EPS);
        uint32x4_t closer = vcltq_f32(tP, t);
        uint32x4_t mask = vandq_u32(notParallel, vandq_u32(ahead, closer));

        t = vbslq_f32(mask, tP, t);
        mat = vbslq_u32(mask, vdupq_n_u32(material), mat);
        normal.x = vbslq_f32(mask, vdupq_n_f32(nx), normal.x);
        normal.y = vbslq_f32(mask, vdupq_n_f32(ny), normal.y);
        normal.z = vbslq_f32(mask, vdupq_n_f32(nz), normal.z);   
    };

    // Floor @y = 0
    testPlane(ro.y, rd.y, 0.0f, 0, 1, 0, WHITE);

    // Ceiling @y = 4 
    {
        float32x4_t denom = rd.y;
        float32x4_t tP = vdivq_f32(vsubq_f32(vdupq_n_f32(4.0f), ro.y), denom);

        uint32x4_t notParallel = vcgtq_f32(vabsq_f32(denom), EPS);
        uint32x4_t ahead = vcgtq_f32(tP, EPS);
        uint32x4_t closer = vcltq_f32(tP, t);
        uint32x4_t mask = vandq_u32(notParallel, vandq_u32(ahead, closer));

        // Compute x/y hits to find light point vs white
        float32x4_t hx = vmlaq_f32(ro.x, rd.x, tP);
        float32x4_t hz = vmlaq_f32(ro.z, rd.z, tP);
        uint32x4_t inX = vcltq_f32(vabsq_f32(hx), vdupq_n_f32(1.0f));
        uint32x4_t inZ = vcltq_f32(vabsq_f32(hz), vdupq_n_f32(1.0f));
        uint32x4_t isLightPanel = vandq_u32(inX, inZ);
        uint32x4_t ceilMat = vbslq_u32(isLightPanel, vdupq_n_u32(LIGHT), vdupq_n_u32(WHITE));

        t = vbslq_f32(mask, tP, t);
        mat = vbslq_u32(mask, ceilMat, mat);
        normal.x = vbslq_f32(mask, vdupq_n_f32(0.0f), normal.x);
        normal.y = vbslq_f32(mask, vdupq_n_f32(-1.0f), normal.y);
        normal.z = vbslq_f32(mask, vdupq_n_f32(0.0f), normal.z);
    }

    // Left wall
    testPlane(ro.x, rd.x, -2.0f, +1, 0, 0, RED);
    
    // Right wall
    testPlane(ro.x, rd.x, +2.0f, -1, 0, 0, GREEN);

    // Back wall
    testPlane(ro.z, rd.z, +2.0f, 0, 0, -1, WHITE);
}

// N E O N T I E M
void trace4(vec3x4 ro, vec3x4 rd, vec3f outColor[4], uint32_t rng[4])
{
    // Accumnulation registers
    vec3x4 color;
    color.x = color.y = color.z = vdupq_n_f32(0.0f);

    vec3x4 throughput;
    throughput.x = throughput.y = throughput.z = vdupq_n_f32(1.0f);

    // Define alive, lines that still need bounces
    uint32x4_t alive = vdupq_n_u32(0xFFFFFFFF);

    const float32x4_t BIAS = vdupq_n_f32(0.001f);

    for(int bounce = 0; bounce <3; bounce++)
    {
        // If all lines are dead, stop
        if(vaddvq_u32(alive) == 0) break;

        float32x4_t t;
        uint32x4_t mat;
        vec3x4 normal;
        intersectScene4(ro, rd, t, mat, normal);

        uint32x4_t hit = vcltq_f32(t, vdupq_n_f32(1e29f));
        uint32x4_t miss = vandq_u32(alive, vmvnq_u32(hit));

        //Add a sky color for any lanes we miss
        color.x = vbslq_f32(miss, vaddq_f32(color.x, vmulq_f32(throughput.x, vdupq_n_f32(0.7f))), color.x);
        color.y = vbslq_f32(miss, vaddq_f32(color.y, vmulq_f32(throughput.y, vdupq_n_f32(0.8f))), color.y);
        color.z = vbslq_f32(miss, vaddq_f32(color.z, vmulq_f32(throughput.z, vdupq_n_f32(1.0f))), color.z);
        // Kill missed lanes
        alive = vandq_u32(alive, hit);

        // Hit position
        vec3x4 pos;
        pos.x = vmlaq_f32(ro.x, rd.x, t);
        pos.y = vmlaq_f32(ro.y, rd.y, t);
        pos.z = vmlaq_f32(ro.z, rd.z, t);

        // Material masks
        uint32x4_t isLight = vandq_u32(alive, vceqq_u32(mat, vdupq_n_u32(LIGHT)));
        uint32x4_t isMirror = vandq_u32(alive, vceqq_u32(mat, vdupq_n_u32(MIRROR)));
        uint32x4_t isDiff = vandq_u32(alive, vmvnq_u32(vorrq_u32(vceqq_u32(mat, vdupq_n_u32(LIGHT)), vceqq_u32(mat, vdupq_n_u32(MIRROR)))));

        // Light
        color.x = vbslq_f32(isLight, vaddq_f32(color.x, vmulq_f32(throughput.x, vdupq_n_f32(6.0f))), color.x);
        color.y = vbslq_f32(isLight, vaddq_f32(color.y, vmulq_f32(throughput.y, vdupq_n_f32(6.0f))), color.y);
        color.z = vbslq_f32(isLight, vaddq_f32(color.z, vmulq_f32(throughput.z, vdupq_n_f32(6.0f))), color.z);
        // Kill light lanes
        alive = vandq_u32(alive, vmvnq_u32(isLight));

        // Mirror reflection, no throughput changes
        vec3x4 refl = reflect(rd, normal);
        rd.x = vbslq_f32(isMirror, refl.x, rd.x);
        rd.y = vbslq_f32(isMirror, refl.y, rd.y);
        rd.z = vbslq_f32(isMirror, refl.z, rd.z);

        // Build albedo from material per lane for diffuse
        // White = 0.9, red = 1,0.2,0.2, geen = 0.2,1,0.2
        uint32x4_t isWhite = vceqq_u32(mat, vdupq_n_u32(WHITE));
        uint32x4_t isRed = vceqq_u32(mat, vdupq_n_u32(RED));
        uint32x4_t isGreen = vceqq_u32(mat, vdupq_n_u32(GREEN));

        float32x4_t albR = vbslq_f32(isWhite, vdupq_n_f32(0.9f), vbslq_f32(isRed, vdupq_n_f32(1.0f), vdupq_n_f32(0.2f)));
        float32x4_t albG = vbslq_f32(isWhite, vdupq_n_f32(0.9f), vbslq_f32(isRed, vdupq_n_f32(0.2f), vdupq_n_f32(1.0f)));
        float32x4_t albB = vbslq_f32(isWhite, vdupq_n_f32(0.9f), vdupq_n_f32(0.2f));

        throughput.x = vbslq_f32(isDiff, vmulq_f32(throughput.x, albR), throughput.x);
        throughput.y = vbslq_f32(isDiff, vmulq_f32(throughput.y, albG), throughput.y);
        throughput.z = vbslq_f32(isDiff, vmulq_f32(throughput.z, albB), throughput.z);

        // Diffuse bounce direction
        // We use scalars per lane as RNG isn't easily vectorized
        // Extract normals to scalar for building
        float nx[4], ny[4], nz[4];
        vst1q_f32(nx, normal.x);
        vst1q_f32(ny, normal.y);
        vst1q_f32(nz, normal.z);

        uint32_t diffMask[4];
        vst1q_u32(diffMask, isDiff);

        float newRx[4], newRy[4], newRz[4];
        vst1q_f32(newRx, rd.x);
        vst1q_f32(newRy, rd.y);
        vst1q_f32(newRz, rd.z);

        for(int i = 0; i < 4; i++)
        {
            if(!diffMask[i]) continue;

            // Sphere sampling
            float rx, ry, rz, len2;
            do {
                rx = randFloat(rng[i]) * 2.0f - 1.0f;
                ry = randFloat(rng[i]) * 2.0f - 1.0f;
                rz = randFloat(rng[i]) * 2.0f - 1.0f;
                len2 = rx*rx + ry*ry + rz*rz;
            } while(len2 > 1.0f || len2 < 1e-6f);

            // Flip hemisphere
            if(rx*nx[i] + ry*ny[i] + rz*nz[i] < 0.0f)
            { rx = -rx; ry = -ry; rz = -rz; }

            // normalize n+r
            float dx = nx[i] + rx;
            float dy = ny[i] + ry;
            float dz = nz[i] + rz;
            float l2 = dx*dx + dy*dy + dz*dz;
            float inv = 1.0f / sqrtf(l2 + 1e-20f);
            newRx[i] = dx * inv;
            newRy[i] = dy * inv;
            newRz[i] = dz * inv;
        }

        // Write back diffuse directions
        // Not needed for mirror lanes as they are already updated
        float32x4_t drx = vld1q_f32(newRx);
        float32x4_t dry = vld1q_f32(newRy);
        float32x4_t drz = vld1q_f32(newRz);
        rd.x = vbslq_f32(isDiff, drx, rd.x);
        rd.y = vbslq_f32(isDiff, dry, rd.y);
        rd.z = vbslq_f32(isDiff, drz, rd.z);

        // Advance rays off the surface
        ro.x = vmlaq_f32(pos.x, normal.x, BIAS);
        ro.y = vmlaq_f32(pos.y, normal.y, BIAS);
        ro.z = vmlaq_f32(pos.z, normal.z, BIAS);
    }

    // Unpack results
    float cr[4], cg[4], cb[4];
    vst1q_f32(cr, color.x);
    vst1q_f32(cg, color.y);
    vst1q_f32(cb, color.z);
    for(int i = 0; i < 4; i++)
        outColor[i] = { cr[i], cg[i], cb[i] };
}

static vec3f camForward;
static vec3f camRight;
static vec3f camUp;
static vec3f camPos;

// Scalar ray compute
inline vec3f computeRay(float x, float y)
{
    float uvx = (x + 0.5f) / float(width);
    float uvy = (y + 0.5f) / float(height);

    float px = uvx * 2.0f - 1.0f;
    float py = uvy * 2.0f - 1.0f;

    float aspect = float(width) / float(height);
    px *= aspect;  

    return normalize(
        camForward +
        camRight * px +
        camUp    * py
    );
}

//S E E B
inline uint32_t seed(int idx, int frame)
{
    return (uint32_t)(idx * 1973u ^ frame * 9277u ^ 0x9e3779b9u) | 1u;
}

//Pin threads to cores so the OS doesn't throw them around everywhere
void pinThread(int core)
{
    Handle thread = CUR_THREAD_HANDLE;

    // Allow only this core
    u64 mask = (1ULL << core);

    svcSetThreadCoreMask(thread, mask, mask);
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
    // Convert to UV
    const float32x4_t invW = vdupq_n_f32(1.0f / width);
    const float32x4_t invH = vdupq_n_f32(1.0f / height);
    const float aspect = float(width) / float(height);


    for(int y = startY; y < endY; y++){

    float32x4_t py = vdupq_n_f32(float(y) + 0.5f);
    float32x4_t uvy = vmulq_f32(py, invH);
    float32x4_t sy = vsubq_f32(vmulq_n_f32(uvy, 2.0f), vdupq_n_f32(1.0f));

    for(int x = 0; x < width; x += 4)
    {
        int base = y * width + x;

        vec3f ro[4], rd[4], col[4];
        uint32_t rng[4];
            
            // pixel coord
            float32x4_t px = {
                float(x + 0) + 0.5f,
                float(x + 1) + 0.5f,
                float(x + 2) + 0.5f,
                float(x + 3) + 0.5f
            };
            
            // Convert to UV
            float32x4_t invW = vdupq_n_f32(1.0f / width);
            float32x4_t invH = vdupq_n_f32(1.0f / height);

            float32x4_t uvx = vmulq_f32(px, invW);

            float32x4_t sx = vsubq_f32(vmulq_n_f32(uvx, 2.0f), vdupq_n_f32(1.0f));

            // correct aspect
            sx = vmulq_n_f32(sx, aspect);

            vec3x4 ro4, rd4;

            // Replicate origin
            ro4.x = vdupq_n_f32(camPos.x);
            ro4.y = vdupq_n_f32(camPos.y);
            ro4.z = vdupq_n_f32(camPos.z);

            // Direction -> forward + right * sx + up * sx
            rd4.x = vaddq_f32(vdupq_n_f32(camForward.x), vaddq_f32(vmulq_n_f32(sx, camRight.x), vmulq_n_f32(sy, camUp.x)));
            rd4.y = vaddq_f32(vdupq_n_f32(camForward.y), vaddq_f32(vmulq_n_f32(sx, camRight.y), vmulq_n_f32(sy, camUp.y)));
            rd4.z = vaddq_f32(vdupq_n_f32(camForward.z), vaddq_f32(vmulq_n_f32(sx, camRight.z), vmulq_n_f32(sy, camUp.z)));

            // Normalize packets
            rd4 = normalize(rd4);

            // Rng still needed
            for(int k=0;k<4;k++)
            {
                int idx = base + k;
                rng[k] = seed(idx, currentFrame);
            }

        trace4(ro4, rd4, col, rng);

        float scale = 1.0f / (float)(currentFrame + 1);
        float32x4_t vScale = vdupq_n_f32(scale);

        for(int k = 0; k < 4; k++)
        {
            // NEON based vectorized accumulation
            int idx = base + k;

            // Load pixels accumulation state into a single register
            float32x4_t old_pix = vld1q_f32((float*)&frameBuffer[idx]);

            // create a vector for traced sample
            float32x4_t new_samp = { col[k].x, col[k].y, col[k].z, 1.0f };

            // Blend pixel data
            float32x4_t diff = vsubq_f32(new_samp, old_pix);
            float32x4_t blended = vmlaq_f32(old_pix, diff, vScale);

            // Force an alpha value of 1 to make sure accumulation doesn't exlode
            blended = vsetq_lane_f32(1.0f, blended, 3);

            // Write results
            vst1q_f32((float*)&frameBuffer[idx], blended);
        }
    }
}
}

// What the hell is cache
// Each pixel writes around 32b
// Width is at 1280, so at 1280x16 the total size per thread is 655KB
// TX1 has 2MB of shared L2, so 655KB * 3 = 1965KB. 
const int TILE_H = 16;

void workerThread(int id)
{
    // Pin threads to cores
    int core = id;
    pinThread(core);

    while (running)
    {
        std::unique_lock<std::mutex> lock(workMutex);
        workCV.wait(lock, []{ return workReady || !running; });
        lock.unlock();

        if (!running) return;

        while (true)
        {
            int tileBase = nextTile.fetch_add(2, std::memory_order_relaxed);

            for (int t = 0; t < 2; t++)
            {
                int tile = tileBase + t;
                int startY = tile * TILE_H;

                if (startY >= height)
                    goto worker_done;
                
                int endY = std::min(startY + TILE_H, height);
                renderTile(startY, endY, currentFrame);
    
            }
        }
        worker_done:
        tilesDone.fetch_add(1, std::memory_order_relaxed);
    }
}


void CPURTSceneinit(){
    pinThread(2);
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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
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
    nextTile = 0;
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

    // Allocate proper memory for CPU frame buffer
    frameBuffer = (PixelData*)aligned_alloc(64, total * sizeof(PixelData));

    // Zero out buffer
    memset(frameBuffer, 0, total * sizeof(PixelData));

    glUniform1i(glGetUniformLocation(s_program, "screenTex"), 0);


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
    nextTile = 0;

    {
        std::lock_guard<std::mutex> lock(workMutex);
        workReady = true;
    }

    workCV.notify_all();

    // Main thread used as a worker
    while (true)
    {
        int tileBase = nextTile.fetch_add(2, std::memory_order_relaxed);

        bool finished = false;

        for(int t = 0; t < 2; t++)
        {
            int tile = tileBase + t;
            int startY = tile * TILE_H;

            if (startY >= height)
            {
                finished = true; 
                break;
            }
            int endY = std::min(startY + TILE_H, height);
            renderTile(startY, endY, currentFrame);
        }
        if (finished) break;
    }

    // Wait for workers to finish
    while (tilesDone.load(std::memory_order_acquire) < THREAD_COUNT)
    {
        svcSleepThread(1000);
    }

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

    // draw our first triangle
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(s_program);


    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, screenTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, frameBuffer);
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
    nextTile = 0;
    workCV.notify_all();

    for(int i = 0; i < THREAD_COUNT; i++){
        if(workers[i].joinable())
            workers[i].join();
    }
    cleanupTextRenderer();
    glDeleteBuffers(1, &s_vbo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_program);

    free(frameBuffer);
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
