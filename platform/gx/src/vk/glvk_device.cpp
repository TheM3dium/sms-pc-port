// glvk: instance, device, command segments, memory rings, images and the
// swapchain.
#include "glvk.h"
#include "glvk_impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <deque>

#ifdef SMS_GX_HAVE_SDL2
#include <SDL.h>
#include <SDL_vulkan.h>
#endif
#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace gx {
std::string userCacheDir();
}

namespace glvk {

Device D;
Ring g_staging, g_dynamic;
Stats g_stats;

double now() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}
Image* g_backbuffer = nullptr;

namespace {

bool s_active = false;

// ------------------------------------------------------------------ segments
struct Segment {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer upload = VK_NULL_HANDLE, main = VK_NULL_HANDLE;
    uint64_t serial = 0;
    bool uploadBegun = false;
};
std::vector<Segment*> s_idle;
std::deque<Segment*> s_busy;
Segment* s_cur = nullptr;

struct Garbage {
    uint64_t serial;
    std::function<void()> fn;
};
std::deque<Garbage> s_garbage;

// ------------------------------------------------------------------ swapchain
enum { kFramesMax = 4 };
struct Swapchain {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> renderDone;
    VkSemaphore acquire[kFramesMax] = {};
    uint64_t frameSerial[kFramesMax] = {};
    uint32_t frame = 0, frames = 2;
    int vsync = 0;
    bool dirty = false;
    bool blitDst = true;
    void* window = nullptr;
};
Swapchain s_sc;
uint32_t s_wantW = 0, s_wantH = 0;  // setDefaultFramebufferSize
uint64_t s_headlessFrames[kFramesMax] = {};
uint32_t s_headlessFrame = 0;
uint32_t s_presents = 0;

bool envTrue(const char* name) {
    const char* v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
    static int count = 0;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && count < 400) {
        count++;
        logmsg("Vulkan validation: %s", data->pMessage);
    }
    return VK_FALSE;
}

// VkPhysicalDevicePortabilitySubsetFeaturesKHR (vulkan_beta.h, which volk is
// not built with): what MoltenVK leaves out.
struct PortabilityFeatures {
    VkStructureType sType;
    void* pNext;
    VkBool32 constantAlphaColorBlendFactors, events, imageViewFormatReinterpretation, imageViewFormatSwizzle,
        imageView2DOn3DImage, multisampleArrayImage, mutableComparisonSamplers, pointPolygons, samplerMipLodBias,
        separateStencilMaskRef, shaderSampleRateInterpolationFunctions, tessellationIsolines, tessellationPointMode,
        triangleFans, vertexAttributeAccessBeyondStride;
};
const VkStructureType kPortabilityFeaturesType = VkStructureType(1000163000);

std::string s_libPath;
bool s_libPathSet = false;

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const VkExtensionProperties& e : list)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

bool fileExists(const std::string& p) {
#ifdef _WIN32
    FILE* f = fopen(p.c_str(), "rb");
    if (f) fclose(f);
    return f != nullptr;
#else
    return access(p.c_str(), R_OK) == 0;
#endif
}

void beginSegment() {
    Segment* s = nullptr;
    pollCompleted();
    while (!s_busy.empty() && s_busy.front()->serial <= D.completed) {
        s_idle.push_back(s_busy.front());
        s_busy.pop_front();
    }
    if (s_idle.empty() && s_busy.size() >= 64) {  // bound the command memory: wait for the oldest
        waitSerial(s_busy.front()->serial);
        s_idle.push_back(s_busy.front());
        s_busy.pop_front();
    }
    if (!s_idle.empty()) {
        s = s_idle.back();
        s_idle.pop_back();
        vkResetCommandPool(D.dev, s->pool, 0);
    } else {
        s = new Segment;
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = D.queueFamily;
        vkCreateCommandPool(D.dev, &pci, nullptr, &s->pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 2;
        VkCommandBuffer cbs[2];
        vkAllocateCommandBuffers(D.dev, &ai, cbs);
        s->upload = cbs[0];
        s->main = cbs[1];
    }
    s->uploadBegun = false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s->main, &bi);
    s_cur = s;
    onSegmentStart();
}

void flushRings() {
    for (Ring* r : {&g_staging, &g_dynamic})
        for (Ring::Block& b : r->blocks)
            if (b.serial == currentSerial()) vmaFlushAllocation(D.vma, b.alloc, 0, b.used);
}

// Ends and submits the segment's command buffers, signalling the next serial
// (and `signal`, after waiting for `wait`, for presenting).
uint32_t s_drawsSinceSubmit = 0;
uint32_t s_flushDraws = 0;  // SMS_VK_FLUSH_DRAWS (0: only at fences and presents)

void submitSegment(VkSemaphore wait, VkPipelineStageFlags waitStage, VkSemaphore signal) {
    const double t0 = now();
    s_drawsSinceSubmit = 0;
    beforeSubmit();
    flushRings();
    // copies into pixel pack buffers are read on the CPU once the serial passes
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(s_cur->main, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
    vkEndCommandBuffer(s_cur->main);
    VkCommandBuffer cbs[2];
    uint32_t n = 0;
    if (s_cur->uploadBegun) {
        vkEndCommandBuffer(s_cur->upload);
        cbs[n++] = s_cur->upload;
    }
    cbs[n++] = s_cur->main;
    const uint64_t serial = D.submitted + 1;
    VkSemaphore signals[2] = {D.timeline, signal};
    uint64_t signalValues[2] = {serial, 0};
    uint64_t waitValues[1] = {0};
    VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.waitSemaphoreValueCount = wait ? 1 : 0;
    ti.pWaitSemaphoreValues = waitValues;
    ti.signalSemaphoreValueCount = signal ? 2 : 1;
    ti.pSignalSemaphoreValues = signalValues;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ti;
    si.waitSemaphoreCount = wait ? 1 : 0;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = n;
    si.pCommandBuffers = cbs;
    si.signalSemaphoreCount = signal ? 2 : 1;
    si.pSignalSemaphores = signals;
    VkResult r = vkQueueSubmit(D.queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        logmsg("Vulkan: vkQueueSubmit failed (%d)%s", int(r), r == VK_ERROR_DEVICE_LOST ? ": the device was lost" : "");
        if (r == VK_ERROR_DEVICE_LOST) abort();
    }
    D.submitted = serial;
    s_cur->serial = serial;
    s_busy.push_back(s_cur);
    s_cur = nullptr;
    beginSegment();
    collectGarbage();
    g_stats.submits++;
    g_stats.submitSeconds += now() - t0;
}

// ------------------------------------------------------------------ pipeline cache file
void loadPipelineCache() {
    std::vector<uint8_t> data;
    const std::string dir = gx::userCacheDir();
    if (!dir.empty() && !(getenv("SMS_GX_SHADER_CACHE") && !strcmp(getenv("SMS_GX_SHADER_CACHE"), "0"))) {
        char name[160];
        snprintf(name, sizeof name, "/vk-pipelines-%04x-%04x-%08x-%d.bin", D.props.vendorID, D.props.deviceID,
                 D.props.driverVersion, int(sizeof(void*) * 8));
        D.pipelineCachePath = dir + name;
        if (FILE* f = fopen(D.pipelineCachePath.c_str(), "rb")) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 32 && n < (256L << 20)) {
                data.resize(size_t(n));
                if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
            }
            fclose(f);
        }
        // the header names the device and driver; a mismatch would be ignored
        // by the driver anyway, but checking spares it the work
        if (data.size() > 32) {
            uint32_t vendor, device;
            memcpy(&vendor, &data[8], 4);
            memcpy(&device, &data[12], 4);
            if (vendor != D.props.vendorID || device != D.props.deviceID || memcmp(&data[16], D.props.pipelineCacheUUID, VK_UUID_SIZE))
                data.clear();
        }
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(D.dev, &ci, nullptr, &D.pipelineCache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(D.dev, &ci, nullptr, &D.pipelineCache);
        data.clear();
    }
    D.pipelineCacheSavedSize = data.size();
    if (!D.pipelineCachePath.empty())
        logmsg("Vulkan pipeline cache: %s (%zu KiB)", D.pipelineCachePath.c_str(), data.size() / 1024);
}

void savePipelineCache(bool force) {
    if (D.pipelineCachePath.empty() || !D.pipelineCache) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(D.dev, D.pipelineCache, &size, nullptr) != VK_SUCCESS) return;
    if (!force && size < D.pipelineCacheSavedSize + (64u << 10)) return;
    if (size <= D.pipelineCacheSavedSize) return;
    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(D.dev, D.pipelineCache, &size, data.data()) != VK_SUCCESS) return;
    const std::string tmp = D.pipelineCachePath + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        const bool ok = fwrite(data.data(), 1, size, f) == size;
        fclose(f);
        if (ok && rename(tmp.c_str(), D.pipelineCachePath.c_str()) == 0) D.pipelineCacheSavedSize = size;
        else remove(tmp.c_str());
    }
}

// ------------------------------------------------------------------ swapchain
VkPresentModeKHR pickPresentMode(int vsync) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(D.phys, s_sc.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(D.phys, s_sc.surface, &n, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    if (vsync == 0) {
        if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
        if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (vsync < 0 && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

bool createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(D.phys, s_sc.surface, &caps) != VK_SUCCESS) return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = s_wantW ? s_wantW : 640;
        extent.height = s_wantH ? s_wantH : 480;
    }
    extent.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, extent.width));
    extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, extent.height));
    if (extent.width == 0 || extent.height == 0) return false;  // minimised
    if (s_sc.format == VK_FORMAT_UNDEFINED) {
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(D.phys, s_sc.surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(D.phys, s_sc.surface, &n, fmts.data());
        s_sc.format = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0].format;
        for (const VkSurfaceFormatKHR& f : fmts)  // GL's default framebuffer is not sRGB
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                s_sc.format = f.format;
                break;
            }
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(D.phys, s_sc.format, &fp);
        s_sc.blitDst = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
        if (!s_sc.blitDst) logmsg("Vulkan: the swapchain format cannot be blitted to; the window shows black");
    }
    uint32_t count = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = s_sc.surface;
    ci.minImageCount = count;
    ci.imageFormat = s_sc.format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                         : caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
        if (caps.supportedCompositeAlpha & a) {
            ci.compositeAlpha = a;
            break;
        }
    ci.presentMode = pickPresentMode(s_sc.vsync);
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = s_sc.handle;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(D.dev, &ci, nullptr, &sc);
    if (s_sc.handle) vkDestroySwapchainKHR(D.dev, s_sc.handle, nullptr);
    s_sc.handle = VK_NULL_HANDLE;
    for (VkSemaphore s : s_sc.renderDone) vkDestroySemaphore(D.dev, s, nullptr);
    s_sc.renderDone.clear();
    if (r != VK_SUCCESS) {
        logmsg("Vulkan: vkCreateSwapchainKHR failed (%d)", int(r));
        return false;
    }
    s_sc.handle = sc;
    s_sc.extent = extent;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(D.dev, sc, &n, nullptr);
    s_sc.images.resize(n);
    vkGetSwapchainImagesKHR(D.dev, sc, &n, s_sc.images.data());
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    s_sc.renderDone.resize(n);
    for (VkSemaphore& s : s_sc.renderDone) vkCreateSemaphore(D.dev, &si, nullptr, &s);
    static const char* const kModes[] = {"immediate", "mailbox", "fifo", "fifo relaxed"};
    logmsg("Vulkan swapchain %ux%u, %u images, present mode %s", extent.width, extent.height, n,
           int(ci.presentMode) < 4 ? kModes[ci.presentMode] : "?");
    s_sc.dirty = false;
    return true;
}

void recreateSwapchain() {
    waitSerial(D.submitted);
    vkQueueWaitIdle(D.queue);
    createSwapchain();
}

}  // namespace

// ------------------------------------------------------------------ serials
uint64_t pollCompleted() {
    uint64_t v = 0;
    if (D.getSemaphoreCounterValue(D.dev, D.timeline, &v) == VK_SUCCESS && v > D.completed) D.completed = v;
    return D.completed;
}

bool serialDone(uint64_t serial) { return serial <= D.completed || serial <= pollCompleted(); }

void waitSerial(uint64_t serial) {
    if (serial <= D.completed) return;
    if (serial > D.submitted) submit();
    if (serial > D.submitted) serial = D.submitted;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &D.timeline;
    wi.pValues = &serial;
    const double t0 = now();
    VkResult r = D.waitSemaphores(D.dev, &wi, 10ull * 1000 * 1000 * 1000);
    g_stats.waitSeconds += now() - t0;
    if (r == VK_TIMEOUT) logmsg("Vulkan: the GPU took over 10 seconds for one segment");
    else if (r != VK_SUCCESS) logmsg("Vulkan: waiting for the GPU failed (%d)", int(r));
    pollCompleted();
}

void submit() { submitSegment(VK_NULL_HANDLE, 0, VK_NULL_HANDLE); }

void drawRecorded() {
    g_stats.draws++;
    if (s_flushDraws && ++s_drawsSinceSubmit >= s_flushDraws) submit();
}

static void statsFrame() {
    static int every = -1;
    if (every < 0) {
        const char* e = getenv("SMS_GX_STATS");
        every = e ? atoi(e) : 0;
    }
    if (every <= 0 || ++g_stats.frames < uint32_t(every)) return;
    const double f = g_stats.frames;
    logmsg("Vulkan stats: %.1f submits/frame (%.2f ms), %.2f ms/frame waiting for the GPU, %.0f draws/frame, "
           "%.1f render passes/frame, %u pipelines made", g_stats.submits / f, g_stats.submitSeconds * 1000.0 / f,
           g_stats.waitSeconds * 1000.0 / f, g_stats.draws / f, g_stats.renderPasses / f, g_stats.pipelines);
    {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(D.vma, budgets);
        const VkPhysicalDeviceMemoryProperties* mp = nullptr;
        vmaGetMemoryProperties(D.vma, &mp);
        size_t ring[2] = {0, 0};
        for (int r = 0; r < 2; r++)
            for (const Ring::Block& b : (r ? g_dynamic : g_staging).blocks) ring[r] += size_t(b.size);
        char heaps[256];
        int len = 0;
        for (uint32_t h = 0; h < mp->memoryHeapCount && len < 200; h++)
            len += snprintf(heaps + len, sizeof heaps - size_t(len), "%s%s %zu MiB", h ? ", " : "",
                            (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "device" : "host",
                            size_t(budgets[h].statistics.allocationBytes >> 20));
        logmsg("Vulkan memory: %s; staging ring %zu MiB, dynamic ring %zu MiB", heaps, ring[0] >> 20, ring[1] >> 20);
    }
    const uint32_t pipelines = g_stats.pipelines;
    g_stats = Stats();
    g_stats.pipelines = pipelines;
}

void deferDelete(std::function<void()> fn) { s_garbage.push_back(Garbage{currentSerial(), std::move(fn)}); }

void collectGarbage() {
    pollCompleted();
    while (!s_garbage.empty() && s_garbage.front().serial <= D.completed) {
        s_garbage.front().fn();
        s_garbage.pop_front();
    }
}

VkCommandBuffer mainCmd() { return s_cur->main; }

VkCommandBuffer uploadCmd() {
    if (!s_cur->uploadBegun) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s_cur->upload, &bi);
        s_cur->uploadBegun = true;
    }
    return s_cur->upload;
}

// ------------------------------------------------------------------ rings
RingAlloc Ring::alloc(VkDeviceSize bytes, VkDeviceSize align) {
    if (align < 1) align = 1;
    const uint64_t serial = currentSerial();
    if (cur >= 0) {
        Block& b = blocks[size_t(cur)];
        VkDeviceSize off = (b.used + align - 1) / align * align;
        if (off + bytes <= b.size) {
            b.used = off + bytes;
            b.serial = serial;
            return RingAlloc{b.buf, off, b.map + off};
        }
    }
    int pick = -1;
    for (size_t i = 0; i < blocks.size(); i++)
        if (int(i) != cur && blocks[i].size >= bytes && serialDone(blocks[i].serial)) {
            pick = int(i);
            break;
        }
    if (pick < 0) {
        Block b;
        b.size = std::max(blockSize, (bytes + 0xFFFF) & ~VkDeviceSize(0xFFFF));
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = b.size;
        bci.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        // its own memory, given back when the block is let go after a burst
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT |
                    VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(D.vma, &bci, &aci, &b.buf, &b.alloc, &info) != VK_SUCCESS) {
            logmsg("Vulkan: out of host memory for a %zu KiB ring block", size_t(b.size >> 10));
            abort();
        }
        b.map = static_cast<uint8_t*>(info.pMappedData);
        blocks.push_back(b);
        pick = int(blocks.size() - 1);
    }
    // blocks past the first few that are free and not this one: let them go
    // (a texture pack's burst of uploads should not keep its staging forever)
    if (blocks.size() > 2) {
        for (size_t i = 0; i < blocks.size();) {
            if (int(i) != pick && int(i) != cur && serialDone(blocks[i].serial) && blocks.size() > 2) {
                vmaDestroyBuffer(D.vma, blocks[i].buf, blocks[i].alloc);
                blocks.erase(blocks.begin() + long(i));
                if (pick > int(i)) pick--;
                if (cur > int(i)) cur--;
            } else {
                i++;
            }
        }
    }
    cur = pick;
    Block& b = blocks[size_t(cur)];
    b.used = bytes;
    b.serial = serial;
    return RingAlloc{b.buf, 0, b.map};
}

void Ring::destroy() {
    for (Block& b : blocks) vmaDestroyBuffer(D.vma, b.buf, b.alloc);
    blocks.clear();
    cur = -1;
}

// ------------------------------------------------------------------ images
bool isDepthFormat(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT ||
           f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

static bool isCompressedFormat(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }

Image* createImage(VkFormat fmt, uint32_t w, uint32_t h, uint32_t levels, VkSampleCountFlagBits samples, bool attachment) {
    Image* img = new Image;
    img->format = fmt;
    img->w = std::max(1u, w);
    img->h = std::max(1u, h);
    img->levels = std::max(1u, levels);
    img->samples = samples;
    const bool depth = isDepthFormat(fmt);
    img->aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {img->w, img->h, 1};
    ci.mipLevels = img->levels;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // multisampled images are only drawn to and resolved (no sampling, which
    // not every sample count supports)
    ci.usage = (samples != VK_SAMPLE_COUNT_1_BIT ? 0 : VK_IMAGE_USAGE_SAMPLED_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (attachment && !isCompressedFormat(fmt) && fmt != VK_FORMAT_R32G32B32A32_SFLOAT)
        ci.usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (attachment && (ci.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
        uint64_t(w) * h >= 512u * 512u)
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VkResult r = vmaCreateImage(D.vma, &ci, &aci, &img->image, &img->alloc, nullptr);
    if (r != VK_SUCCESS) {
        logmsg("Vulkan: could not make a %ux%u image (format %d, %u levels): %d", w, h, int(fmt), levels, int(r));
        delete img;
        return nullptr;
    }
    return img;
}

void destroyImageLater(Image* img) {
    if (!img) return;
    VkImage image = img->image;
    VmaAllocation alloc = img->alloc;
    std::vector<VkImageView> views;
    for (auto& kv : img->views) views.push_back(kv.second);
    const bool owned = img->owned;
    delete img;
    deferDelete([image, alloc, views, owned] {
        for (VkImageView v : views) vkDestroyImageView(D.dev, v, nullptr);
        if (owned) vmaDestroyImage(D.vma, image, alloc);
    });
}

VkImageView imageView(Image* img, uint32_t base, uint32_t count) {
    if (base >= img->levels) base = img->levels - 1;
    if (count == 0 || base + count > img->levels) count = img->levels - base;
    const uint32_t key = base | count << 8;
    auto it = img->views.find(key);
    if (it != img->views.end()) return it->second;
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = img->image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = img->format;
    ci.subresourceRange = {img->aspect, base, count, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(D.dev, &ci, nullptr, &v);
    img->views[key] = v;
    return v;
}

void use(Image* img, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access, bool write, VkCommandBuffer cb) {
    const bool main = cb == VK_NULL_HANDLE || cb == s_cur->main;
    if (img->layout != layout || img->lastWrite || write) {
        if (main) {
            if (renderingActive()) endRendering();
            cb = s_cur->main;
        }
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = img->lastWrite ? img->access : 0;
        b.dstAccessMask = access;
        b.oldLayout = img->layout;
        b.newLayout = layout;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img->image;
        b.subresourceRange = {img->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
        vkCmdPipelineBarrier(cb, img->stage, stage, 0, 0, nullptr, 0, nullptr, 1, &b);
        img->stage = stage;
        img->access = access;
        img->lastWrite = write;
    } else {
        if (main) cb = s_cur->main;
        img->stage |= stage;
        img->access |= access;
    }
    img->layout = layout;
    img->lastUse = currentSerial();
    if (main) img->lastMain = currentSerial();
}

void resizeBackbuffer(uint32_t w, uint32_t h) {
    if (g_backbuffer && g_backbuffer->w == w && g_backbuffer->h == h) return;
    if (renderingActive()) endRendering();
    destroyImageLater(g_backbuffer);
    g_backbuffer = createImage(VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, VK_SAMPLE_COUNT_1_BIT, true);
}

// ------------------------------------------------------------------ init
static bool pickDevice(VkSurfaceKHR surface, bool wantSwapchain) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(D.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(D.instance, &n, devs.data());
    const char* want = getenv("SMS_VK_DEVICE");
    int best = -1, bestScore = -1;
    uint32_t bestFamily = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_1) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qf.data());
        int family = -1;
        for (uint32_t q = 0; q < qn && family < 0; q++) {
            if (!(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_TRUE;
            if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, surface, &present);
            if (present) family = int(q);
        }
        if (family < 0) continue;
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> exts(en);
        vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &en, exts.data());
        if (!hasExtension(exts, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) continue;
        if (wantSwapchain && !hasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        if (p.apiVersion < VK_API_VERSION_1_3 && !hasExtension(exts, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) continue;
        if (p.apiVersion < VK_API_VERSION_1_2 && !hasExtension(exts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 500
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 100 : 10;
        if (want && *want) {
            char idx[16];
            snprintf(idx, sizeof idx, "%u", i);
            if (!strcmp(want, idx) || strstr(p.deviceName, want)) score += 100000;
        }
        if (score > bestScore) {
            bestScore = score;
            best = int(i);
            bestFamily = uint32_t(family);
        }
    }
    if (best < 0) return false;
    D.phys = devs[size_t(best)];
    D.queueFamily = bestFamily;
    vkGetPhysicalDeviceProperties(D.phys, &D.props);
    snprintf(D.name, sizeof D.name, "%s", D.props.deviceName);
    return true;
}

static void chooseDepthFormat() {
    struct Cand {
        VkFormat f;
        bool d24;
    };
    // depth-only formats: GL's EFB has no stencil, and a view of a combined
    // depth/stencil image would need both aspects attached
    const Cand cands[] = {{VK_FORMAT_X8_D24_UNORM_PACK32, true}, {VK_FORMAT_D32_SFLOAT, false}};
    for (const Cand& c : cands) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(D.phys, c.f, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((fp.optimalTilingFeatures & need) == need) {
            D.depthFormat = c.f;
            D.depthIsD24 = c.d24;
            D.depthBlit = (fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) ==
                          (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);
            return;
        }
    }
    D.depthFormat = VK_FORMAT_D32_SFLOAT;
    D.depthIsD24 = false;
}

bool init(const InitInfo& info) {
    if (s_active) return true;
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
#ifndef _WIN32
    if (const char* lib = libraryPath()) {
        // a bundled library (MoltenVK): its entry points stand in for the loader's
        void* h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
        auto gipa = h ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vkGetInstanceProcAddr")) : nullptr;
        if (gipa) {
            volkInitializeCustom(gipa);
            vr = VK_SUCCESS;
            logmsg("Vulkan: using %s", lib);
        } else {
            logmsg("Vulkan: could not load %s (%s)", lib, dlerror());
        }
    }
#endif
    if (vr != VK_SUCCESS) vr = volkInitialize();
    if (vr != VK_SUCCESS) {
        logmsg("Vulkan: no Vulkan loader found");
        return false;
    }
    const uint32_t loader = volkGetInstanceVersion();
    if (loader < VK_API_VERSION_1_1) {
        logmsg("Vulkan: the loader is Vulkan %u.%u; 1.1 or newer is needed", VK_VERSION_MAJOR(loader), VK_VERSION_MINOR(loader));
        return false;
    }
    // instance
    std::vector<const char*> exts, layers;
#ifdef SMS_GX_HAVE_SDL2
    if (info.sdlWindow) {
        unsigned n = 0;
        SDL_Vulkan_GetInstanceExtensions(static_cast<SDL_Window*>(info.sdlWindow), &n, nullptr);
        static std::vector<const char*> sdlExts;
        sdlExts.resize(n);
        if (!SDL_Vulkan_GetInstanceExtensions(static_cast<SDL_Window*>(info.sdlWindow), &n, sdlExts.data())) {
            logmsg("Vulkan: SDL has no Vulkan surface support here (%s)", SDL_GetError());
            return false;
        }
        exts.insert(exts.end(), sdlExts.begin(), sdlExts.begin() + n);
    }
#else
    if (info.sdlWindow) return false;
#endif
    uint32_t ien = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ien, nullptr);
    std::vector<VkExtensionProperties> iexts(ien);
    vkEnumerateInstanceExtensionProperties(nullptr, &ien, iexts.data());
    VkInstanceCreateFlags iflags = 0;
    if (hasExtension(iexts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        iflags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    const bool validation = envTrue("SMS_VK_VALIDATION");
    if (validation) {
        uint32_t ln = 0;
        vkEnumerateInstanceLayerProperties(&ln, nullptr);
        std::vector<VkLayerProperties> ls(ln);
        vkEnumerateInstanceLayerProperties(&ln, ls.data());
        bool found = false;
        for (const VkLayerProperties& l : ls) found = found || !strcmp(l.layerName, "VK_LAYER_KHRONOS_validation");
        if (found) layers.push_back("VK_LAYER_KHRONOS_validation");
        else logmsg("Vulkan: SMS_VK_VALIDATION is set but VK_LAYER_KHRONOS_validation is not installed");
        if (hasExtension(iexts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Super Mario Sunshine";
    app.pEngineName = "sms_gx glvk";
    app.apiVersion = std::min(loader, uint32_t(VK_API_VERSION_1_3));
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = iflags;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    VkResult r = vkCreateInstance(&ici, nullptr, &D.instance);
    if (r != VK_SUCCESS) {
        logmsg("Vulkan: vkCreateInstance failed (%d)", int(r));
        return false;
    }
    volkLoadInstance(D.instance);
    if (validation && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = debugCallback;
        vkCreateDebugUtilsMessengerEXT(D.instance, &mci, nullptr, &D.messenger);
    }
    // surface
#ifdef SMS_GX_HAVE_SDL2
    if (info.sdlWindow) {
        if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(info.sdlWindow), D.instance, &s_sc.surface)) {
            logmsg("Vulkan: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
            shutdown();
            return false;
        }
        s_sc.window = info.sdlWindow;
    }
#endif
    if (!pickDevice(s_sc.surface, info.sdlWindow != nullptr)) {
        logmsg("Vulkan: no GPU with Vulkan 1.1, dynamic rendering, push descriptors and timeline semaphores%s",
               info.sdlWindow ? " that can show this window" : "");
        shutdown();
        return false;
    }
    D.apiVersion = std::min(D.props.apiVersion, app.apiVersion);

    // device extensions and features
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(D.phys, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> dexts(en);
    vkEnumerateDeviceExtensionProperties(D.phys, nullptr, &en, dexts.data());
    std::vector<const char*> want;
    auto add = [&](const char* e) {
        if (!hasExtension(dexts, e)) return false;
        want.push_back(e);
        return true;
    };
    add(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    if (info.sdlWindow) add(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const bool core12 = D.apiVersion >= VK_API_VERSION_1_2, core13 = D.apiVersion >= VK_API_VERSION_1_3;
    if (!core13) {
        add(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        add(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
        add(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    }
    if (!core12) {
        add(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        D.hostQueryReset = add(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
    }
    const bool edsExt = !core13 && add(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    const bool clipCtl = add(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    D.portability = add("VK_KHR_portability_subset");

    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures fts{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceHostQueryResetFeatures fhq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeatures fdr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fcc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    PortabilityFeatures fps{};
    fps.sType = kPortabilityFeaturesType;
    void** tail = &f2.pNext;
    auto chain = [&](void* s, void** next) {
        *tail = s;
        tail = next;
    };
    if (core12) chain(&f12, &f12.pNext);
    else {
        chain(&fts, &fts.pNext);
        if (D.hostQueryReset) chain(&fhq, &fhq.pNext);
    }
    if (core13) chain(&f13, &f13.pNext);
    else chain(&fdr, &fdr.pNext);
    if (edsExt) chain(&feds, &feds.pNext);
    if (clipCtl) chain(&fcc, &fcc.pNext);
    if (D.portability) chain(&fps, &fps.pNext);
    vkGetPhysicalDeviceFeatures2(D.phys, &f2);
    if (D.portability) {
        // enable only what glvk uses; samplers without LOD bias leave it to the shaders
        const VkBool32 lodBias = fps.samplerMipLodBias, alpha = fps.constantAlphaColorBlendFactors;
        void* next = fps.pNext;
        fps = PortabilityFeatures{};
        fps.sType = kPortabilityFeaturesType;
        fps.pNext = next;
        fps.samplerMipLodBias = lodBias;
        fps.constantAlphaColorBlendFactors = alpha;
        fps.vertexAttributeAccessBeyondStride = VK_FALSE;
        D.noLodBias = !lodBias;
        if (!alpha) logmsg("Vulkan: no constant alpha blend factors: destination alpha writes may differ");
    }
    const VkPhysicalDeviceFeatures have = f2.features;
    VkPhysicalDeviceFeatures& en0 = f2.features;
    en0 = VkPhysicalDeviceFeatures{};
    en0.geometryShader = have.geometryShader;
    en0.logicOp = have.logicOp;
    en0.depthClamp = have.depthClamp;
    en0.largePoints = have.largePoints;
    en0.samplerAnisotropy = have.samplerAnisotropy;
    en0.occlusionQueryPrecise = have.occlusionQueryPrecise;
    en0.textureCompressionBC = have.textureCompressionBC;
    en0.shaderClipDistance = have.shaderClipDistance;
    // SMS_VK_PORTABILITY_TEST=1: behave as on MoltenVK (no geometry shaders,
    // logic ops or sampler LOD bias, 32-bit float depth) to try those paths
    // on a desktop GPU
    const bool emulate = envTrue("SMS_VK_PORTABILITY_TEST");
    if (emulate) {
        en0.geometryShader = VK_FALSE;
        en0.logicOp = VK_FALSE;
        D.noLodBias = true;
        logmsg("Vulkan: SMS_VK_PORTABILITY_TEST: no geometry shaders, logic ops or sampler LOD bias, float depth");
    }
    D.features = en0;
    bool ok = true;
    if (core12) {
        ok = ok && f12.timelineSemaphore;
        D.hostQueryReset = f12.hostQueryReset;
        VkPhysicalDeviceVulkan12Features keep{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        keep.pNext = f12.pNext;
        keep.timelineSemaphore = VK_TRUE;
        keep.hostQueryReset = f12.hostQueryReset;
        f12 = keep;
    } else {
        ok = ok && fts.timelineSemaphore;
        D.hostQueryReset = D.hostQueryReset && fhq.hostQueryReset;
    }
    if (core13) {
        ok = ok && f13.dynamicRendering;
        VkPhysicalDeviceVulkan13Features keep{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        keep.pNext = f13.pNext;
        keep.dynamicRendering = VK_TRUE;
        f13 = keep;
        D.eds = true;  // core in 1.3
    } else {
        ok = ok && fdr.dynamicRendering;
        D.eds = edsExt && feds.extendedDynamicState;
    }
    D.depthClipControl = clipCtl && fcc.depthClipControl;
    if (!ok) {
        logmsg("Vulkan: %s lacks timeline semaphores or dynamic rendering", D.name);
        shutdown();
        return false;
    }
    if (!D.hostQueryReset) {
        logmsg("Vulkan: %s lacks host query reset", D.name);
        shutdown();
        return false;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = D.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(want.size());
    dci.ppEnabledExtensionNames = want.data();
    r = vkCreateDevice(D.phys, &dci, nullptr, &D.dev);
    if (r != VK_SUCCESS) {
        logmsg("Vulkan: vkCreateDevice failed (%d)", int(r));
        shutdown();
        return false;
    }
    volkLoadDevice(D.dev);
    vkGetDeviceQueue(D.dev, D.queueFamily, 0, &D.queue);
    D.cmdBeginRendering = core13 ? vkCmdBeginRendering : vkCmdBeginRenderingKHR;
    D.cmdEndRendering = core13 ? vkCmdEndRendering : vkCmdEndRenderingKHR;
    D.cmdPushDescriptorSet = vkCmdPushDescriptorSetKHR;
    D.waitSemaphores = core12 ? vkWaitSemaphores : vkWaitSemaphoresKHR;
    D.getSemaphoreCounterValue = core12 ? vkGetSemaphoreCounterValue : vkGetSemaphoreCounterValueKHR;
    D.resetQueryPool = core12 ? vkResetQueryPool : vkResetQueryPoolEXT;
    if (D.eds) {
        D.cmdSetCullMode = core13 ? vkCmdSetCullMode : vkCmdSetCullModeEXT;
        D.cmdSetFrontFace = core13 ? vkCmdSetFrontFace : vkCmdSetFrontFaceEXT;
        D.cmdSetDepthTestEnable = core13 ? vkCmdSetDepthTestEnable : vkCmdSetDepthTestEnableEXT;
        D.cmdSetDepthWriteEnable = core13 ? vkCmdSetDepthWriteEnable : vkCmdSetDepthWriteEnableEXT;
        D.cmdSetDepthCompareOp = core13 ? vkCmdSetDepthCompareOp : vkCmdSetDepthCompareOpEXT;
        D.cmdSetPrimitiveTopology = core13 ? vkCmdSetPrimitiveTopology : vkCmdSetPrimitiveTopologyEXT;
        D.eds = D.cmdSetCullMode && D.cmdSetFrontFace && D.cmdSetDepthTestEnable && D.cmdSetDepthWriteEnable &&
                D.cmdSetDepthCompareOp && D.cmdSetPrimitiveTopology;
    }
    if (!D.cmdBeginRendering || !D.cmdPushDescriptorSet || !D.waitSemaphores) {
        logmsg("Vulkan: %s is missing entry points it reported", D.name);
        shutdown();
        return false;
    }
    D.maxAnisotropy = D.features.samplerAnisotropy ? D.props.limits.maxSamplerAnisotropy : 1.0f;

    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo vci{};
    vci.vulkanApiVersion = D.apiVersion;
    vci.physicalDevice = D.phys;
    vci.device = D.dev;
    vci.instance = D.instance;
    vci.pVulkanFunctions = &vf;
    // 32 MiB blocks (VMA's default is 256): the host-visible ones are system
    // memory the process keeps while any allocation in them lives
    vci.preferredLargeHeapBlockSize = VkDeviceSize(32) << 20;
    if (vmaCreateAllocator(&vci, &D.vma) != VK_SUCCESS) {
        logmsg("Vulkan: the memory allocator failed to start");
        shutdown();
        return false;
    }

    VkSemaphoreTypeCreateInfo stci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &stci;
    vkCreateSemaphore(D.dev, &sci, nullptr, &D.timeline);

    chooseDepthFormat();
    if (emulate) {
        D.depthFormat = VK_FORMAT_D32_SFLOAT;
        D.depthIsD24 = false;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(D.phys, D.depthFormat, &fp);
        D.depthBlit = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
    }
    loadPipelineCache();

    // the descriptor set and pipeline layout every program shares
    VkDescriptorSetLayoutBinding binds[B_COUNT];
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                      (D.features.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
    for (uint32_t b = 0; b < B_COUNT; b++) {
        binds[b] = VkDescriptorSetLayoutBinding{};
        binds[b].binding = b;
        binds[b].stageFlags = stages;
        if (b < B_SAMPLER_ARRAY) {
            binds[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binds[b].descriptorCount = 1;
        } else {
            binds[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binds[b].descriptorCount = b == B_SAMPLER_ARRAY ? kSamplerArrayMax : 1;
        }
    }
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dsl.bindingCount = B_COUNT;
    dsl.pBindings = binds;
    vkCreateDescriptorSetLayout(D.dev, &dsl, nullptr, &D.setLayout);
    // bytes 0-15: the point size (vertex and geometry stages); 16-47: the
    // LOD bias of each of u_tex's eight samplers, without sampler LOD bias
    D.pushStages = stages & ~VkShaderStageFlags(VK_SHADER_STAGE_FRAGMENT_BIT);
    VkPushConstantRange pcr[2] = {{D.pushStages, 0, 16}, {VK_SHADER_STAGE_FRAGMENT_BIT, 16, 32}};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &D.setLayout;
    plci.pushConstantRangeCount = 2;
    plci.pPushConstantRanges = pcr;
    vkCreatePipelineLayout(D.dev, &plci, nullptr, &D.pipeLayout);


    g_staging.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    g_staging.blockSize = VkDeviceSize(16) << 20;
    g_dynamic.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    g_dynamic.blockSize = VkDeviceSize(4) << 20;

    s_sc.vsync = info.vsync;
    s_flushDraws = 200;
    if (const char* e = getenv("SMS_VK_FLUSH_DRAWS")) s_flushDraws = uint32_t(std::max(0, atoi(e)));
    if (const char* e = getenv("SMS_VK_FRAMES")) s_sc.frames = uint32_t(std::max(1, std::min(int(kFramesMax), atoi(e))));
    if (s_sc.surface) {
        VkSemaphoreCreateInfo bsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (VkSemaphore& s : s_sc.acquire) vkCreateSemaphore(D.dev, &bsci, nullptr, &s);
#ifdef SMS_GX_HAVE_SDL2
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window*>(info.sdlWindow), &w, &h);
        s_wantW = uint32_t(std::max(1, w));
        s_wantH = uint32_t(std::max(1, h));
#endif
        if (!createSwapchain()) {
            logmsg("Vulkan: no swapchain for this window");
            shutdown();
            return false;
        }
    }
    s_active = true;
    beginSegment();
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    if (core12) p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(D.phys, &p2);
    logmsg("Vulkan %u.%u.%u on %s (%s %s)%s%s%s, depth %s%s", VK_VERSION_MAJOR(D.apiVersion), VK_VERSION_MINOR(D.apiVersion),
           VK_VERSION_PATCH(D.props.apiVersion), D.name, core12 ? drv.driverName : "driver", core12 ? drv.driverInfo : "",
           D.eds ? ", extended dynamic state" : "", D.depthClipControl ? ", depth clip control" : "",
           D.portability ? ", portability subset" : "", D.depthIsD24 ? "24-bit" : "32-bit float",
           validation ? ", validation on" : "");
    if (!D.features.geometryShader) logmsg("Vulkan: no geometry shaders: wide lines are drawn 1 pixel wide");
    if (!D.features.logicOp) logmsg("Vulkan: no logic ops: drawn as plain copies");
    return true;
}

void shutdown() {
    if (D.dev) {
        if (s_active) {
            if (s_cur) submit();
            vkDeviceWaitIdle(D.dev);
            D.completed = D.submitted;
            collectGarbage();
            savePipelineCache(true);
        }
        if (s_active) {
            destroyAllObjects();
            destroyDrawObjects();
        }
        shaderShutdown();
        destroyImageLater(g_backbuffer);
        g_backbuffer = nullptr;
        vkDeviceWaitIdle(D.dev);
        D.completed = ~0ull;
        collectGarbage();
        if (s_active) destroySamplers();
        g_staging.destroy();
        g_dynamic.destroy();
        for (Segment* s : s_idle) vkDestroyCommandPool(D.dev, s->pool, nullptr), delete s;
        for (Segment* s : s_busy) vkDestroyCommandPool(D.dev, s->pool, nullptr), delete s;
        if (s_cur) vkDestroyCommandPool(D.dev, s_cur->pool, nullptr), delete s_cur;
        s_idle.clear();
        s_busy.clear();
        s_cur = nullptr;
        if (s_sc.handle) vkDestroySwapchainKHR(D.dev, s_sc.handle, nullptr);
        for (VkSemaphore s : s_sc.renderDone) vkDestroySemaphore(D.dev, s, nullptr);
        for (VkSemaphore& s : s_sc.acquire)
            if (s) vkDestroySemaphore(D.dev, s, nullptr), s = VK_NULL_HANDLE;
        s_sc.renderDone.clear();
        s_sc.handle = VK_NULL_HANDLE;
        if (D.pipeLayout) vkDestroyPipelineLayout(D.dev, D.pipeLayout, nullptr);
        if (D.setLayout) vkDestroyDescriptorSetLayout(D.dev, D.setLayout, nullptr);
        if (D.pipelineCache) vkDestroyPipelineCache(D.dev, D.pipelineCache, nullptr);
        if (D.timeline) vkDestroySemaphore(D.dev, D.timeline, nullptr);
        if (D.vma) vmaDestroyAllocator(D.vma);
        vkDestroyDevice(D.dev, nullptr);
    }
    if (s_sc.surface) vkDestroySurfaceKHR(D.instance, s_sc.surface, nullptr);
    s_sc.surface = VK_NULL_HANDLE;
    if (D.messenger) vkDestroyDebugUtilsMessengerEXT(D.instance, D.messenger, nullptr);
    if (D.instance) vkDestroyInstance(D.instance, nullptr);
    D = Device();
    s_active = false;
}

bool active() { return s_active; }

const char* libraryPath() {
    if (!s_libPathSet) {
        s_libPathSet = true;
        if (const char* e = getenv("SMS_VK_LIBRARY")) {
            if (*e) s_libPath = e;
        } else {
#if defined(__APPLE__) && defined(SMS_GX_HAVE_SDL2)
            if (char* base = SDL_GetBasePath()) {
                const std::string b = base;
                SDL_free(base);
                for (const char* rel : {"libMoltenVK.dylib", "../Frameworks/libMoltenVK.dylib", "libvulkan.1.dylib",
                                        "../Frameworks/libvulkan.1.dylib"})
                    if (fileExists(b + rel)) {
                        s_libPath = b + rel;
                        break;
                    }
            }
#endif
        }
    }
    return s_libPath.empty() ? nullptr : s_libPath.c_str();
}
const char* deviceName() { return D.name; }

void setDefaultFramebufferSize(int w, int h) {
    if (w < 1 || h < 1) return;
    s_wantW = uint32_t(w);
    s_wantH = uint32_t(h);
    resizeBackbuffer(s_wantW, s_wantH);
    if (s_sc.handle && (s_sc.extent.width != s_wantW || s_sc.extent.height != s_wantH)) s_sc.dirty = true;
}

void setSwapInterval(int vsync) {
    if (s_sc.vsync == vsync) return;
    s_sc.vsync = vsync;
    if (s_sc.handle) s_sc.dirty = true;
}

bool presentFrame() {
    s_presents++;
    statsFrame();
    realizePending();
    if ((s_presents & 1023) == 0) savePipelineCache(false);
    if (!s_sc.surface) {  // headless: submit, keeping as many frames in flight as with a window
        submit();
        s_headlessFrames[s_headlessFrame] = D.submitted;
        s_headlessFrame = (s_headlessFrame + 1) % s_sc.frames;
        waitSerial(s_headlessFrames[s_headlessFrame]);
        return true;
    }
    if (s_sc.dirty || !s_sc.handle) recreateSwapchain();
    if (!s_sc.handle) {  // minimised
        submit();
        return false;
    }
    const uint32_t f = s_sc.frame;
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(D.dev, s_sc.handle, UINT64_MAX, s_sc.acquire[f], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        if (!s_sc.handle) {
            submit();
            return false;
        }
        r = vkAcquireNextImageKHR(D.dev, s_sc.handle, UINT64_MAX, s_sc.acquire[f], VK_NULL_HANDLE, &idx);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        logmsg("Vulkan: vkAcquireNextImageKHR failed (%d)", int(r));
        submit();
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) s_sc.dirty = true;
    if (renderingActive()) endRendering();
    VkCommandBuffer cb = mainCmd();
    VkImage sci = s_sc.images[idx];
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = sci;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    if (g_backbuffer && s_sc.blitDst) {
        use(g_backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
        // framebuffer 0's row 0 is the bottom of the window (GL), the swapchain's the top
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, int32_t(g_backbuffer->h), 0};
        blit.srcOffsets[1] = {int32_t(g_backbuffer->w), 0, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {int32_t(s_sc.extent.width), int32_t(s_sc.extent.height), 1};
        const bool same = g_backbuffer->w == s_sc.extent.width && g_backbuffer->h == s_sc.extent.height;
        vkCmdBlitImage(cb, g_backbuffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sci, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    } else {
        VkClearColorValue black{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, sci, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &b);
    VkSemaphore done = s_sc.renderDone[idx];
    submitSegment(s_sc.acquire[f], VK_PIPELINE_STAGE_TRANSFER_BIT, done);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &s_sc.handle;
    pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(D.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) s_sc.dirty = true;
    else if (r != VK_SUCCESS) logmsg("Vulkan: vkQueuePresentKHR failed (%d)", int(r));
    s_sc.frameSerial[f] = D.submitted;
    s_sc.frame = (f + 1) % s_sc.frames;
    // as many frames in flight as SMS_VK_FRAMES (2): the acquire semaphore
    // about to be reused was waited for by that frame
    waitSerial(s_sc.frameSerial[s_sc.frame]);
    return true;
}

void present() {
    if (s_active) presentFrame();
}

}  // namespace glvk
