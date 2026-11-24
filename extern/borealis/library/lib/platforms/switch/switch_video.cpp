/*
Copyright 2020 Adubbz
Copyright 2020-2021 Luis Scheurenbrand
Copyright 2021 natinusala

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <borealis/core/application.hpp>
#include <borealis/platforms/switch/switch_video.hpp>

#ifdef __SWITCH2__

#include <GLES3/gl3.h>
#define NANOVG_GLES3_IMPLEMENTATION
#include <nanovg_gl.h>
#include <cmath>

#define HANDHELD_WIDTH 1280.0f
#define HANDHELD_HEIGHT 720.0f

#define DOCKED_WIDTH 1920.f
#define DOCKED_HEIGHT 1080.f

namespace brls
{

SwitchVideoContext::SwitchVideoContext()
{
    Result rc = appletGetDefaultDisplayResolutionChangeEvent(&this->defaultDisplayResolutionChangeEvent);
    if (R_FAILED(rc))
    {
        Logger::warning(fmt::runtime("switch2: failed to get display resolution change event ({:#x}), falling back to operation mode"), rc);
        this->displayResolutionChangeEventReady = false;
    }

    this->updateWindowSize();

    this->eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (this->eglDisplay == EGL_NO_DISPLAY || !eglInitialize(this->eglDisplay, nullptr, nullptr))
    {
        fatal(fmt::format("switch2: eglInitialize failed (0x{:x})", eglGetError()));
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint configAttributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint configCount = 0;
    if (!eglChooseConfig(this->eglDisplay, configAttributes, &config, 1, &configCount) || configCount == 0)
    {
        fatal(fmt::format("switch2: eglChooseConfig failed (0x{:x})", eglGetError()));
    }

    this->eglSurface = eglCreateWindowSurface(this->eglDisplay, config, nwindowGetDefault(), nullptr);
    if (this->eglSurface == EGL_NO_SURFACE)
    {
        fatal(fmt::format("switch2: eglCreateWindowSurface failed (0x{:x})", eglGetError()));
    }

    const EGLint contextAttributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_NONE,
    };
    this->eglContext = eglCreateContext(this->eglDisplay, config, EGL_NO_CONTEXT, contextAttributes);
    if (this->eglContext == EGL_NO_CONTEXT)
    {
        fatal(fmt::format("switch2: eglCreateContext failed (0x{:x})", eglGetError()));
    }

    if (!eglMakeCurrent(this->eglDisplay, this->eglSurface, this->eglSurface, this->eglContext))
    {
        fatal(fmt::format("switch2: eglMakeCurrent failed (0x{:x})", eglGetError()));
    }

    Logger::info("switch2: GL Vendor: {}", reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    Logger::info("switch2: GL Renderer: {}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    Logger::info("switch2: GL Version: {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    this->nvgContext = nvgCreateGLES3(NVG_STENCIL_STROKES | NVG_ANTIALIAS);
    if (!this->nvgContext)
    {
        fatal("switch2: nvgCreateGLES3 failed");
    }

    glViewport(0, 0, static_cast<GLsizei>(this->framebufferWidth), static_cast<GLsizei>(this->framebufferHeight));
    Application::setWindowSize(roundf(this->framebufferWidth), roundf(this->framebufferHeight));
    setSwapInterval(VideoContext::swapInterval);
}

void SwitchVideoContext::updateWindowSize()
{
    int32_t width = 0, height = 0;
    Result rc = appletGetDefaultDisplayResolution(&width, &height);
    if (R_SUCCEEDED(rc) && width > 0 && height > 0)
    {
        this->framebufferWidth  = static_cast<float>(width);
        this->framebufferHeight = static_cast<float>(height);
    }
    else
    {
        switch (appletGetOperationMode())
        {
            case AppletOperationMode_Handheld:
                this->framebufferWidth  = HANDHELD_WIDTH;
                this->framebufferHeight = HANDHELD_HEIGHT;
                break;
            case AppletOperationMode_Console:
                this->framebufferWidth  = DOCKED_WIDTH;
                this->framebufferHeight = DOCKED_HEIGHT;
                break;
            default:
                return;
        }
    }

    nwindowSetDimensions(nwindowGetDefault(),
        static_cast<u32>(roundf(this->framebufferWidth)),
        static_cast<u32>(roundf(this->framebufferHeight)));

    if (this->eglContext != EGL_NO_CONTEXT)
    {
        glViewport(0, 0,
            static_cast<GLsizei>(roundf(this->framebufferWidth)),
            static_cast<GLsizei>(roundf(this->framebufferHeight)));
    }

    Application::onWindowResized(roundf(this->framebufferWidth), roundf(this->framebufferHeight));
}

void SwitchVideoContext::clear(NVGcolor color)
{
    glClearColor(color.r, color.g, color.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void SwitchVideoContext::resetState()
{
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
}

void SwitchVideoContext::resetFramebuffer()
{
    Logger::info("switch2: resetFramebuffer");
    this->updateWindowSize();
}

void SwitchVideoContext::beginFrame()
{
    if (this->displayResolutionChangeEventReady &&
        R_SUCCEEDED(eventWait(&this->defaultDisplayResolutionChangeEvent, 0)))
    {
        this->resetFramebuffer();
    }

    // The GLES3 video renderer changes glViewport() while a stream is
    // playing, so restore the real surface viewport before NanoVG draws.
    // Query the EGL surface itself: the applet default resolution can be
    // ahead of the actual surface after a dock/undock change, which would
    // otherwise scale both the UI and the video beyond the screen frame.
    int width = 0;
    int height = 0;
    if (this->eglDisplay != EGL_NO_DISPLAY && this->eglSurface != EGL_NO_SURFACE)
    {
        eglQuerySurface(this->eglDisplay, this->eglSurface, EGL_WIDTH, &width);
        eglQuerySurface(this->eglDisplay, this->eglSurface, EGL_HEIGHT, &height);
    }
    if (width <= 0 || height <= 0)
    {
        width = static_cast<int>(roundf(this->framebufferWidth));
        height = static_cast<int>(roundf(this->framebufferHeight));
    }
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
}

void SwitchVideoContext::endFrame()
{
    if (frameCapture)
    {
        auto capture = std::move(frameCapture);
        frameCapture = {};
        capture();
    }
    eglSwapBuffers(this->eglDisplay, this->eglSurface);
}

void SwitchVideoContext::setSwapInterval(int interval)
{
    VideoContext::swapInterval = interval;
    if (this->eglDisplay != EGL_NO_DISPLAY)
        eglSwapInterval(this->eglDisplay, interval);
}

void SwitchVideoContext::appletCallback(AppletHookType hookType)
{
    if (hookType == AppletHookType_OnOperationMode && !this->displayResolutionChangeEventReady)
        resetFramebuffer();
}

NVGcontext* SwitchVideoContext::getNVGContext()
{
    return this->nvgContext;
}

SwitchVideoContext::~SwitchVideoContext()
{
    if (this->nvgContext)
    {
        nvgDeleteGLES3(this->nvgContext);
        this->nvgContext = nullptr;
    }
    if (this->eglDisplay != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(this->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (this->eglContext != EGL_NO_CONTEXT)
            eglDestroyContext(this->eglDisplay, this->eglContext);
        if (this->eglSurface != EGL_NO_SURFACE)
            eglDestroySurface(this->eglDisplay, this->eglSurface);
        eglTerminate(this->eglDisplay);
        this->eglDisplay = EGL_NO_DISPLAY;
    }
}

double SwitchVideoContext::getScaleFactor()
{
    return 1.0;
}

} // namespace brls

#else // !__SWITCH2__

// nanovg implementation
#include <nanovg_dk.h>

#define HANDHELD_WIDTH 1280.0f
#define HANDHELD_HEIGHT 720.0f

#define DOCKED_WIDTH 1920.f
#define DOCKED_HEIGHT 1080.f

#define IMAGES_POOL_SIZE 4 * 1024 * 1024
#define CODE_POOL_SIZE 128 * 1024
#define DATA_POOL_SIZE 1 * 1024 * 1024

namespace brls
{

static constexpr const unsigned STATIC_CMD_SIZE = 0x1000;

SwitchVideoContext::SwitchVideoContext()
{
    // Get display resolution change event
    Result rc = appletGetDefaultDisplayResolutionChangeEvent(&this->defaultDisplayResolutionChangeEvent);

    if (R_FAILED(rc))
    {
        Logger::warning(fmt::runtime("switch: failed to get default display resolution change event ({:#x}), falling back to operation mode change event"), rc);
        Logger::warning(fmt::runtime("switch: resolution might not change properly when docking while the app is running using 3rd party docks!"));
        this->displayResolutionChangeEventReady = false; // default is true
    }

    // Setup scaling
    this->updateWindowSize();

    // Init deko
    this->device = dk::DeviceMaker {}.create();
    this->queue  = dk::QueueMaker(this->device)
        // Give this queue a high priority to help render the ui smoothly even if libmpv is hogging the gpu
        .setFlags(DkQueueFlags_Graphics | DkQueueFlags_DisableZcull | DkQueueFlags_HighPrio)
        .create();

    this->imagesPool.emplace(device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, IMAGES_POOL_SIZE);
    this->codePool.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, CODE_POOL_SIZE);
    this->dataPool.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, DATA_POOL_SIZE);

    this->cmdbuf            = dk::CmdBufMaker { device }.create();
    CMemPool::Handle cmdmem = this->dataPool->allocate(STATIC_CMD_SIZE);
    cmdbuf.addMemory(cmdmem.getMemBlock(), cmdmem.getOffset(), cmdmem.getSize());

    // Create framebuffer resources
    this->createFramebufferResources();

    // Create renderer and nvg context
    this->renderer.emplace(this->framebufferWidth, this->framebufferHeight, this->device, this->queue, *this->imagesPool, *this->codePool, *this->dataPool);
    this->nvgContext = nvgCreateDk(&*this->renderer, NVG_ANTIALIAS | NVG_STENCIL_STROKES);

    Application::setWindowSize(this->framebufferWidth, this->framebufferHeight);

    setSwapInterval(VideoContext::swapInterval);
}

void SwitchVideoContext::createFramebufferResources()
{
    // Create layout for the depth buffer
    dk::ImageLayout depthBufferLayout;
    dk::ImageLayoutMaker { this->device }
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
        .setFormat(DkImageFormat_S8)
        .setDimensions(this->framebufferWidth, this->framebufferHeight)
        .initialize(depthBufferLayout);

    // Create the depth buffer
    this->depthBufferHandle = this->imagesPool->allocate(depthBufferLayout.getSize(), depthBufferLayout.getAlignment());
    this->depthBuffer.initialize(depthBufferLayout, depthBufferHandle.getMemBlock(), depthBufferHandle.getOffset());

    // Create layout for the framebuffers
    dk::ImageLayout framebufferLayout;
    dk::ImageLayoutMaker { device }
        .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine)
        .setFormat(DkImageFormat_RGBA8_Unorm)
        .setDimensions(this->framebufferWidth, this->framebufferHeight)
        .initialize(framebufferLayout);

    // Create the framebuffers
    std::array<DkImage const*, FRAMEBUFFERS_COUNT> fbArray;
    uint64_t fbSize  = framebufferLayout.getSize();
    uint32_t fbAlign = framebufferLayout.getAlignment();

    for (unsigned i = 0; i < FRAMEBUFFERS_COUNT; i++)
    {
        // Allocate a framebuffer
        framebuffersHandles[i] = this->imagesPool->allocate(fbSize, fbAlign);
        this->framebuffers[i].initialize(framebufferLayout, framebuffersHandles[i].getMemBlock(), framebuffersHandles[i].getOffset());

        // Generate a command list that binds it
        dk::ImageView colorTarget { this->framebuffers[i] }, depthTarget { depthBuffer };
        this->cmdbuf.bindRenderTargets(&colorTarget, &depthTarget);
        framebuffersCmdLists[i] = this->cmdbuf.finishList();

        // Fill in the array for use later by the swapchain creation code
        fbArray[i] = &this->framebuffers[i];
    }

    // Create the swapchain using the framebuffers
    this->swapchain = dk::SwapchainMaker { this->device, nwindowGetDefault(), fbArray }.create();

    // Generate the main rendering cmdlist
    this->recordStaticCommands();
}

void SwitchVideoContext::recordStaticCommands()
{
    // This method should always be called AFTER initializing the theme variant
    // since the static clear command uses the theme background color

    // Initialize state structs with deko3d defaults
    dk::RasterizerState rasterizerState;
    dk::ColorState colorState;
    dk::ColorWriteState colorWriteState;
    dk::BlendState blendState;

    // Configure the viewport and scissor
    this->cmdbuf.setViewports(0, { { 0.0f, 0.0f, this->framebufferWidth, this->framebufferHeight, 0.0f, 1.0f } });
    this->cmdbuf.setScissors(0, { { 0, 0, static_cast<uint32_t>(this->framebufferWidth), static_cast<uint32_t>(this->framebufferHeight) } });

    // Clear the color and depth buffers
    Theme theme              = Application::getTheme();
    NVGcolor backgroundColor = theme["brls/background"];
    this->cmdbuf.clearColor(0, DkColorMask_RGBA, backgroundColor.r, backgroundColor.g, backgroundColor.b, 1.0f);

    this->cmdbuf.clearDepthStencil(true, 1.0f, 0xFF, 0);

    // Bind required state
    this->cmdbuf.bindRasterizerState(rasterizerState);
    this->cmdbuf.bindColorState(colorState);
    this->cmdbuf.bindColorWriteState(colorWriteState);

    this->renderCmdlist = cmdbuf.finishList();
}

void SwitchVideoContext::updateWindowSize()
{
    // First try to get actual resolution with GetDefaultDisplayResolution
    int32_t width, height;
    Result rc = appletGetDefaultDisplayResolution(&width, &height);
    if (R_SUCCEEDED(rc))
    {
        this->framebufferWidth  = (float)width;
        this->framebufferHeight = (float)height;
    }
    // If it failed, use hardcoded resolutions as fallback
    else
    {
        Logger::warning(fmt::runtime("switch: failed to detect display resolution ({:#x}), falling back to hardcoded resolution"));

        switch (appletGetOperationMode())
        {
            case AppletOperationMode_Handheld:
                this->framebufferWidth  = HANDHELD_WIDTH;
                this->framebufferHeight = HANDHELD_HEIGHT;
                break;
            case AppletOperationMode_Console:
                this->framebufferWidth  = DOCKED_WIDTH;
                this->framebufferHeight = DOCKED_HEIGHT;
                break;
            default:
                return;
        }
    }

    Application::onWindowResized(roundf(this->framebufferWidth), roundf(this->framebufferHeight));

    if (this->renderer.has_value())
        this->renderer->UpdateViewBounds(this->framebufferWidth, this->framebufferHeight);
}

void SwitchVideoContext::clear(NVGcolor color)
{
    // Unimplemented here (clear is made using a static command ran every frame)
}

void SwitchVideoContext::resetState()
{
    // Unimplemented
}

void SwitchVideoContext::resetFramebuffer()
{
    brls::Logger::error("resetFramebuffer");
    this->destroyFramebufferResources();
    this->updateWindowSize();
    this->createFramebufferResources();
}

void SwitchVideoContext::beginFrame()
{
    // Poll the display resolution change event
    if (this->displayResolutionChangeEventReady && R_SUCCEEDED(eventWait(&this->defaultDisplayResolutionChangeEvent, 0)))
        this->resetFramebuffer();

    this->imageSlot = this->queue.acquireImage(this->swapchain);

    // Run the command list that attaches said framebuffer to the queue
    this->queue.submitCommands(this->framebuffersCmdLists[this->imageSlot]);

    // Run the main rendering command list
    queue.submitCommands(this->renderCmdlist);
}

void SwitchVideoContext::endFrame()
{
    if (frameCapture)
    {
        auto capture = std::move(frameCapture);
        frameCapture = {};
        capture();
    }
    // Now that we are done rendering, present it to the screen
    queue.presentImage(this->swapchain, this->imageSlot);
}

void SwitchVideoContext::setSwapInterval(int interval)
{
    VideoContext::swapInterval = interval;
    swapchain.setSwapInterval(interval);
}

void SwitchVideoContext::destroyFramebufferResources()
{
    // Return early if we have nothing to destroy
    if (!this->swapchain)
        return;

    // Make sure the queue is idle before destroying anything
    this->queue.waitIdle();

    // Clear the static cmdbuf, destroying the static cmdlists in the process
    this->cmdbuf.clear();

    // Destroy the swapchain
    this->swapchain.destroy();

    // Destroy the framebuffers
    for (unsigned i = 0; i < FRAMEBUFFERS_COUNT; i++)
        framebuffersHandles[i].destroy();

    // Destroy the depth buffer
    depthBufferHandle.destroy();
}

void SwitchVideoContext::appletCallback(AppletHookType hookType)
{
    // Only reset framebuffer if the display resolution change event isn't ready
    if (hookType == AppletHookType_OnOperationMode && !this->displayResolutionChangeEventReady)
        resetFramebuffer();
}

NVGcontext* SwitchVideoContext::getNVGContext()
{
    return this->nvgContext;
}

SwitchVideoContext::~SwitchVideoContext()
{
    // Destroy the framebuffer resources. This should be done first.
    this->destroyFramebufferResources();

    // Cleanup vg. This needs to be done first as it relies on the renderer.
    nvgDeleteDk(this->nvgContext);

    // Destroy the renderer
    this->renderer.reset();
}

double SwitchVideoContext::getScaleFactor()
{
    return 1.0;
}

dk::Device SwitchVideoContext::getDeko3dDevice() { return device; }

dk::Queue SwitchVideoContext::getQueue() { return queue; }

dk::Image* SwitchVideoContext::getFramebuffer()
{
    return &framebuffers[imageSlot];
}

int SwitchVideoContext::allocateImageIndex() { return renderer->AllocateImageIndex(); }

bool SwitchVideoContext::freeImageIndex(int index) { return renderer->FreeImageIndex(index); }

bool SwitchVideoContext::updateImageDescriptor(dk::CmdBuf cmdbuf, int index, dk::ImageDescriptor const& descriptor)
{
    return renderer->UpdateImageDescriptor(cmdbuf, index, descriptor);
}

void SwitchVideoContext::invalidateImageDescriptors(dk::CmdBuf cmdbuf)
{
    renderer->InvalidateImageDescriptors(cmdbuf);
}

void SwitchVideoContext::queueWaitFence(DkFence* fence)
{
    dkQueueWaitFence(this->queue, fence);
}

void SwitchVideoContext::queueSignalFence(DkFence* fence, bool flash)
{
    dkQueueSignalFence(this->queue, fence, flash);
}

void SwitchVideoContext::queueFlush()
{
    dkQueueFlush(this->queue);
}

} // namespace brls

#endif // !__SWITCH2__
