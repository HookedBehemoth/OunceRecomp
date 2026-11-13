// The window (M2a): each present blits the NVN window texture, with the host's crop, into the swapchain's next image
// (letterboxed by gpu/vk/vk_present_rect.h); with no window (HOS_HEADLESS) into an image of its own. The host's call is
// noted and its GPU work runs in its queue's order (direct_sync.cpp): after every submission handed to that queue
// before it. The swapchain as gpu/vk/vk_main.cpp makes it, without HDR, the
// overlays' views or Reflex. Every 600 presents: the frame time and the counts of the other parts, one log line each.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "direct_compute.h"
#include "direct_assets.h"
#include "direct_cmd.h"
#include "direct_draw.h"
#include "direct_task.h"
#include "direct_meshcap.h"
#include "direct_dump.h"
#include "direct_image.h"
#include "direct_internal.h"
#include "direct_pass.h"
#include "direct_query.h"
#include "direct_check.h"
#include "direct_marks.h"
#include "direct_pstats.h"
#include "vk_present_rect.h"

namespace hsd {
// (M6a, direct_sync.cpp: HOS_DBG=gputime / frames, measurement)
bool gputime_on();
void gputime_present_begin(VkCommandBuffer cb, uint32_t& slot);
void gputime_present_end(VkCommandBuffer cb, uint32_t slot);
void gputime_read();
bool gputime_row(double out[3]);
void gputime_stats(char* out, size_t cap);
extern std::atomic<int64_t> st_fence_wait_ns;
namespace {

using clk = std::chrono::steady_clock;
const VkClearColorValue kBlack{{0.0f, 0.0f, 0.0f, 1.0f}};
std::atomic<uint64_t> asked{0};   // (the host's presents)
std::atomic<int64_t> st_no_image{0}, st_shown{0};

const MkwVkWindow* window = nullptr;
VkSwapchainKHR swapchain = VK_NULL_HANDLE;
VkExtent2D extent{1920, 1080};
VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
std::vector<VkImage> images;
std::vector<VkSemaphore> rendered;   // (per swapchain image: the present waits for its clear)
std::atomic<uint64_t> resize_to{0};
VkImage offscreen = VK_NULL_HANDLE;
bool unseen = false;
bool swap_src = false;   // (M4d: the swapchain's images can be read: shots)   // (no image back in 100 ms: a Wayland window that cannot be seen)

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    // (M4d: a shot of what this frame presented, read back into its own buffer; written once its fence has passed)
    VkBuffer shot_buf = VK_NULL_HANDLE;
    Mem* shot_mem = nullptr;
    void* shot_map = nullptr;
    int64_t shot_present = 0;   // (0: none waiting)
};

// (M4d) Shots of the presented frame (hsd_shots: the host's RX_VK_SHOTS / RX_VK_SHOT_EVERY, as the frozen back end reads
// them): every Nth present the image presented (the swapchain's or the offscreen one, after the blit with the crop) is
// scaled into a 1280x720 RGBA8 image and copied into the frame slot's buffer; once that slot's fence has passed (three
// presents later, where the present waits for it anyway) the pixels go to <dir>/vk_<present>.png on a thread of its own.
// The image and the three buffers are made at the first shot; nothing is made or recorded when shots are off.
constexpr uint32_t kShotW = 1280, kShotH = 720;
std::string shot_dir;
int shot_every = 0;
VkImage shot_image = VK_NULL_HANDLE;
Mem* shot_image_mem = nullptr;
bool shot_failed = false;
int64_t run_count = 0;   // (presents run: the shots' numbers)
Frame frames[3];
uint32_t frame_index = 0;

// (the 600-present line)
int64_t presents = 0;
clk::time_point last_present{};
std::vector<float> intervals_ms;

bool create_swapchain(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev.physical, dev.surface, &caps) != VK_SUCCESS) return false;
    if (caps.currentExtent.width != UINT32_MAX) width = caps.currentExtent.width, height = caps.currentExtent.height;
    if (!width || !height || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical, dev.surface, &n, nullptr);
    if (!n) return false;
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev.physical, dev.surface, &n, formats.data());
    VkSurfaceFormatKHR fmt = formats[0];
    for (const auto& f : formats)   // (a UNORM sRGB-tagged one, as vk_main.cpp: Wayland's pass-through entries left out)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            fmt = f;
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical, dev.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev.physical, dev.surface, &n, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;   // (mailbox where there is one: the game paces itself, nn_vi.c)
    for (auto m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = dev.surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = {width, height};
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (shot_every && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // (M4d: shots)
    swap_src = (ci.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha))
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                ci.compositeAlpha = a;
                break;
            }
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(dev.device, &ci, nullptr, &sc);
    if (swapchain) {   // (retired even when the new one failed; its images' presents done first)
        {
            std::lock_guard qk(dev.queue_mu);
            vkDeviceWaitIdle(dev.device);
        }
        for (VkSemaphore s : rendered) vkDestroySemaphore(dev.device, s, nullptr);
        rendered.clear(), images.clear();
        vkDestroySwapchainKHR(dev.device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
    if (r != VK_SUCCESS) {
        logf("direct: vkCreateSwapchainKHR failed (VkResult %d, %ux%u)", (int)r, width, height);
        return false;
    }
    vkGetSwapchainImagesKHR(dev.device, sc, &n, nullptr);
    images.resize(n);
    vkGetSwapchainImagesKHR(dev.device, sc, &n, images.data());
    rendered.assign(n, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& s : rendered) vkCreateSemaphore(dev.device, &si, nullptr, &s);
    swapchain = sc, extent = {width, height}, format = fmt.format;
    logf("direct: swapchain %ux%u, %u images, %s", width, height, n, mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    return true;
}

bool make_offscreen() {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {extent.width, extent.height, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // (M4d: shots read it)
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev.device, &ii, nullptr, &offscreen) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev.device, offscreen, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < dev.memory.memoryTypeCount && ai.memoryTypeIndex == UINT32_MAX; i++)
        if ((req.memoryTypeBits >> i & 1) && (dev.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            ai.memoryTypeIndex = i;
    VkDeviceMemory mem;
    return ai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(dev.device, &ai, nullptr, &mem) == VK_SUCCESS &&
           vkBindImageMemory(dev.device, offscreen, mem, 0) == VK_SUCCESS;
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src,
             VkAccessFlags2 src_access, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src, b.srcAccessMask = src_access, b.dstStageMask = dst, b.dstAccessMask = dst_access;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

bool shots_ready() {
    if (shot_image) return true;
    if (shot_failed) return false;
    shot_failed = true;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {kShotW, kShotH, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!mem_image(ii, shot_image, shot_image_mem)) {
        shot_image = VK_NULL_HANDLE;
        logf("direct: shots: no image for them");
        return false;
    }
    for (Frame& f : frames)
        if (!mem_buffer((uint64_t)kShotW * kShotH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, f.shot_buf, f.shot_mem, &f.shot_map) || !f.shot_map) {
            logf("direct: shots: no readback buffer");
            return false;
        }
    shot_failed = false;
    logf("direct: shots of the presented frame every %d presents, %ux%u, into %s", shot_every, kShotW, kShotH, shot_dir.c_str());
    return true;
}

void shot_write(Frame& f) {   // (its fence has passed)
    if (!f.shot_present) return;
    mem_invalidate(f.shot_mem);
    auto px = std::make_shared<std::vector<uint8_t>>((const uint8_t*)f.shot_map, (const uint8_t*)f.shot_map + (size_t)kShotW * kShotH * 4);
    char path[1200];
    std::snprintf(path, sizeof path, "%s/vk_%06lld.png", shot_dir.c_str(), (long long)f.shot_present);
    f.shot_present = 0;
    std::thread([px, p = std::string(path)] {
        for (size_t i = 3; i < px->size(); i += 4) (*px)[i] = 255;   // (opaque, as shown)
        if (!dump_write_png(p.c_str(), px->data(), kShotW, kShotH)) logf("direct: shots: %s not written", p.c_str());
    }).detach();
}

// the presented image dst (TRANSFER_DST, its blit done) into the shot image and f's buffer; dst back in TRANSFER_DST
void shot_record(Frame& f, VkImage dst) {
    barrier(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(f.cmd, shot_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {(int)extent.width, (int)extent.height, 1};
    b.dstOffsets[1] = {(int)kShotW, (int)kShotH, 1};
    vkCmdBlitImage(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    barrier(f.cmd, shot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {kShotW, kShotH, 1};
    vkCmdCopyImageToBuffer(f.cmd, shot_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.shot_buf, 1, &c);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(f.cmd, &d);
    barrier(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

void log_line() {
    std::vector<float> v = intervals_ms;
    intervals_ms.clear();
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (float x : v) sum += x;
    const double mean = v.empty() ? 0 : sum / v.size();
    auto pct = [&](double p) { return v.empty() ? 0.0 : (double)v[std::min(v.size() - 1, (size_t)(p * v.size()))]; };
    logf("vk: %lld presents, %.1f fps (direct); frame time %.2f ms mean, %.2f median, %.2f p99, %.2f max", (long long)presents,
         mean > 0 ? 1000.0 / mean : 0.0, mean, pct(0.5), pct(0.99), v.empty() ? 0.0 : (double)v.back());
    static char b[8192];
    if (gputime_on()) {
        gputime_stats(b, sizeof b);
        logf("direct: %s", b);
    }
    const double frames = 600;
    lists_stats(b, sizeof b, frames);
    logf("direct: %s", b);
    sync_stats(b, sizeof b, frames);
    logf("direct: %s", b);
    regs_stats(b, sizeof b);
    logf("direct: %s", b);
    pass_stats(b, sizeof b, frames);
    logf("direct: %s", b);
    jobs_stats(b, sizeof b, frames);   // (M3a)
    logf("direct: %s", b);
    query_stats(b, sizeof b, frames);   // (M5)
    logf("direct: %s", b);
    image_stats(b, sizeof b);
    char* own = std::strstr(b, "; not from the store");   // (M6f: the images' memory on a line of its own)
    if (own) *own = 0;
    logf("direct: %s; presents shown %lld, with no image %lld", b, (long long)st_shown.exchange(0), (long long)st_no_image.exchange(0));
    if (own) logf("direct: images' memory: %s", own + 2);
    cmd_stats(b, sizeof b, frames);
    logf("direct: %s", b);
    assets_stats(b, sizeof b, frames);
    logf("direct: %s", b);
    layer0_stats(b, sizeof b);
    logf("direct: %s", b);
    mem_stats(b, sizeof b);   // (M6f)
    logf("direct: %s", b);
    sites_stats(b, sizeof b, frames);
    logf("direct: %s", b);
    draw_log(frames);   // (M2b)
    compute_log(frames);   // (M4a)
    validation_stats(b, sizeof b);   // (M6b)
    logf("validation: %s", b);
    strict_stats(b, sizeof b);
    logf("strict: %s", b);
}

}  // namespace

int present_start(void* hwnd) {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = dev.queue_family;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (Frame& f : frames) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        if (vkCreateCommandPool(dev.device, &pi, nullptr, &f.pool) != VK_SUCCESS) return 0;
        ai.commandPool = f.pool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev.device, &ai, &f.cmd) != VK_SUCCESS || vkCreateFence(dev.device, &fi, nullptr, &f.done) != VK_SUCCESS ||
            vkCreateSemaphore(dev.device, &si, nullptr, &f.acquired) != VK_SUCCESS)
            return 0;
    }
    if (!hwnd) {   // (HOS_HEADLESS)
        dev.offscreen = true;
        logf("direct: no window: presents clear an image of their own (%ux%u)", extent.width, extent.height);
        return make_offscreen() ? 1 : 0;
    }
    window = (const MkwVkWindow*)hwnd;
    if (window->version < 1 || window->version > MKWVK_WINDOW_VERSION) return 0;
    if (!window->create_surface(window->window, (void*)dev.instance, (void*)&dev.surface)) {
        logf("direct: SDL_Vulkan_CreateSurface failed");
        return 0;
    }
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.queue_family, dev.surface, &ok);
    int width = 0, height = 0;
    window->drawable_size(window->window, &width, &height);
    if (!ok || width <= 0 || height <= 0 || !create_swapchain((uint32_t)width, (uint32_t)height)) {
        logf("direct: no swapchain for the window (%dx%d, present support %u)", width, height, ok);
        return 0;
    }
    return 1;
}

void present_resize(int width, int height) {   // (the window's thread: noted; the next present makes the swapchain)
    if (dev.offscreen || width <= 0 || height <= 0) return;
    resize_to.store((uint64_t)(uint32_t)width << 32 | (uint32_t)height, std::memory_order_release);
}

uint64_t present_count() { return asked.load(std::memory_order_acquire); }

void present(const nvn_tex* t, const int* crop) {   // (the guest's Presentation Thread)
    const auto now = clk::now();
    if (presents) intervals_ms.push_back(std::chrono::duration<float, std::milli>(now - last_present).count());
    {   // (M6a, HOS_DBG=frames: each present's interval in direct_frames.csv; with gputime also the GPU's frame time, busy and idle as the
        // timestamps of an earlier frame came in, a row late; measurement)
        static FILE* frames_file = dbg("frames") ? std::fopen("direct_frames.csv", "w") : nullptr;
        if (frames_file && presents) {
            double g[3] = {0, 0, 0};
            const bool have = gputime_row(g);
            std::fprintf(frames_file, "%lld,%.4f,%.4f,%.4f,%.4f\n", (long long)presents, std::chrono::duration<double, std::milli>(now - last_present).count(),
                         have ? g[0] : -1.0, have ? g[1] : -1.0, have ? g[2] : -1.0);
            if (presents % 600 == 0) std::fflush(frames_file);
        }
    }
    last_present = now;
    presents++;
    if (presents % 600 == 0) log_line();
    const uint64_t n = asked.fetch_add(1, std::memory_order_acq_rel) + 1;
    strict_present(n);   // (M6b: arming)
    cmd_frame_presented(n);
    image_sweep(n);
    assets_present(n);
    dump_present(n);
    pstats_present(n);   // (M6h: HOS_DBG=pstats)
    sync_present(t, crop);
}

void present_run(const nvn_tex& t, const int crop[4]) {   // (direct_sync.cpp's mu held: one at a time)
    draw_meshlog_after();   // (M4a: HOS_DBG=meshlog's GPU-side records)
    task_log_after();       // (M4c: HOS_DBG=tasklog's task headers)
    meshcap_after();        // (M6e: HOS_DBG=meshcap's file)
    if (const uint64_t want = resize_to.exchange(0, std::memory_order_acquire); want || (!dev.offscreen && !swapchain))
        if (!create_swapchain(want ? (uint32_t)(want >> 32) : extent.width, want ? (uint32_t)want : extent.height)) return;
    Frame& f = frames[frame_index];
    frame_index = (frame_index + 1) % 3;
    {
        const auto w0 = clk::now();
        vkWaitForFences(dev.device, 1, &f.done, VK_TRUE, UINT64_MAX);
        st_fence_wait_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - w0).count(), std::memory_order_relaxed);
    }
    if (gputime_on()) gputime_read();   // (M6a)
    shot_write(f);   // (M4d: the shot this slot took three presents ago)
    const int64_t number = ++run_count;
    const bool shot = shot_every && number % shot_every == 0 && (dev.offscreen || swap_src) && shots_ready();
    uint32_t index = 0;
    VkImage dst = offscreen;
    if (!dev.offscreen) {
        const VkResult r = vkAcquireNextImageKHR(dev.device, swapchain, unseen ? 0 : 100000000ull, f.acquired, VK_NULL_HANDLE, &index);
        if (r == VK_TIMEOUT || r == VK_NOT_READY) {
            if (!unseen) logf("direct: the window is not shown (no image back in 100 ms): frames are not shown until it is");
            unseen = true;
            return;
        }
        unseen = false;
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            create_swapchain(extent.width, extent.height);
            return;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
        dst = images[index];
    }
    Image* src = t.gpu ? image_find(&t) : nullptr;
    if (src && (src->placeholder || !(src->aspect & VK_IMAGE_ASPECT_COLOR_BIT))) src = nullptr;
    (src ? st_shown : st_no_image).fetch_add(1, std::memory_order_relaxed);
    vkResetFences(dev.device, 1, &f.done);
    vkResetCommandPool(dev.device, f.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    uint32_t gt_slot = 0;
    if (gputime_on()) gputime_present_begin(f.cmd, gt_slot);   // (M6a)
    // (after the frame's work on the queue: the window texture's writes, before the blit reads it)
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT, mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(f.cmd, &d);
    barrier(f.cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const mkwvk::PresentRect rect = src ? mkwvk::present_rect(src->width, src->height, crop, extent.width, extent.height)
                                        : mkwvk::PresentRect{0, 0, 0, 0};
    const mkwvk::PresentSourceRect from = src ? mkwvk::present_source_rect(src->width, src->height, crop) : mkwvk::PresentSourceRect{};
    const bool bars = !src || !from.w || !from.h || rect.x || rect.y || rect.w != extent.width || rect.h != extent.height;
    if (bars) {
        vkCmdClearColorImage(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &kBlack, 1, &range);
        barrier(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    if (src && from.w && from.h && rect.w && rect.h) {
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[0] = {(int)from.x, (int)from.y, 0};
        b.srcOffsets[1] = {(int)(from.x + from.w), (int)(from.y + from.h), 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.dstOffsets[0] = {(int)rect.x, (int)rect.y, 0};
        b.dstOffsets[1] = {(int)(rect.x + rect.w), (int)(rect.y + rect.h), 1};
        vkCmdBlitImage(f.cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    if (shot) shot_record(f, dst), f.shot_present = number;
    if (!dev.offscreen)
        barrier(f.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    if (gputime_on()) gputime_present_end(f.cmd, gt_slot);
    vkEndCommandBuffer(f.cmd);
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = f.cmd;
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.acquired, wait.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    signal.semaphore = dev.offscreen ? VK_NULL_HANDLE : rendered[index], signal.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    if (!dev.offscreen) si.waitSemaphoreInfoCount = si.signalSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &wait, si.pSignalSemaphoreInfos = &signal;
    std::lock_guard qk(dev.queue_mu);
    VkResult r = vkQueueSubmit2(dev.queue, 1, &si, f.done);
    if (r == VK_SUCCESS && !dev.offscreen) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        r = vkQueuePresentKHR(dev.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) resize_to.store((uint64_t)extent.width << 32 | extent.height);
        else if (r != VK_SUCCESS) logf("direct: vkQueuePresentKHR: VkResult %d", (int)r);
    } else if (r != VK_SUCCESS) {
        logf("direct: the present's submit failed (VkResult %d)", (int)r);
        if (r == VK_ERROR_DEVICE_LOST) marks_report("the present's submit"), host->log_flush(), abort();
    }
}

}  // namespace hsd

extern "C" __attribute__((visibility("default"))) void hsd_shots(const char* dir, int every) {   // (before the renderer starts)
    if (!dir || !*dir || every <= 0) return;
    hsd::shot_dir = dir, hsd::shot_every = every;
}
