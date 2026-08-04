#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/errno.h>
#include <unistd.h>


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

extern "C" void userAppInit() { 
    initNxLink(); 
    Result rc = romfsInit();
    if (R_FAILED(rc)) {
        TRACE("Failed to initialize RomFS: %08X", rc);
    }
}

extern "C" void userAppExit() { 
    romfsExit();
    deinitNxLink(); 
}
#endif

// Alright boys after not having a red bull in 3 months its DK3D time
// Globals
// Tripe buffer your shit becasue I was lazy and I didn't do this.
// As a result, this ran like shit and didn't use up the GPU entirely.
// 
static const unsigned NUM_FRAMEBUFFERS = 3;
static const unsigned FB_WIDTH = 1280;
static const unsigned FB_HEIGHT = 720;
static const unsigned CMDBUF_SIZE = 256 * 1024;

static dk::UniqueDevice s_device;
static dk::UniqueQueue s_queue;
static dk::UniqueSwapchain s_swapchain;

static dk::UniqueCmdBuf s_cmdbuf;
static dk::UniqueMemBlock s_cmdbufMemBlock;

// Sharing memory is mid but it causes issues like having to wait for the CPU or GPU to sync to each other
// That basically leads to random stalls because this is dumb.
// So instead we allocate an extra buffer so any needed swapchain ops can run on a different buffer
static dk::UniqueCmdBuf s_renderCmdbufs[NUM_FRAMEBUFFERS];
static dk::UniqueMemBlock s_renderCmdbufMemBlocks[NUM_FRAMEBUFFERS];
static dk::Fence s_frameFences[NUM_FRAMEBUFFERS];
static bool s_frameFenceValid[NUM_FRAMEBUFFERS] = {};

static dk::Image s_framebuffers[NUM_FRAMEBUFFERS];
static dk::UniqueMemBlock s_fbMemBlock;

// Memory pool allocation, one for GPU cached and another for CPU visible data
static dk::UniqueMemBlock s_imagePool;
static dk::UniqueMemBlock s_dataPool;

static uint32_t s_dataPoolOffset = 0;
static uint32_t s_imagePoolOffset = 0;

static uint32_t s_uboOffsets[NUM_FRAMEBUFFERS];

// Link shaders
static dk::Shader s_furVertexShader, s_furFragmentShader;
static dk::UniqueMemBlock s_furVshCode, s_furFshCode;

// Generic shader loader from romfs
static void loadShader(dk::Shader& shader, dk::UniqueMemBlock& outMemBlock, const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { TRACE("Failed to open shader %s", path);  abort(); }
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

// Comp the actual sizes needed for textures instead of guessing
static uint32_t computeImagePoolSize()
{
    const unsigned char* pngs[3]   = { fur_png, noise_png, wall_png };
    unsigned              sizes[3] = { fur_png_size, noise_png_size, wall_png_size };

    uint32_t total = 0;
    for (int i = 0; i < 3; i++)
    {
        int width, height, nchan;
        // Header-only decode no pixel data.
        stbi_info_from_memory(pngs[i], sizes[i], &width, &height, &nchan);

        dk::ImageLayout layout;
        dk::ImageLayoutMaker{s_device}
            .setFlags(DkImageFlags_UsageLoadStore | DkImageFlags_HwCompression)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(width, height)
            .initialize(layout);

        uint32_t align  = layout.getAlignment();
        uint32_t offset = (total + align - 1) & ~(align - 1);
        total = offset + layout.getSize();
    }
    // Small headroom for alignment  between textures, rounded somewhat
    return (total + 4095u) & ~4095u;

}

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
    Result rc = romfsInit();
    if (R_FAILED(rc)) {
        TRACE("Failed to initialize RomFS: %08X", rc);
    }
    s_device = dk::DeviceMaker{}.create();
    s_queue = dk::QueueMaker{s_device}.setFlags(DkQueueFlags_Graphics).create();

    // CMD Buffer (only used once now)
    s_cmdbufMemBlock = dk::MemBlockMaker{s_device, CMDBUF_SIZE}
        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached)
        .create();
    s_cmdbuf = dk::CmdBufMaker{s_device}.create();
    s_cmdbuf.addMemory(s_cmdbufMemBlock, 0, CMDBUF_SIZE);

    // One command buffer per swapchain slot for the actual render loop
    for (unsigned i = 0; i < NUM_FRAMEBUFFERS; i++)
    {
        s_uboOffsets[i] = s_dataPoolOffset;
        s_dataPoolOffset += s_uboSize;
        s_renderCmdbufMemBlocks[i] = dk::MemBlockMaker{s_device, CMDBUF_SIZE}
            .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached)
            .create();
        s_renderCmdbufs[i] = dk::CmdBufMaker{s_device}.create();
        s_renderCmdbufs[i].addMemory(s_renderCmdbufMemBlocks[i], 0, CMDBUF_SIZE);
    }

    // Da creechur (fuck vsync)a
     nwindowSetSwapInterval(nwindowGetDefault(), 0);


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
    s_imagePool = dk::MemBlockMaker{s_device, computeImagePoolSize()}
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

    // Bind descriptors in this spot, only done at init and flush here.
    s_cmdbuf.bindImageDescriptorSet(s_imageDescMemBlock.getGpuAddr(), 3);
    s_cmdbuf.bindSamplerDescriptorSet(s_samplerDescMemBlock.getGpuAddr(), 3);
    s_queue.submitCommands(s_cmdbuf.finishList());
    s_queue.waitIdle();


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

    // IF this slot's frame is still in use for some goddamn reason, then wait
    // We could do without this check but if a cosmic ray tells me to go fuck myself then we need this to not hang
    if (s_frameFenceValid[slot])
        s_frameFences[slot].wait();

    dk::CmdBuf& cmdbuf = s_renderCmdbufs[slot];


    // Write frame uniforms
    FurParams params;
    params.u_resolution[0] = (float)FB_WIDTH;
    params.u_resolution[1] = (float)FB_HEIGHT;
    params.u_time = getTimed();
    memcpy((uint8_t*)s_dataPool.getCpuAddr() + s_uboOffsets[slot], &params, sizeof(params));

    cmdbuf.clear();

    dk::ImageView fbView{s_framebuffers[slot]};
    cmdbuf.bindRenderTargets(&fbView);

    DkViewport viewport{0.0f, 0.0f, (float)FB_WIDTH, (float)FB_HEIGHT};
    DkScissor  scissor{0, 0, FB_WIDTH, FB_HEIGHT};
    cmdbuf.setViewports(0, viewport);
    cmdbuf.setScissors(0, scissor);

    cmdbuf.clearColor(0, DkColorMask_RGBA, 0.2f, 0.3f, 0.3f, 1.0f);

    // Disable depth test and culling because lmao
    cmdbuf.bindDepthStencilState(dk::DepthStencilState{}.setDepthTestEnable(false));
    cmdbuf.bindRasterizerState(dk::RasterizerState{}.setCullMode(DkFace_None));
    cmdbuf.bindColorState(dk::ColorState{});
    cmdbuf.bindColorWriteState(dk::ColorWriteState{});

    cmdbuf.bindShaders(DkStageFlag_GraphicsMask, { &s_furVertexShader, &s_furFragmentShader});
    cmdbuf.bindUniformBuffer(DkStage_Fragment, 0, s_dataPool.getGpuAddr() + s_uboOffsets[slot], s_uboSize);

    DkResHandle texHandles[3] = { s_tex1.handle, s_tex2.handle, s_tex3.handle };
    cmdbuf.bindTextures(DkStage_Fragment, 0, {texHandles[0], texHandles[1], texHandles[2]});

    // Draw this as a triangle
    cmdbuf.draw(DkPrimitive_Triangles, 3, 1, 0, 0);

    // Signal that this slot is fine to overwrite
    cmdbuf.signalFence(s_frameFences[slot]);
    s_frameFenceValid[slot] = true;

    s_queue.submitCommands(cmdbuf.finishList());
    s_queue.presentImage(s_swapchain, slot);

    // Timings for debug


    // OH BROTHER THIS GUY STINKS
    (void)s_fps;
}
void frdExit()
{


    s_queue.waitIdle();
    s_cmdbuf.destroy();
    for (unsigned i = 0; i < NUM_FRAMEBUFFERS; i++)
    {
        s_renderCmdbufs[i].destroy();
        s_renderCmdbufMemBlocks[i].destroy();
        s_frameFenceValid[i] = false;
    }

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