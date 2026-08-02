#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include <deko3d.hpp>

#define GLM_FORCE_PURE
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include "../stb_image.h"
#include "fur_png.h"
#include "noise_png.h"
#include "wall_png.h"
#include "../sates.h"
#include "furmarkDK3D.h"

// Usual NXLINK Shit
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
    if (s_nxlinkSock < 0)
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

extern "C" void userAppInit() { initNxLink(); }
extern "C" void userAppExit() { deinitNxLink(); }
#endif

// Alright boys after not having a red bull in 3 months its DK3D time
// Globals

static const unsigned NUM_FRAMEBUFFERS = 2;
static const unsigned FB_WIDTH = 1280;
static const unsigned FB_HEIGHT = 720;
static const unsigned CMDBUF_SIZE = 256 * 1024;

static dk::UniqueDevice s_device;
static dk::UniqueQueue s_queue;
static dk::UniqueSwapchain s_swapchain;

static dk::UniqueCmdBuf s_cmdbuf;
static dk::UniqueMemBlock s_cmdbufMemBlock;

static dk::Image s_framebuffers[NUM_FRAMEBUFFERS];
static dk::UniqueMemBlock s_fbMemBlock;

// Memory pool allocation, one for GPU cached and another for CPU visible data
static dk::UniqueMemBlock s_imagePool;
static dk::UniqueMemBlock s_dataPool;

static uint32_t s_dataPoolOffset = 0;
static uint32_t s_imagePoolOffset = 0;

// Link shaders
static dk::Shader s_furVertexShader, s_furFragmentShader;
static dk::UniqueMemBlock s_furVshCode, s_furFshCode;

// Generic shader loader from romfs
static void loadShader(dk::Shader& shader, dk::UniqueMemBlock& outMemBlock, const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { TRACE("Failed to open shader %s", path); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Shader code memory must be Code-flagged and sized/aligned
    uint32_t alignedSize = (size + DK_SHADER_CODE_UNUSABLE_SIZE + 4095) & ~4095u;

    outMemBlock = dk::MemBlockMaker{s_device, alignedSize}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code)
        .create();

    fread(outMemBlock.getCpuAddr(), 1, size, f);
    fclose(f);

    dk::ShaderMaker{outMemBlock, 0}.initialize(shader);
}

// Textures
struct Texture
{
    dk::Image image;
    DkResHandle handle;
};

static Texture s_tex1, s_tex2, s_tex3;

// Descriptor sets
static dk::UniqueMemBlock s_imageDescMemBlock, s_samplerDescMemBlock;
static dk::ImageDescriptor* s_imageDescriptors;
static dk::SamplerDescriptor* s_samplerDescriptors;

static Texture loadTexture(const unsigned char* pngData, unsigned pngSize, int slot)
{
    int width, height, nchan;
    stbi_set_flip_vertically_on_load(true);
    stbi_uc* img = stbi_load_from_memory(pngData, pngSize, &width, &height, &nchan, 4);

    dk::ImageLayout layout;
    dk::ImageLayoutMaker{s_device}
        .setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(width, height)
        .initialize(layout);
    
    uint32_t size = layout.getSize();
    uint32_t align = layout.getAlignment();
    uint32_t offset = (s_imagePoolOffset + align - 1) & ~(align - 1);

    Texture tex;
    tex.image.initialize(layout, s_imagePool, offset);
    s_imagePoolOffset = offset + size;

    // Upload textures
    dk::UniqueMemBlock staging = dk::MemBlockMaker{s_device, (size + 4095) & ~4095u}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();
    memcpy(staging.getCpuAddr(), img, width * height * 4);
    stbi_image_free(img);

    dk::ImageView dstView{tex.image};
    DkCopyBuf scrBuf{};
    scrBuf.addr = staging.getGpuAddr();
    scrBuf.rowLength = 0;
    scrBuf.imageHeight = 0;
    s_cmdbuf.copyBufferToImage(scrBuf, dstView, {0, 0, 0, (uint32_t)width, (uint32_t)height, 1});
    s_queue.submitCommands(s_cmdbuf.finishList());
    s_queue.waitIdle();

    // Final descriptors and sampler
    s_imageDescriptors[slot].initialize(dstView);
    s_samplerDescriptors[slot].initialize(
        dk::Sampler{}
            .setFilter(DkFilter_Linear, DkFilter_Linear)
            .setWrapMode(DkWrapMode_Repeat, DkWrapMode_Repeat, DkWrapMode_Repeat)
    );
    tex.handle = dkMakeTextureHandle(slot, slot);
    return tex; 
}

// Uniform buffers

struct FurParams
{
    float u_resolution[2];
    float u_time;
    float _pad;
};

static uint32_t s_uboOffset;
static uint32_t s_uboSize = (sizeof(FurParams) + 255) & ~255u;

static u64 s_startTicks;

float getTimed()
{
    u64 elapsed = armGetSystemTick() - s_startTicks;
    return (elapsed * 625 / 12) / 2000000000.0;
}

// I fucking love low level languages
void frdSceneInit()
{
    s_device = dk::DeviceMaker{}.create();
    s_queue = dk::QueueMaker{s_device}.setFlags(DkQueueFlags_Graphics).create();

    // CMD Buffer
    s_cmdbufMemBlock = dk::MemBlockMaker{s_device, CMDBUF_SIZE}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached)
        .create();
    s_cmdbuf = dk::CmdBufMaker{s_device}.create();
    s_cmdbuf.addMemory(s_cmdbufMemBlock, 0, CMDBUF_SIZE);

    // Swapchain buffers
    dk::ImageLayout fbLayout;
    dk::ImageLayoutMaker{s_device}
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(FB_WIDTH, FB_HEIGHT)
        .initialize(fbLayout);

    uint32_t fbSize  = fbLayout.getSize();
    uint32_t fbAlign = fbLayout.getAlignment();
    fbSize = (fbSize + fbAlign - 1) & ~(fbAlign - 1);

    s_fbMemBlock = dk::MemBlockMaker{s_device, fbSize * NUM_FRAMEBUFFERS}
        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();
    
    DkImage const* swapchainImages[NUM_FRAMEBUFFERS];
    for (unsigned i = 0; i < NUM_FRAMEBUFFERS; i++)
    {
        s_framebuffers[i].initialize(fbLayout, s_fbMemBlock, fbSize * i);
        swapchainImages[i] = &s_framebuffers[i];
    }
    s_swapchain = dk::SwapchainMaker{s_device, nwindowGetDefault(), swapchainImages, NUM_FRAMEBUFFERS}.create();    


    // Texture and shared data pools
    s_imagePool = dk::MemBlockMaker{s_device, 16 * 1024 * 1024}
        .setFlags(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image)
        .create();
    s_dataPool = dk::MemBlockMaker{s_device, 1 * 1024 * 1024}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
        .create();
    // Align descriptor sizes properly
    uint32_t imageDescSize   = (3 * sizeof(dk::ImageDescriptor)   + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
    uint32_t samplerDescSize = (3 * sizeof(dk::SamplerDescriptor) + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);

    // Directly reserve space 3 image and sampler descriptors, aligned properly this time
    s_imageDescMemBlock = dk::MemBlockMaker{s_device, imageDescSize}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached).create();
    s_samplerDescMemBlock = dk::MemBlockMaker{s_device, samplerDescSize}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached).create();
    s_imageDescriptors   = (dk::ImageDescriptor*)s_imageDescMemBlock.getCpuAddr();
    s_samplerDescriptors = (dk::SamplerDescriptor*)s_samplerDescMemBlock.getCpuAddr();


    // Load shaders boys
    loadShader(s_furVertexShader,   s_furVshCode, "romfs:/shaders/FRDK3D_vsh.dksh");
    loadShader(s_furFragmentShader, s_furFshCode, "romfs:/shaders/FRDK3D_fsh.dksh");

    // Load textures
    s_tex1 = loadTexture(fur_png, fur_png_size, 0);
    s_tex2 = loadTexture(noise_png, noise_png_size, 1);
    s_tex3 = loadTexture(wall_png, wall_png_size, 2);

    // Reserve UBO Space
    s_uboOffset = s_dataPoolOffset;
    s_dataPoolOffset += s_uboSize;

    s_startTicks = armGetSystemTick();

    // Please do not use the FPS meter, I don't have an animation for it
}

// Me fockin ROPs be missing
static u64 s_lastFrameTime = 0, s_fpsUpdateTime = 0;
static float s_fps = 0.0f;
static int s_frameCount = 0;

void frdRender()
{
    u64 currentTime = armGetSystemTick();
    s_frameCount++;
    u64 timeSinceUpdate = currentTime - s_fpsUpdateTime;
    float secondsSinceUpdate = (timeSinceUpdate * 625.0f / 12.0f) / 1000000000.0f;
    if (secondsSinceUpdate >= 0.01f)
    {
        s_fps = s_frameCount / secondsSinceUpdate;
        s_frameCount = 0;
        s_fpsUpdateTime = currentTime;
    }

    int slot = s_queue.acquireImage(s_swapchain);

    // Write frame uniforms
    FurParams params;
    params.u_resolution[0] = (float)FB_WIDTH;
    params.u_resolution[1] = (float)FB_HEIGHT;
    params.u_time = getTimed();
    memcpy((uint8_t*)s_dataPool.getCpuAddr() + s_uboOffset, &params, sizeof(params));

    s_cmdbuf.clear();

    s_cmdbuf.bindImageDescriptorSet(s_imageDescMemBlock.getGpuAddr(), 3);
    s_cmdbuf.bindSamplerDescriptorSet(s_samplerDescMemBlock.getGpuAddr(), 3);

    dk::ImageView fbView{s_framebuffers[slot]};
    s_cmdbuf.bindRenderTargets(&fbView);

    DkViewport viewport{0.0f, 0.0f, (float)FB_WIDTH, (float)FB_HEIGHT};
    DkScissor  scissor{0, 0, FB_WIDTH, FB_HEIGHT};
    s_cmdbuf.setViewports(0, viewport);
    s_cmdbuf.setScissors(0, scissor);

    s_cmdbuf.clearColor(0, DkColorMask_RGBA, 0.2f, 0.3f, 0.3f, 1.0f);

    // Disable depth test and culling because lmao
    s_cmdbuf.bindDepthStencilState(dk::DepthStencilState{}.setDepthTestEnable(false));
    s_cmdbuf.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
    s_cmdbuf.bindColorState(dk::ColorState{});
    s_cmdbuf.bindColorWriteState(dk::ColorWriteState{});

    s_cmdbuf.bindShaders(DkStageFlag_GraphicsMask, { &s_furVertexShader, &s_furFragmentShader});
    s_cmdbuf.bindUniformBuffer(DkStage_Fragment, 0, s_dataPool.getGpuAddr() + s_uboOffset, s_uboSize);

    DkResHandle texHandles[3] = { s_tex1.handle, s_tex2.handle, s_tex3.handle };
    s_cmdbuf.bindTextures(DkStage_Fragment, 0, {texHandles[0], texHandles[1], texHandles[2]});

    // Draw this as a triangle
    s_cmdbuf.draw(DkPrimitive_Triangles, 3, 1, 0, 0);

    s_queue.submitCommands(s_cmdbuf.finishList());
    s_queue.presentImage(s_swapchain, slot);

    // So funny story, we need to wait for the GPU for this to work.
    // Can we kind of go around this, yes, can I care right now? no.
    // So we just have to wait for an idle so memory doesn't get overwritten
    s_queue.waitIdle();

    // OH BROTHER THIS GUY STINKS
    (void)s_fps;
}
void frdExit()
{
    s_queue.waitIdle();
    s_cmdbuf.destroy();
    s_swapchain.destroy();
    s_fbMemBlock.destroy();
    s_imagePool.destroy();
    s_dataPool.destroy();
    s_imageDescMemBlock.destroy();
    s_samplerDescMemBlock.destroy();
    s_furVshCode.destroy();
    s_furFshCode.destroy();
    s_cmdbufMemBlock.destroy();
    s_queue.destroy();
    s_device.destroy();
}
int frdMain(int argc, char* argv[])
{
    frdSceneInit();

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    while (appletMainLoop())
    {
        padUpdate(&pad);
        u32 kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_B)
        {
            state = STATE_MENU;
            return 0;
        }

        frdRender();
    }

    frdExit();
    return EXIT_SUCCESS;
}