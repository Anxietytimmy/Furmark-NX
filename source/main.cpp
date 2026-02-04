#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <switch.h>

#include <EGL/egl.h>
#include <glad/glad.h>

#define GLM_FORCE_PURE
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "stb_image.h"

// Include parts
#include "furmark.h"
#include "sates.h"
#include "fmrkRAM.h"

// NX Link Support

constexpr auto TAU = glm::two_pi<float>();
#ifndef ENABLE_NXLINK
#define TRACE(fmt,...) ((void)0)
#else
#include <unistd.h>
#define TRACE(fmt,...) printf("%s: " fmt "\n", __PRETTY_FUNCTION__, ## __VA_ARGS__)
static int s_nxlinkSock = -1;
static void initNxLink() {
    if (R_FAILED(socketInitializeDefault())) return;
    s_nxlinkSock = nxlinkStdio();
    if (s_nxlinkSock >= 0) TRACE("printf output now goes to nxlink server");
    else socketExit();
}
static void deinitNxLink() {
    if (s_nxlinkSock >= 0) {
        close(s_nxlinkSock);
        socketExit();
        s_nxlinkSock = -1;
    }
}
extern "C" void userAppInit() { initNxLink(); }
extern "C" void userAppExit() { deinitNxLink(); }
#endif

// Menu test
enum MenuOption {
    MENU_FURMARK = 0,
    MENU_FURMARK_RAMBURN,
    MENU_GPU_PT,
    MENU_BH_RT,
    MENU_CPU_RT,
    MENU_CPU_RAMBURN,
    MENU_EXIT,
    MENU_COUNT
};

typedef struct {
    const char* label;
    int row;
} MenuEntry;

// Text for menu entries
static const MenuEntry s_menuEntries[MENU_COUNT] = {
    {"Furmark (48 Step)", 6},
    {"Furmark (RAM Texture stress)", 7},
    {"GPU PathTracing", 8},
    {"GPU RT Black Hole", 9},
    {"CPU Ray Tracing", 10},
    {"CPU RAM Stress", 11},
    {"Exit", 12}
};

// waow
static void drawMenu(int selected) {
    consoleClear();
    printf("\x1b[1;31m");
    printf("\x1b[2;10H===============================================");
    printf("\x1b[3;10H                Furmark-NX v0.2.0                    ");
    printf("\x1b[4;10H===============================================");
    printf("\x1b[0m");
    
    for(int i = 0; i < MENU_COUNT; i++) {
        printf("\x1b[1;31m");
        printf("\x1b[%d;15H", s_menuEntries[i].row);
        if(selected == i) printf("\x1b[7m");
        printf("> %s", s_menuEntries[i].label);
        if(selected == i) printf("\x1b[0m");
    }
    
    printf("\x1b[20;10H B - Back, + - Exit. Use DPAD/LStick to navigate");


    consoleUpdate(NULL);
}

// EGL init so exiting doesn't completely shit itself
static EGLDisplay s_display;
static EGLContext s_context;
static EGLSurface s_surface;

// EGL display/FB init
static bool initEgl(NWindow* win) {
    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!s_display) return false;
    eglInitialize(s_display, nullptr, nullptr);
    eglBindAPI(EGL_OPENGL_API);

    EGLConfig config;
    EGLint numConfigs;
    static const EGLint framebufferAttributeList[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE
    };
    eglChooseConfig(s_display, framebufferAttributeList, &config, 1, &numConfigs);
    if (numConfigs == 0) return false;

    s_surface = eglCreateWindowSurface(s_display, config, win, nullptr);
    if (!s_surface) return false;

    static const EGLint contextAttributeList[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_NONE
    };
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, contextAttributeList);
    if (!s_context) return false;

    eglMakeCurrent(s_display, s_surface, s_surface, s_context);
    return true;
}

// EGL Deinit 
static void deinitEgl() {
    if (s_display) {
        eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (s_context) eglDestroyContext(s_display, s_context);
        if (s_surface) eglDestroySurface(s_display, s_surface);
        eglTerminate(s_display);
        s_display = nullptr;
        s_context = nullptr;
        s_surface = nullptr;
    }
}




// I am writing this at almost 11pm
// To whoever tries to understand this dumpster fire, I am so sorry

int main(int argc,char* argv[]){
    // Configure our supported input.
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    
    consoleInit(NULL);
    
    // Set defaults
    int selectedItem = 0;
    AppState state = STATE_MENU;
    

    drawMenu(selectedItem);

    while(appletMainLoop()){
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        
        switch(state) {
            case STATE_MENU: {
                if(kDown & HidNpadButton_Plus) {
                    goto exit_app;
                }
                
                if(kDown & HidNpadButton_AnyDown) {
                    selectedItem = (selectedItem + 1) % MENU_COUNT;
                    drawMenu(selectedItem);
                }
                
                if(kDown & HidNpadButton_AnyUp) {
                    selectedItem = (selectedItem - 1 + MENU_COUNT) % MENU_COUNT;
                    drawMenu(selectedItem);
                }
                
                if(kDown & HidNpadButton_A) {
                    switch((MenuOption)selectedItem) {
                        case MENU_FURMARK:
                            consoleExit(NULL);
                            if(initEgl(nwindowGetDefault())) {
                                gladLoadGL();
                                frSceneInit();
                                state = STATE_FURMARK;
                            }
                            break;

                        case MENU_FURMARK_RAMBURN:
                            consoleExit(NULL);
                            if(initEgl(nwindowGetDefault())) {
                                gladLoadGL();
                                frRamSceneInit();
                                state = STATE_FURMARK_RB;
                            }
                            break;

                        case MENU_EXIT:
                            goto exit_app;
                        
                        default:
                            break;
                    }
                }
                break;
            }
            
            case STATE_FURMARK: {
                if(kDown & HidNpadButton_B) {
                    frExit();
                    deinitEgl();
                    consoleInit(NULL);
                    drawMenu(selectedItem);
                    state = STATE_MENU;
                } else {
                    frRender();
                    eglSwapBuffers(s_display, s_surface);
                }
                break;
            }

            case STATE_FURMARK_RB: {
                if(kDown & HidNpadButton_B) {
                    frRamExit();
                    deinitEgl();
                    consoleInit(NULL);
                    drawMenu(selectedItem);
                    state = STATE_MENU;
                } else {
                    frRamRender();
                    eglSwapBuffers(s_display, s_surface);
                }
                break;
                }




            }
        }
    

 exit_app:
    if(state == STATE_FURMARK) {
        frExit();
        deinitEgl();
    } else if(state == STATE_FURMARK_RB) {
        frRamExit();
        deinitEgl();
    }

    consoleExit(NULL);
    return EXIT_SUCCESS;
}