#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include <EGL/egl.h>    // EGL library
#include <EGL/eglext.h> // EGL extensions
#include <glad/glad.h>  // glad library (OpenGL loader)

#define GLM_FORCE_PURE
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include "stb_image.h"
#include "fur_png.h"
#include "noise_png.h"
#include "wall_png.h"

//-----------------------------------------------------------------------------
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
// Main program
//-----------------------------------------------------------------------------

static void setMesaConfig()
{
    // Uncomment below to disable error checking and save CPU time (useful for production):
    //setenv("MESA_NO_ERROR", "1", 1);

    // Uncomment below to enable Mesa logging:
    //setenv("EGL_LOG_LEVEL", "debug", 1);
    //setenv("MESA_VERBOSE", "all", 1);
    //setenv("NOUVEAU_MESA_DEBUG", "1", 1);

    // Uncomment below to enable shader debugging in Nouveau:
    //setenv("NV50_PROG_OPTIMIZE", "0", 1);
    //setenv("NV50_PROG_DEBUG", "1", 1);
    //setenv("NV50_PROG_CHIPSET", "0x120", 1);
}
    // vertex shader just to get anything on screen
static const char* const vertexShaderSource = R"text(
    #version 330 core

    out vec2 v_uv;

    void main() {
        vec2 pos = vec2(
            (gl_VertexID == 1) ? 3.0 : -1.0,
            (gl_VertexID == 2) ? 3.0 : -1.0
        );
    v_uv = pos * 0.5 + 0.5; 
    gl_Position = vec4(pos, 0.0, 1.0);
    }
)text";

    //trace melter
    //Currently, textures are broken, and only a single color is rendered
static const char* const fragmentShaderSource = R"text(
    #version 330 core

    out vec4 fragColor;

    uniform vec2 u_resolution;
    uniform float u_time;
    uniform sampler2D u_texture1;
    uniform sampler2D u_texture2;
    uniform sampler2D u_texture3;

    const float PI = 3.1416;
    const float TAU = 2 * PI;


    float displace(vec3 p, sampler2D tex) {
        float s = 4.5;
        float u = s / TAU * atan(p.y / p.x);
        float v = sign(p.z) / TAU *
            acos((p.z * p.z * sqrt(s * s + 1) + sqrt(1 - p.z * p.z * s * s)) / (p.z * p.z + 1));
        vec2 uv = 2.0 * vec2(u, v);
        float disp = texture(tex, uv).r;
        return disp * 0.06;
    }


    mat2 rot2D(float a) {
        float sa = sin(a);
        float ca = cos(a);
        return mat2(ca, sa, -sa, ca);
    }


    void rotate(inout vec3 p) {
        p.xy *= rot2D(sin(u_time * 0.8) * 0.25);
        p.yz *= rot2D(sin(u_time * 0.7) * 0.2);
    }


    float map(vec3 p) {
        float dist = length(vec2(length(p.xy) - 0.6, p.z)) - 0.22;
        return dist * 0.7;
    }


    vec3 getNormal(vec3 p) {
        vec2 e = vec2(0.01, 0.0);
        vec3 n = vec3(map(p)) - vec3(map(p - e.xyy), map(p - e.yxy), map(p - e.yyx));
        return normalize(n);
    }


    float rayMarch(vec3 ro, vec3 rd) {
        float dist = 0.0;
        for (int i = 0; i < 64; i++) {
            vec3 p = ro + dist * rd;

            rotate(p);
            float hit = map(p);
            dist += hit;

            // displace
            dist -= displace(0.5 * p, u_texture2);

            if (dist > 100.0 || abs(hit) < 0.0001) break;
        }
        return dist;
    }


    vec3 triPlanar(sampler2D tex, vec3 p, vec3 normal) {
        normal = abs(normal);
        normal = pow(normal, vec3(15));
        normal /= normal.x + normal.y + normal.z;
        p = p * 0.5 + 0.5;
        return (texture(tex, p.xy) * normal.z +
                texture(tex, p.xz) * normal.y +
                texture(tex, p.yz) * normal.x).rgb;
    }


    vec3 render(vec2 offset) {
        vec2 uv = (2.0 * (gl_FragCoord.xy + offset) - u_resolution.xy) / u_resolution.y;
        vec3 col = vec3(0);

        vec3 ro = vec3(0, 0, -1.0);
        vec3 rd = normalize(vec3(uv, 1.0));
        float dist = rayMarch(ro, rd);

        if (dist < 100.0) {
            vec3 p = ro + dist * rd;
            rotate(p);
            col += triPlanar(u_texture1, p * 1.0, getNormal(p));
        } else {
            float phi = atan(uv.y, uv.x);
            float rho = length(uv) + 0.2;

            phi += sin(0.3 * rho - 0.5 * u_time);

            float h = sin(8.0 * phi) * 0.5 + 0.5;

            vec2 st;
            st.x = 3.0 * phi / PI;
            st.y = u_time * 0.5 + PI / (rho + 0.1 * smoothstep(0.45, 0.5, h));

            col += texture(u_texture3, st).rgb;

            float occ = smoothstep(0.0, 0.45, h) - smoothstep(0.5, 1.0, h);
            col *= 1.0 - 0.45 * occ * rho;
            col *= rho;
        }
        return col;
    }


    vec3 renderAAx4() {
        vec4 e = vec4(0.125, -0.125, 0.375, -0.375);
        vec3 colAA = render(e.xz) + render(e.yw) + render(e.wx) + render(e.zy);
        return colAA /= 4.0;
    }


    void main() {
        vec3 color = renderAAx4();

        fragColor = vec4(color, 1.0);
    }
)text";

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

static GLuint s_program;
static GLuint s_vao, s_vbo;
// needed for shader to render onscreen
GLint resolutionLoc;
static GLuint tex1;
static GLuint tex2;
static GLuint tex3;

static GLint loc_mdlvMtx, loc_projMtx;
static GLint loc_lightPos, loc_ambient, loc_diffuse, loc_specular, loc_tex_diffuse;
static GLint loc_time;

static u64 s_startTicks;


static void sceneInit()
{
    GLint vsh = createAndCompileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLint fsh = createAndCompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);

    s_program = glCreateProgram();
    glAttachShader(s_program, vsh);
    glAttachShader(s_program, fsh);
    glLinkProgram(s_program);
    resolutionLoc = glGetUniformLocation(s_program, "u_resolution");
    GLuint tex1Loc = glGetUniformLocation(s_program, "u_texture1");
    GLuint tex2Loc = glGetUniformLocation(s_program, "u_texture2");
    GLuint tex3Loc = glGetUniformLocation(s_program, "u_texture3");
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
    loc_lightPos = glGetUniformLocation(s_program, "lightPos");
    loc_ambient = glGetUniformLocation(s_program, "ambient");
    loc_diffuse = glGetUniformLocation(s_program, "diffuse");
    loc_specular = glGetUniformLocation(s_program, "specular");
    loc_tex_diffuse = glGetUniformLocation(s_program, "tex_diffuse");
    loc_time = glGetUniformLocation(s_program, "u_time");

    struct Vertex
    {
        float position[3];
        float color[3];
        glm::vec2 texcoord;
        glm::vec3 normal;
    };

    static const Vertex vertices[] =
    {
        { { -0.5f, -0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
        { {  0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
        { {  0.0f,  0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
    };

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    // bind the Vertex Array Object first, then bind and set vertex buffer(s), and then configure vertex attributes(s).
    glBindVertexArray(s_vao);

    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, texcoord));
    glEnableVertexAttribArray(2);

    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(3);
    // note that this is allowed, the call to glVertexAttribPointer registered VBO as the vertex attribute's bound vertex buffer object so afterwards we can safely unbind
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // You can unbind the VAO afterwards so other VAO calls won't accidentally modify this VAO, but this rarely happens. Modifying other
    // VAOs requires a call to glBindVertexArray anyways so we generally don't unbind VAOs (nor VBOs) when it's not directly necessary.
    glBindVertexArray(0);

    glGenerateMipmap(GL_TEXTURE_2D);

    int width, height, nchan;
    stbi_set_flip_vertically_on_load(true);

    //Texture 1
    glGenTextures(1, &tex1);
    glBindTexture(GL_TEXTURE_2D, tex1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    stbi_uc* img = stbi_load_from_memory((const stbi_uc*)fur_png, fur_png_size, &width, &height, &nchan, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    stbi_image_free(img);

    //Texture 2
    glGenTextures(1, &tex2);
    glBindTexture(GL_TEXTURE_2D, tex2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    img = stbi_load_from_memory((const stbi_uc*)noise_png, noise_png_size, &width, &height, &nchan, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    stbi_image_free(img);

    //Texture 3 
    // just let me end
    glGenTextures(1, &tex3);
    glBindTexture(GL_TEXTURE_2D, tex3);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    img = stbi_load_from_memory((const stbi_uc*)wall_png, wall_png_size, &width, &height, &nchan, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    stbi_image_free(img);

        // Uniforms
    glUseProgram(s_program);
    auto projMtx = glm::perspective(
        glm::radians(40.0f),
        1280.0f / 720.0f,
        0.01f,
        1000.0f
    );
    glUniformMatrix4fv(loc_projMtx, 1, GL_FALSE, glm::value_ptr(projMtx));
    glUniform4f(loc_lightPos, 0.0f, 0.0f, 0.5f, 1.0f);
    glUniform3f(loc_ambient, 0.1f, 0.1f, 0.1f);
    glUniform3f(loc_diffuse, 0.4f, 0.4f, 0.4f);
    glUniform4f(loc_specular, 0.5f, 0.5f, 0.5f, 20.0f);
    s_startTicks = armGetSystemTick();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex1);
    glUniform1i(tex1Loc, 0);

    //Tex 2?
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex2);
    glUniform1i(tex2Loc, 1);

    //Tex 3 its 12am
    // im not here
    // this isn't happening
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex3);
    glUniform1i(tex3Loc, 2);
}

static float getTime()
    {
        u64 elapsed = armGetSystemTick() - s_startTicks;
        return (elapsed * 625 / 12) / 1000000000.0;
    }

static void sceneRender()
{
    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    // We want as much rendered as possible
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // draw our first triangle
    glUseProgram(s_program);
    glUniform2f(resolutionLoc, 1280.0f, 720.0f); //technically any resolution would work, but 1280x720 is actually visible.
    glBindVertexArray(s_vao); // seeing as we only have a single VAO there's no need to bind it every time, but we'll do so to keep things a bit more organized
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void sceneExit()
{
    glDeleteBuffers(1, &s_vbo);
    glDeleteVertexArrays(1, &s_vao);
    glDeleteProgram(s_program);
}

int main(int argc, char* argv[])
{
    // Set mesa configuration (useful for debugging)
    setMesaConfig();

    // Initialize EGL on the default window
    if (!initEgl(nwindowGetDefault()))
        return EXIT_FAILURE;

    // Load OpenGL routines using glad
    gladLoadGL();

    // Initialize our scene
    sceneInit();

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
        if (kDown & HidNpadButton_Plus)
            break;

        // Render stuff!
        sceneRender();
        eglSwapBuffers(s_display, s_surface);
    }

    // Deinitialize our scene
    sceneExit();

    // Deinitialize EGL
    deinitEgl();
    return EXIT_SUCCESS;
}