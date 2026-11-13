// meshlab (M6h): the fragment harness. A captured vertex draw's fragment stage (HOS_DBG=meshcap's MC_VDRAW: its program's
// stages and heap mappings, its uniform buffers' bytes) drawn outside the game against a replacement, into float targets, and
// compared bit for bit; and both timed on the GPU. Included by meshlab.cpp (its device, buffers, heaps and shader objects).
//
// What the fragment stage gets (the draw's own vertex stage is not run: its vertex buffers are not captured):
// - its uniform buffers: the captured bytes (driver banks as the back end made them, the game's buffers read at the frame's end,
//   c1), through the address table pushed at 120, and the draw's push data;
// - its inputs: a vertex stage of ours writes every generic location (0..31) of a grid of GRID x GRID quads over the target, each
//   corner's value a hash of its corner and location, in [0, 1) (or the --range given): smooth within a quad, varied across them;
// - its textures: every descriptor of every view-type region of the heap is one noise image of that type (2D, 2D array, 3D,
//   cube, ...: 64x64, 4 levels, RGBA8 values from a hash), every sampler linear / repeat (the compare ones LESS_OR_EQUAL), so that
//   whatever handle the uniform data names samples varied, deterministic texels;
// - its targets: every colour output it writes as R32G32B32A32_SFLOAT, cleared to a NaN pattern no shader writes, no blending, no
//   depth (a stage that writes depth: a D32 target, compare always).
// Compare: both stages draw the same grid into their own targets; the targets are read back and compared word for word (a
// discarded pixel keeps the clear pattern, so both must discard it). Bench: the grid drawn reps times a submission, the two
// stages alternating ABBA over rounds, GPU timestamps around each: ns per million pixels shaded.
#pragma once

namespace fsb {

struct NoiseHeap {
    std::vector<Img> images;
    bool made = false;
};
NoiseHeap noise;

uint32_t hash32(uint32_t x) {
    x ^= x >> 16, x *= 0x7feb352d, x ^= x >> 15, x *= 0x846ca68b, x ^= x >> 16;
    return x;
}

/// Every descriptor of the heap's sampled regions (and the storage regions) one noise image of the region's view type; every
/// sampler one linear / repeat sampler (compare: LESS_OR_EQUAL)
void noise_heap() {
    if (noise.made) return;
    noise.made = true;
    auto pWrite = (PFN_vkWriteResourceDescriptorsEXT)vkGetDeviceProcAddr(g.dev, "vkWriteResourceDescriptorsEXT");
    auto pWriteS = (PFN_vkWriteSamplerDescriptorsEXT)vkGetDeviceProcAddr(g.dev, "vkWriteSamplerDescriptorsEXT");
    if (!pWrite || !pWriteS) die("no descriptor writes");
    constexpr uint32_t kSlots = 1u << 16, kImageDesc = 64, kRegions = 7, kSamplers = 4096, kSamplerDesc = 16, kSide = 64, kLevels = 4;
    // (one image per view type: 2D array of 6 layers serves 2D, 2D array, cube and cube array; a 3D one; a 1D array of 6)
    struct Made { VkImage im; VkImageType type; uint32_t layers; uint32_t depth; };
    auto make = [&](VkImageType type, uint32_t layers, uint32_t depth, bool cube) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ci.imageType = type, ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {kSide, type == VK_IMAGE_TYPE_1D ? 1 : kSide, depth}, ci.mipLevels = kLevels, ci.arrayLayers = layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage im;
        VK(vkCreateImage(g.dev, &ci, nullptr, &im));
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g.dev, im, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size, ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory m;
        VK(vkAllocateMemory(g.dev, &ai, nullptr, &m));
        VK(vkBindImageMemory(g.dev, im, m, 0));
        // (texels: a hash of level, layer, texel)
        uint64_t bytes = 0;
        for (uint32_t l = 0; l < kLevels; l++) {
            const uint32_t s = std::max(1u, kSide >> l), sd = std::max(1u, depth >> l);
            bytes += 4ull * s * (type == VK_IMAGE_TYPE_1D ? 1 : s) * sd * layers;
        }
        Buf st = make_buf(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::vector<VkBufferImageCopy> regions;
        uint64_t at = 0;
        for (uint32_t l = 0; l < kLevels; l++) {
            const uint32_t s = std::max(1u, kSide >> l), sh = type == VK_IMAGE_TYPE_1D ? 1 : s, sd = std::max(1u, depth >> l);
            const uint64_t n = (uint64_t)s * sh * sd * layers;
            for (uint64_t i = 0; i < n; i++) ((uint32_t*)(st.map + at))[i] = hash32((uint32_t)(i * 2654435761u) ^ (l << 28) ^ (uint32_t)type << 24);
            VkBufferImageCopy r{};
            r.bufferOffset = at, r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, layers}, r.imageExtent = {s, sh, sd};
            regions.push_back(r);
            at += 4 * n;
        }
        VkCommandBuffer cb = new_cb();
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK(vkBeginCommandBuffer(cb, &bi));
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = im, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kLevels, 0, layers};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &di);
        vkCmdCopyBufferToImage(cb, st.b, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier2(cb, &di);
        VK(vkEndCommandBuffer(cb));
        VkFence f = new_fence();
        submit(cb, f);
        VK(vkWaitForFences(g.dev, 1, &f, VK_TRUE, ~0ull));
        return Made{im, type, layers, depth};
    };
    const Made i1 = make(VK_IMAGE_TYPE_1D, 6, 1, false), i2 = make(VK_IMAGE_TYPE_2D, 6, 1, true), i3 = make(VK_IMAGE_TYPE_3D, 1, kSide, false);
    // (the regions in VkImageViewType order: 1D, 2D, 3D, CUBE, 1D_ARRAY, 2D_ARRAY, CUBE_ARRAY)
    const struct { const Made* m; VkImageViewType vt; uint32_t layers; } views[kRegions] = {
        {&i1, VK_IMAGE_VIEW_TYPE_1D, 1}, {&i2, VK_IMAGE_VIEW_TYPE_2D, 1}, {&i3, VK_IMAGE_VIEW_TYPE_3D, 1}, {&i2, VK_IMAGE_VIEW_TYPE_CUBE, 6},
        {&i1, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 6}, {&i2, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 6}, {&i2, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6}};
    for (uint32_t r = 0; r < kRegions; r++)
        for (const bool storage : {false, true}) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = views[r].m->im, vi.viewType = views[r].vt, vi.format = VK_FORMAT_R8G8B8A8_UNORM;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, storage ? 1u : kLevels, 0, views[r].layers};
            if (storage && (views[r].vt == VK_IMAGE_VIEW_TYPE_CUBE || views[r].vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            VkImageViewUsageCreateInfo uci{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
            uci.usage = storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
            vi.pNext = &uci;
            VkImageDescriptorInfoEXT ii{VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
            ii.pView = &vi, ii.layout = VK_IMAGE_LAYOUT_GENERAL;
            VkResourceDescriptorInfoEXT ri{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
            ri.type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ri.data.pImage = &ii;
            uint8_t d[kImageDesc];
            std::memset(d, 0, sizeof d);
            VkHostAddressRangeEXT dst{d, (size_t)kImageDesc};
            VK(pWrite(g.dev, 1, &ri, &dst));
            uint8_t* base = heaps.res.map + (uint64_t)((storage ? kRegions : 0) + r) * kSlots * kImageDesc;
            for (uint32_t i = 0; i < kSlots; i++) std::memcpy(base + (uint64_t)i * kImageDesc, d, kImageDesc);
        }
    for (const bool cmp : {false, true}) {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = ci.minFilter = VK_FILTER_LINEAR, ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        ci.maxLod = 16, ci.compareEnable = cmp, ci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        uint8_t d[kSamplerDesc];
        VkHostAddressRangeEXT dst{d, (size_t)kSamplerDesc};
        VK(pWriteS(g.dev, 1, &ci, &dst));
        for (uint32_t i = 0; i < kSamplers; i++) std::memcpy(heaps.smp.map + (uint64_t)((cmp ? kSamplers : 0) + i) * kSamplerDesc, d, kSamplerDesc);
    }
}

/// Our vertex stage: a GRID x GRID grid of quads over the target, every generic location written with a hash of the corner
VkShaderEXT grid_vs(float lo, float hi) {
    static std::map<std::pair<float, float>, VkShaderEXT> made;
    if (const auto it = made.find({lo, hi}); it != made.end()) return it->second;
    char src[4096];
    std::snprintf(src, sizeof src, R"(#version 460
#define GRID 24u
layout(location = 0) out vec4 o[32];
uint h(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
void main() {
    const uint q = uint(gl_VertexIndex) / 6u, c = uint(gl_VertexIndex) %% 6u;
    const uint cx = (c == 1u || c == 2u || c == 4u) ? 1u : 0u, cy = (c == 2u || c == 4u || c == 5u) ? 1u : 0u;
    const uint x = q %% GRID + cx, y = q / GRID + cy;
    gl_Position = vec4(float(x) / float(GRID) * 2.0 - 1.0, float(y) / float(GRID) * 2.0 - 1.0, 0.5, 1.0);
    for (uint l = 0u; l < 32u; l++) {
        const uint b = h(x * 7919u + y * 104729u + l * 1299709u);
        o[l] = mix(vec4(%.9g), vec4(%.9g), vec4(float(b & 255u), float(b >> 8 & 255u), float(b >> 16 & 255u), float(b >> 24)) / 255.0);
    }
}
)", lo, hi);
    const std::string dir = "/tmp/meshlab_gridvs_" + std::to_string(getpid());
    if (std::system(("mkdir -p " + dir).c_str())) die("mkdir");
    FILE* f = std::fopen((dir + "/g.vert").c_str(), "w");
    std::fputs(src, f);
    std::fclose(f);
    if (std::system(("glslangValidator -V --target-env vulkan1.3 -o " + dir + "/g.spv " + dir + "/g.vert > /dev/null").c_str())) die("glslang failed on the grid vertex stage");
    const auto code = read_spv(dir + "/g.spv");
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    VkShaderDescriptorSetAndBindingMappingInfoEXT mi{VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT};
    ci.pNext = &mi;
    ci.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
    ci.stage = VK_SHADER_STAGE_VERTEX_BIT, ci.nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
    ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT, ci.codeSize = code.size() * 4, ci.pCode = code.data(), ci.pName = "main";
    VkShaderEXT o;
    VK(g.vkCreateShadersEXT(g.dev, 1, &ci, nullptr, &o));
    return made[{lo, hi}] = o;
}

struct FsDraw {
    const Draw* d = nullptr;
    const Program* p = nullptr;
    std::vector<Buf> ubo;
    Buf table;
    uint8_t push[256]{};
    uint32_t push_bytes = 0;
    VkShaderEXT fs[2]{};
    uint32_t outputs = 0;   // (colour outputs the stage writes: a mask, from the pass's targets)
};

/// A captured vertex draw made ready: its uniform buffers, push data, and the two fragment stages (A: the translator's as
/// captured; B: the replacement's SPIR-V, with the captured mappings)
FsDraw fs_prepare(const Frame& f, const Draw& d, const std::string& repl) {
    FsDraw r;
    r.d = &d;
    const auto pit = f.programs.find(d.d.key);
    if (pit == f.programs.end()) die("draw %u: program %016llx not in the capture", d.d.seq, (unsigned long long)d.d.key);
    r.p = &pit->second;
    const Program& p = *r.p;
    for (size_t i = 0; i < d.ubos.size(); i++) {
        const mcap::McDrawUbo& u = d.ubos[i];
        std::vector<uint8_t> bytes;
        if (u.blob != ~0u) bytes = f.blobs[u.blob].bytes;
        else if (u.index == 1)
            for (const Stage& s : p.stages)
                if (s.h.nvn_stage == u.stage) bytes = s.c1;
        Buf b = make_buf(std::max<uint64_t>(bytes.size(), 64 << 10) + 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(b.map, bytes.data(), bytes.size());
        r.ubo.push_back(b);
    }
    r.table = make_buf(8 * std::max<size_t>(1, r.ubo.size()), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    for (size_t i = 0; i < r.ubo.size(); i++) std::memcpy(r.table.map + 8 * i, &r.ubo[i].addr, 8);
    r.push_bytes = d.d.push_bytes;
    std::memcpy(r.push, d.d.push, sizeof r.push);
    std::memset(r.push + 64, 0, 24);
    std::memset(r.push + 96, 0, 24);
    std::memcpy(r.push + 88, &guest.windows.addr, 8);
    std::memcpy(r.push + 120, &r.table.addr, 8);
    for (const auto& q : p.pushed)
        for (size_t i = 0; i < p.ubos.size(); i++)
            if (p.ubos[i].stage == q.stage && p.ubos[i].index == q.index && q.push_off + 8 <= sizeof r.push) std::memcpy(r.push + q.push_off, &r.ubo[i].addr, 8);
    const Stage* fs = p.stage(VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!fs) die("program %016llx has no fragment stage", (unsigned long long)p.h.key);
    const std::string ka = std::to_string(p.h.key) + "/fsA";
    auto it = shader_cache.find(ka);
    if (it == shader_cache.end()) {
        std::string err;
        VkShaderEXT o = make_shader(VK_SHADER_STAGE_FRAGMENT_BIT, fs->spirv, fs->maps.data(), fs->h.maps, &err);
        if (!o) die("program %016llx: fragment stage not made: %s", (unsigned long long)p.h.key, err.c_str());
        it = shader_cache.emplace(ka, o).first;
    }
    r.fs[0] = r.fs[1] = it->second;
    if (!repl.empty()) {
        char b[64];
        std::snprintf(b, sizeof b, "/%016llx.spv", (unsigned long long)p.h.codes[1]);
        const auto code = read_spv(repl + b);
        if (code.empty()) die("no replacement %s%s", repl.c_str(), b);
        const std::string kb = std::to_string(p.h.key) + "/fsB";
        auto jt = shader_cache.find(kb);
        if (jt == shader_cache.end()) {
            std::string err;
            VkShaderEXT o = make_shader(VK_SHADER_STAGE_FRAGMENT_BIT, code, fs->maps.data(), fs->h.maps, &err);
            if (!o) die("program %016llx: the replacement fragment stage not made: %s", (unsigned long long)p.h.key, err.c_str());
            jt = shader_cache.emplace(kb, o).first;
        }
        r.fs[1] = jt->second;
    }
    for (uint32_t i = 0; i < d.d.n && i < 8; i++)
        if (d.d.color[i]) r.outputs |= 1u << i;
    return r;
}

struct FsTarget {
    std::vector<Img> color;   // (an R32G32B32A32_SFLOAT image per output, null where none)
    Img depth;
    uint32_t w = 0, h = 0;
};
FsTarget fs_target(uint32_t outputs, uint32_t w, uint32_t h) {
    FsTarget t;
    t.w = w, t.h = h;
    for (int i = 0; i < 8; i++) t.color.push_back(outputs >> i & 1 ? make_img(VK_FORMAT_R32G32B32A32_SFLOAT, w, h, VK_SAMPLE_COUNT_1_BIT, false) : Img{});
    t.depth = make_img(VK_FORMAT_D32_SFLOAT, w, h, VK_SAMPLE_COUNT_1_BIT, true);
    return t;
}
constexpr uint32_t kGrid = 24;
/// One draw of the grid with stage fs into t (cleared first when clear)
void fs_record(VkCommandBuffer cb, const FsDraw& r, VkShaderEXT fs, const FsTarget& t, bool clear, uint32_t reps, float lo, float hi) {
    std::vector<VkRenderingAttachmentInfo> ca;
    uint32_t n = 0;
    for (int i = 0; i < 8; i++)
        if (t.color[i].im) n = i + 1;
    for (uint32_t i = 0; i < n; i++) {
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = t.color[i].view, a.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        a.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.clearValue.color.uint32[0] = a.clearValue.color.uint32[1] = a.clearValue.color.uint32[2] = a.clearValue.color.uint32[3] = 0x7fbadbadu;
        ca.push_back(a);
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = t.depth.view, da.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    da.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD, da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {t.w, t.h}}, ri.layerCount = 1;
    ri.colorAttachmentCount = n, ri.pColorAttachments = ca.data(), ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cb, &ri);
    Dyn dyn{};
    dyn.vp = {0, 0, (float)t.w, (float)t.h, 0, 1};
    dyn.sc = {{0, 0}, {t.w, t.h}};
    dyn.cull = VK_CULL_MODE_NONE, dyn.front = VK_FRONT_FACE_COUNTER_CLOCKWISE, dyn.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    dyn.dt = VK_TRUE, dyn.dw = VK_FALSE, dyn.dop = VK_COMPARE_OP_ALWAYS;   // (depth always passes: every rep shades every pixel)
    dyn.poly = VK_POLYGON_MODE_FILL, dyn.samples = VK_SAMPLE_COUNT_1_BIT, dyn.n = n;
    for (uint32_t i = 0; i < n; i++) dyn.wm[i] = 0xf, dyn.en[i] = VK_FALSE, dyn.eq[i] = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
    set_state(cb, dyn);
    g.vkCmdSetVertexInputEXT(cb, 0, nullptr, 0, nullptr);
    VkShaderStageFlagBits st[6] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                   VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_MESH_BIT_EXT};
    VkShaderEXT sh[6] = {grid_vs(lo, hi), fs, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    g.vkCmdBindShadersEXT(cb, 6, st, sh);
    push(cb, r.push, std::max<uint32_t>(r.push_bytes, 128));
    for (uint32_t k = 0; k < reps; k++) vkCmdDraw(cb, kGrid * kGrid * 6, 1, 0, 0);
    vkCmdEndRendering(cb);
}

void fs_to_attachment(VkCommandBuffer cb, const FsTarget& t) {
    Target tt;
    for (const Img& i : t.color)
        if (i.im) tt.color.push_back(i);
    tt.depth = t.depth;
    to_attachment(cb, tt);
}

/// The target's colour outputs read back (each R32G32B32A32: w*h*4 words)
std::vector<std::vector<uint32_t>> fs_read(const FsTarget& t) {
    std::vector<std::vector<uint32_t>> out(8);
    const uint64_t bytes = 16ull * t.w * t.h;
    for (int i = 0; i < 8; i++) {
        if (!t.color[i].im) continue;
        Buf b = make_buf(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VkCommandBuffer cb = new_cb();
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK(vkBeginCommandBuffer(cb, &bi));
        VkImageMemoryBarrier2 br{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        br.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, br.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        br.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, br.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        br.oldLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, br.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        br.image = t.color[i].im, br.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &br;
        vkCmdPipelineBarrier2(cb, &di);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, r.imageExtent = {t.w, t.h, 1};
        vkCmdCopyImageToBuffer(cb, t.color[i].im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.b, 1, &r);
        VK(vkEndCommandBuffer(cb));
        VkFence f = new_fence();
        submit(cb, f);
        VK(vkWaitForFences(g.dev, 1, &f, VK_TRUE, ~0ull));
        out[i].assign((const uint32_t*)b.map, (const uint32_t*)b.map + 4ull * t.w * t.h);
    }
    return out;
}

/// fs mode: compare and / or time the fragment stages of the chosen vertex draws
int fs_main(const Frame& f, const std::vector<const Draw*>& draws, const std::string& repl, bool compare, bool bench, uint32_t reps, int rounds,
            uint32_t w, uint32_t h, float lo, float hi) {
    noise_heap();
    int different = 0, equal = 0;
    for (const Draw* d : draws) {
        FsDraw r = fs_prepare(f, *d, repl);
        if (!r.outputs) r.outputs = 1;
        if (compare) {
            FsTarget ta = fs_target(r.outputs, w, h), tb = fs_target(r.outputs, w, h);
            VkCommandBuffer cb = new_cb();
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            VK(vkBeginCommandBuffer(cb, &bi));
            fs_to_attachment(cb, ta), fs_to_attachment(cb, tb);
            heaps_bind(cb);
            fs_record(cb, r, r.fs[0], ta, true, 1, lo, hi);
            fs_record(cb, r, r.fs[1], tb, true, 1, lo, hi);
            VK(vkEndCommandBuffer(cb));
            VkFence fe = new_fence();
            submit(cb, fe);
            if (vkWaitForFences(g.dev, 1, &fe, VK_TRUE, 10ull * 1000 * 1000 * 1000) != VK_SUCCESS) die("the compare draw did not finish");
            const auto A = fs_read(ta), B = fs_read(tb);
            uint64_t words = 0, diff = 0, max_ulp = 0, written = 0, nan_a = 0, by_comp[4]{}, rows[16]{};
            std::string first;
            for (int i = 0; i < 8; i++)
                for (size_t k = 0; k < A[i].size(); k++) {
                    words++;
                    written += A[i][k] != 0x7fbadbadu;
                    if (A[i][k] != A[i][k] || std::isnan(*(const float*)&A[i][k])) nan_a += A[i][k] != 0x7fbadbadu;
                    if (A[i][k] == B[i][k]) continue;
                    if (!diff) {
                        char b[200];
                        std::snprintf(b, sizeof b, "target %d pixel (%zu, %zu) component %zu: %08x (%g) against %08x (%g)", i, k / 4 % w, k / 4 / w, k % 4,
                                      A[i][k], *(const float*)&A[i][k], B[i][k], *(const float*)&B[i][k]);
                        first = b;
                    }
                    diff++;
                    by_comp[k % 4]++;
                    rows[std::min<uint64_t>(15, k / 4 / w * 16 / h)]++;
                    max_ulp = std::max(max_ulp, ulps(A[i][k], B[i][k]));
                }
            (diff ? different : equal)++;
            std::printf("draw #%u %016llx (fragment %016llx): %s: %llu words, %llu written by the translator's stage (%llu NaN), %llu different",
                        d->d.seq, (unsigned long long)d->d.key, (unsigned long long)r.p->h.codes[1], diff ? "DIFFERENT" : "EQUAL",
                        (unsigned long long)words, (unsigned long long)written, (unsigned long long)nan_a, (unsigned long long)diff);
            if (diff) {
                std::printf(" (max %llu ulp; by component %llu %llu %llu %llu; by sixteenth of the rows", (unsigned long long)max_ulp,
                            (unsigned long long)by_comp[0], (unsigned long long)by_comp[1], (unsigned long long)by_comp[2], (unsigned long long)by_comp[3]);
                for (uint64_t x : rows) std::printf(" %llu", (unsigned long long)x);
                std::printf("); first: %s", first.c_str());
            }
            std::printf("\n");
        }
        if (bench) {
            FsTarget t = fs_target(r.outputs, w, h);
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP, qi.queryCount = 2;
            VkQueryPool qp;
            VK(vkCreateQueryPool(g.dev, &qi, nullptr, &qp));
            VkCommandBuffer cbs[2];
            for (int k = 0; k < 2; k++) {
                VkCommandBuffer cb = cbs[k] = new_cb();
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                VK(vkBeginCommandBuffer(cb, &bi));
                vkCmdResetQueryPool(cb, qp, 0, 2);
                fs_to_attachment(cb, t);
                heaps_bind(cb);
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 0);
                fs_record(cb, r, r.fs[k], t, true, reps, lo, hi);
                vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 1);
                VK(vkEndCommandBuffer(cb));
            }
            VkFence fe = new_fence();
            auto run = [&](int k) {
                submit(cbs[k], fe);
                if (vkWaitForFences(g.dev, 1, &fe, VK_TRUE, 10ull * 1000 * 1000 * 1000) != VK_SUCCESS) die("bench: a submission did not finish");
                VK(vkResetFences(g.dev, 1, &fe));
                uint64_t ts[2];
                VK(vkGetQueryPoolResults(g.dev, qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
                return (double)(ts[1] - ts[0]) * g.props.limits.timestampPeriod;
            };
            run(0), run(1);
            std::vector<double> ns[2];
            for (int q = 0; q < rounds; q++) {
                const int first = q & 1;
                ns[first].push_back(run(first));
                ns[1 - first].push_back(run(1 - first));
            }
            auto pct = [](std::vector<double> x, double p) { std::sort(x.begin(), x.end()); return x[std::min(x.size() - 1, (size_t)(p * x.size()))]; };
            const double px = (double)w * h * reps;
            std::printf("bench draw #%u fragment %016llx: %ux%u x %u reps, %d rounds\n", d->d.seq, (unsigned long long)r.p->h.codes[1], w, h, reps, rounds);
            const char* names[2] = {"translator", "replacement"};
            for (int k = 0; k < (repl.empty() ? 1 : 2); k++)
                std::printf("  %-11s median %.3f ms (p10 %.3f, p90 %.3f): %.2f us a million pixels\n", names[k], pct(ns[k], .5) / 1e6, pct(ns[k], .1) / 1e6,
                            pct(ns[k], .9) / 1e6, pct(ns[k], .5) / px * 1e6 / 1e3);
            if (!repl.empty()) {
                std::vector<double> ratio;
                for (int q = 0; q < rounds; q++) ratio.push_back(ns[1][q] / ns[0][q]);
                std::printf("  replacement / translator: median %.3f (p10 %.3f, p90 %.3f)\n", pct(ratio, .5), pct(ratio, .1), pct(ratio, .9));
            }
        }
    }
    if (compare) std::printf("fs compare: %d equal, %d different\n", equal, different);
    return different ? 1 : 0;
}

}  // namespace fsb
