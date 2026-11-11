// (native-vk, RX_VK_DESC_HEAP=1) VK_EXT_descriptor_heap: the heaps, the programs' mappings, the entries' state (vk_desc_heap.h).
// The draw path's half (the scan of a draw's handles, the bank uploads, the ring's descriptors, push data) is in vk_draw.cpp.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "vk_desc_heap.h"
#include "vk_texture.h"

namespace mkwvk {

uint64_t replay_completed_value();   // (vk_replay.cpp: the timeline value the GPU has finished)
uint32_t so_push_size();             // (vk_draw.cpp: the classic push constants' size)
bool texture_handle_is_pointer(const Stage& st, const Shader::TextureDescriptor& d, uint32_t k);   // (vk_draw.cpp)
VkImageViewType texture_view_type(Shader::TextureType t);   // (vk_draw.cpp view_type)

namespace {

bool env_on(const char* name) {
    const char* e = getenv(name);
    return e && *e && atoi(e) != 0;
}

} // namespace

bool desc_heap_wanted() {
    static const bool on = env_on("RX_VK_DESC_HEAP");
    return on;
}
bool desc_heap_check() {
    static const bool on = desc_heap_wanted() && env_on("RX_VK_DESC_HEAP_CHECK");
    return on;
}
bool desc_heap_log() {
    static const bool on = desc_heap_wanted() && env_on("RX_VK_DESC_HEAP_LOG");
    return on;
}

#ifndef VK_EXT_descriptor_heap
// (Vulkan headers without the extension: the heap is never on)
bool desc_heap_on() { return false; }
void desc_heap_device_extensions(const std::vector<VkExtensionProperties>&, std::vector<const char*>&, void**&) {
    if (desc_heap_wanted()) logf("vk: descriptor heap off: these Vulkan headers have no VK_EXT_descriptor_heap");
}
void desc_heap_features_resolved(bool, bool) {}
void desc_heap_device_created() {}
void desc_heap_registered(int) {}
void desc_heap_texture_retired(uint32_t) {}
void desc_heap_view_made(VkImageView, const VkImageViewCreateInfo&, VkImageUsageFlags) {}
void desc_heap_view_destroyed(VkImageView) {}
void desc_heap_sampler_made(VkSampler, const VkSamplerCreateInfo&) {}
void desc_heap_frame() {}
#else

namespace {

// ---- the device ----
bool ext_ok = false;      // (the extension and its dependencies are there and asked for)
bool feat_ok = false;     // (descriptorHeap and the rest enabled)
bool heap_ready = false;  // (the heaps made)
VkPhysicalDeviceDescriptorHeapFeaturesEXT heap_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
VkPhysicalDeviceMaintenance5FeaturesKHR m5_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
VkPhysicalDeviceDescriptorHeapPropertiesEXT props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
const char* off_reason = "not asked for";

PFN_vkWriteResourceDescriptorsEXT p_write_res = nullptr;
PFN_vkWriteSamplerDescriptorsEXT p_write_smp = nullptr;
PFN_vkCmdBindResourceHeapEXT p_bind_res = nullptr;
PFN_vkCmdBindSamplerHeapEXT p_bind_smp = nullptr;
PFN_vkCmdPushDataEXT p_push_data = nullptr;

// ---- the heaps ----
struct HeapBuf {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0, reserved_at = 0, reserved = 0;
};
HeapBuf res_heap, smp_heap;
uint64_t isz = 64, ssz = 16, bsz = 16;   // (descriptor sizes)
uint64_t ring_slot_bytes = 1u << 20;     // (RX_VK_DESC_HEAP_RING_KB: a recording's ring)
constexpr int kRingSlots = 64;           // (the recording slots: vk_replay.cpp's 48, with room)
uint64_t region_base = 0;                // (the first image region, after the rings)
uint64_t tail_at = 0;                    // (the zero tail: Null textures point here)

bool make_heap(HeapBuf& h, VkDeviceSize used, VkDeviceSize min_reserved, VkBufferUsageFlags extra) {
    const VkDeviceSize align = 256;
    h.reserved_at = (used + align - 1) & ~(align - 1);
    h.reserved = (min_reserved + align - 1) & ~(align - 1);
    h.size = h.reserved_at + h.reserved;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = h.size;
    bi.usage = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extra;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &h.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev.device, h.buffer, &mr);
    const VkMemoryPropertyFlags want[2] = {
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,   // (BAR)
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    uint32_t type = UINT32_MAX;
    for (VkMemoryPropertyFlags w : want) {
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++)
            if ((mr.memoryTypeBits >> i & 1) && (dev.memory.memoryTypes[i].propertyFlags & w) == w) type = i;
        if (type != UINT32_MAX) break;
    }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fi;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev.device, &ai, nullptr, &h.memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(dev.device, h.buffer, h.memory, 0) != VK_SUCCESS) return false;
    void* p = nullptr;
    if (vkMapMemory(dev.device, h.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
    h.map = (uint8_t*)p;
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = h.buffer;
    h.address = vkGetBufferDeviceAddress(dev.device, &da);
    return h.address != 0;
}

// ---- the classic descriptor commands, wrapped: any of them puts a command buffer in classic mode, which unbinds the heaps ----
std::atomic<uint64_t> classic_epoch{1};
PFN_vkCmdPushDescriptorSetKHR orig_push_set = nullptr;
PFN_vkCmdBindDescriptorSets orig_bind_sets = nullptr;
PFN_vkCmdPushConstants orig_push_consts = nullptr;
VKAPI_ATTR void VKAPI_CALL wrap_push_set(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n,
                                         const VkWriteDescriptorSet* w) {
    classic_epoch.fetch_add(1, std::memory_order_relaxed);
    orig_push_set(c, bp, l, set, n, w);
}
VKAPI_ATTR void VKAPI_CALL wrap_bind_sets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n,
                                          const VkDescriptorSet* s, uint32_t nd, const uint32_t* d) {
    classic_epoch.fetch_add(1, std::memory_order_relaxed);
    orig_bind_sets(c, bp, l, first, n, s, nd, d);
}
VKAPI_ATTR void VKAPI_CALL wrap_push_consts(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t n,
                                            const void* v) {
    classic_epoch.fetch_add(1, std::memory_order_relaxed);
    orig_push_consts(c, l, st, off, n, v);
}
VkCommandBuffer bound_cmd = VK_NULL_HANDLE;   // (the command buffer the heaps were last bound on, and the classic epoch then)
uint64_t bound_epoch = 0;

// ---- views and samplers: what they were made from ----
struct ViewCi {
    VkImage image;
    VkImageViewType type;
    VkFormat format;
    VkComponentMapping cm;
    VkImageSubresourceRange range;
    VkImageUsageFlags usage;
};
std::mutex view_mu;
std::unordered_map<VkImageView, ViewCi> view_cis;
struct SamplerCi {
    VkSamplerCreateInfo ci;
    VkSamplerReductionMode reduction;
    bool has_reduction;
    bool custom_border;
};
std::mutex samp_mu;
std::unordered_map<VkSampler, SamplerCi> sampler_cis;

// ---- entries ----
std::unique_ptr<HeapEntry[]> entries[kHeapRegions];
HeapSamplerEntry samp_entries[2][kHeapSamplers];
std::unordered_map<uint32_t, std::vector<uint32_t>> by_texture;   // (texture id -> region << 20 | slot)

// ---- the recording ----
int rec_slot = 0;
uint64_t rec_value = 0, rec_completed = 0, last_submission = ~0ull;
uint32_t rec_serial = 0;
uint64_t ring_used = 0;

std::mutex inval_mu;
std::vector<int> registered;        // (slots the game registered since the last drain)
std::vector<uint32_t> retired;      // (textures the cache let go since)

std::atomic<int64_t> counts[HC_COUNT];
const char* const kCountNames[HC_COUNT] = {"draws", "dispatches", "classic", "scans", "resolves", "in place", "upload copies",
                                           "in-stream copies", "sampler writes", "registrations invalidated", "of them hot",
                                           "retired", "ring full", "CHECK mismatches", "CHECK draws", "bank uploads",
                                           "ids past the regions", "custom borders as black", "slots sampled as a second view type", "ring blocks kept", "buffer descriptors from the cache",
                                           "buffer descriptors written"};

uint64_t mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

/// (a new submission) the game's registrations and the textures let go since: their entries are stale. One no work in flight
/// used is nulled now; a hot one stays (nothing samples it before a scan resolves it again, which copies on the GPU)
void drain_invalidations() {
    std::vector<int> regs;
    std::vector<uint32_t> gone;
    {
        std::lock_guard lk(inval_mu);
        regs.swap(registered);
        gone.swap(retired);
    }
    auto drop = [](uint32_t region, uint32_t slot, bool reg) {
        HeapEntry& e = entries[region][slot];
        if (!e.valid && !e.view) return;
        e.valid = false;
        if (reg) desc_heap_count(HC_REG_INVALID);
        if (desc_heap_put_kind(e.last_value, e.last_rec) != HeapPut::InPlace) {
            if (reg) desc_heap_count(HC_REG_HOT);   // (the small ring's guard: a scan copies it on the GPU before it is sampled)
            return;
        }
        if (e.view) {
            uint8_t d[64];
            if (desc_heap_encode_image(VK_NULL_HANDLE, false, d))
                std::memcpy(res_heap.map + desc_heap_image_offset(region, slot), d, isz);
            desc_heap_entry_texture(region, slot, e.tex_id, 0);
            e.view = VK_NULL_HANDLE, e.tex = nullptr, e.tex_id = 0;
        }
    };
    for (int id : regs) {
        if (id < 0 || (uint32_t)id >= kHeapSlots) continue;
        for (uint32_t r = 0; r < kHeapRegions; r++)
            if (entries[r]) drop(r, (uint32_t)id, true);
    }
    for (uint32_t id : gone) {
        const auto it = by_texture.find(id);
        if (it == by_texture.end()) continue;
        const std::vector<uint32_t> list = it->second;   // (drop edits the map)
        for (uint32_t rs : list) {
            const uint32_t region = rs >> 20, slot = rs & 0xFFFFF;
            if (region < kHeapRegions && entries[region] && entries[region][slot].tex_id == id) drop(region, slot, false), desc_heap_count(HC_RETIRED);
        }
        by_texture.erase(id);
    }
}

// ---- programs ----
struct ProgEntry {
    uint64_t key = 0, variant = 0;
    VkShaderModule mods[8]{};
    uint32_t nmods = 0;
    std::unique_ptr<HeapProgram> hp;   // (null: classic)
};
std::mutex prog_mu;
std::unordered_map<const Program*, ProgEntry> progs;

bool same(const ProgEntry& e, const Program& p) {
    if (e.key != p.key || e.variant != p.variant || e.nmods != std::min<size_t>(p.stages.size(), 8)) return false;
    for (uint32_t i = 0; i < e.nmods; i++)
        if (e.mods[i] != p.stages[i].module) return false;
    return true;
}

std::unique_ptr<HeapProgram> make_program(const Program& p, const char** why) {
    if (p.fallback) return *why = "the mesh fallback", nullptr;
    auto hp = std::make_unique<HeapProgram>();
    const uint32_t base = desc_heap_push_base();
    uint32_t push = base + 8;   // (the ring index, then the banks' addresses, then pushed handles)
    // (first: the buffers' and images' places in the block, and the banks)
    uint32_t nbuf = 0, nimg = 0;
    for (const Stage& st : p.stages) {
        for (const auto& d : st.info.constant_buffer_descriptors) nbuf += d.count;
        for (const auto& d : st.info.storage_buffers_descriptors) nbuf += d.count;
        if (!st.info.texture_buffer_descriptors.empty() || !st.info.image_buffer_descriptors.empty())
            return *why = "texel buffers", nullptr;
        for (const auto& d : st.info.texture_descriptors)
            if (d.count != 1) return *why = "a texture array", nullptr;
        for (const auto& d : st.info.image_descriptors)
            if (d.count != 1) return *why = "an image array", nullptr;
        nimg += (uint32_t)st.info.image_descriptors.size();
    }
    const uint32_t img0 = (16 * nbuf + 63) & ~63u;
    hp->block_bytes = std::max<uint32_t>(64, (img0 + 64 * nimg + 63) & ~63u);
    uint32_t buf_i = 0, img_i = 0, binding = 0;
    const uint64_t ring0 = 0;   // (the rings are the heap's first bytes)
    hp->maps.resize(p.stages.size());
    std::vector<std::pair<size_t, size_t>> tex_map;   // (each texture element's mapping: stage, index)
    for (size_t si = 0; si < p.stages.size(); si++) {
        const Stage& st = p.stages[si];
        auto& maps = hp->maps[si];
        auto map_of = [&](uint32_t b, VkSpirvResourceTypeFlagsEXT mask, VkDescriptorMappingSourceEXT src) {
            VkDescriptorSetAndBindingMappingEXT m{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT};
            m.descriptorSet = 0, m.firstBinding = b, m.bindingCount = 1, m.resourceMask = mask, m.source = src;
            return m;
        };
        auto ring_map = [&](uint32_t b, uint32_t at, VkSpirvResourceTypeFlagsEXT mask, uint32_t array_stride) {
            VkDescriptorSetAndBindingMappingEXT m = map_of(b, mask, VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT);
            auto& pi = m.sourceData.pushIndex;
            pi.heapOffset = (uint32_t)(ring0 + at), pi.pushOffset = base, pi.heapIndexStride = 64, pi.heapArrayStride = array_stride;
            return m;
        };
        for (const auto& d : st.info.constant_buffer_descriptors) {
            maps.push_back(ring_map(binding++, 16 * buf_i, VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT, 16));
            for (uint32_t k = 0; k < d.count; k++) hp->buf_at.push_back(16 * buf_i++);
        }
        for (const auto& d : st.info.storage_buffers_descriptors) {
            maps.push_back(ring_map(binding++, 16 * buf_i,
                                    VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT,
                                    16));
            for (uint32_t k = 0; k < d.count; k++) hp->buf_at.push_back(16 * buf_i++);
        }
        // (texel buffers: none, refused above)
        for (const auto& d : st.info.texture_descriptors) {
            const uint32_t b = binding++;
            HeapTex t;
            t.region = (uint8_t)std::min<uint32_t>((uint32_t)texture_view_type(d.type), kHeapRegions - 1);
            t.compare = d.is_depth;
            const uint32_t region_off = (uint32_t)(region_base + (uint64_t)t.region * kHeapSlots * isz);
            const uint32_t smp_off = t.compare ? (uint32_t)(kHeapSamplers * ssz) : 0;
            if (texture_handle_is_pointer(st, d, 0)) {   // (not a handle: the dummy, as the classic path's)
                t.kind = HeapTex::Null;
                VkDescriptorSetAndBindingMappingEXT m = map_of(b, VK_SPIRV_RESOURCE_TYPE_ALL_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT);
                m.sourceData.constantOffset.heapOffset = (uint32_t)tail_at;
                m.sourceData.constantOffset.heapArrayStride = (uint32_t)isz;
                m.sourceData.constantOffset.samplerHeapOffset = smp_off;
                m.sourceData.constantOffset.samplerHeapArrayStride = (uint32_t)ssz;
                maps.push_back(m);
            } else if ((d.cbuf_index == 0 || d.cbuf_index == 2) && !d.has_secondary && d.handle_program == ~0u && d.cbuf_offset + 4 <= 0xA00 &&
                       !(d.cbuf_offset & 3)) {
                // (a driver bank's word: the bank uploaded for the draw, its address in push data, the driver reads the handle)
                t.kind = HeapTex::Indirect;
                t.cbuf_offset = d.cbuf_offset;
                uint32_t bi = 0;
                for (; bi < hp->banks.size(); bi++)
                    if (hp->banks[bi].stage == si && hp->banks[bi].index == d.cbuf_index) break;
                if (bi == hp->banks.size()) hp->banks.push_back({0, (uint8_t)si, (uint8_t)d.cbuf_index, 0});
                hp->banks[bi].bytes = std::max<uint32_t>(hp->banks[bi].bytes, (d.cbuf_offset + 4 + 15) & ~15u);
                t.bank = (uint16_t)bi;
                VkDescriptorSetAndBindingMappingEXT m = map_of(b, VK_SPIRV_RESOURCE_TYPE_ALL_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_EXT);
                auto& ii = m.sourceData.indirectIndex;
                ii.heapOffset = region_off, ii.pushOffset = 0 /* (set below, once the banks are placed) */, ii.addressOffset = d.cbuf_offset;
                ii.heapIndexStride = (uint32_t)isz, ii.heapArrayStride = (uint32_t)isz, ii.useCombinedImageSamplerIndex = VK_TRUE;
                ii.samplerHeapOffset = smp_off, ii.samplerPushOffset = 0, ii.samplerAddressOffset = d.cbuf_offset;
                ii.samplerHeapIndexStride = (uint32_t)ssz, ii.samplerHeapArrayStride = (uint32_t)ssz;
                maps.push_back(m);
            } else {   // (read on the CPU, as today: pushed)
                t.kind = HeapTex::PushIndex;
                VkDescriptorSetAndBindingMappingEXT m = map_of(b, VK_SPIRV_RESOURCE_TYPE_ALL_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT);
                auto& pi = m.sourceData.pushIndex;
                pi.heapOffset = region_off, pi.pushOffset = 0 /* (below) */, pi.heapIndexStride = (uint32_t)isz, pi.heapArrayStride = (uint32_t)isz;
                pi.useCombinedImageSamplerIndex = VK_TRUE;
                pi.samplerHeapOffset = smp_off, pi.samplerPushOffset = 0, pi.samplerHeapIndexStride = (uint32_t)ssz, pi.samplerHeapArrayStride = (uint32_t)ssz;
                maps.push_back(m);
            }
            hp->tex.push_back(t);
            tex_map.push_back({si, maps.size() - 1});
        }
        for (size_t k = 0; k < st.info.image_descriptors.size(); k++) {
            const uint32_t at = img0 + 64 * img_i++;
            maps.push_back(ring_map(binding++, at, VK_SPIRV_RESOURCE_TYPE_READ_ONLY_IMAGE_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT, 64));
            hp->img_at.push_back(at);
        }
    }
    // (push data: the banks' addresses, then the pushed handles)
    for (auto& bk : hp->banks) bk.push_off = (uint16_t)push, push += 8;
    for (size_t ti = 0; ti < hp->tex.size(); ti++) {
        HeapTex& t = hp->tex[ti];
        VkDescriptorSetAndBindingMappingEXT& m = hp->maps[tex_map[ti].first][tex_map[ti].second];
        if (t.kind == HeapTex::Indirect) {
            t.push_off = hp->banks[t.bank].push_off;
            m.sourceData.indirectIndex.pushOffset = m.sourceData.indirectIndex.samplerPushOffset = t.push_off;
        } else if (t.kind == HeapTex::PushIndex) {
            t.push_off = (uint16_t)push, push += 4;
            m.sourceData.pushIndex.pushOffset = m.sourceData.pushIndex.samplerPushOffset = t.push_off;
        }
    }
    hp->push_bytes = (push + 3) & ~3u;
    if (hp->push_bytes > props.maxPushDataSize) return *why = "push data over the device's limit", nullptr;
    // (the binary cache's key: every mapping's numbers, the push layout, the heap's layout)
    uint64_t h = mix64(0x48454150ull, (uint64_t)hp->push_bytes << 32 | hp->block_bytes);
    h = mix64(h, (uint64_t)isz << 48 | (uint64_t)ssz << 32 | (uint64_t)kHeapSlots);
    h = mix64(h, region_base), h = mix64(h, tail_at);
    for (size_t si = 0; si < hp->maps.size(); si++)
        for (const auto& m : hp->maps[si]) {
            h = mix64(h, (uint64_t)si << 56 | (uint64_t)m.firstBinding << 32 | (uint64_t)m.source << 16 | m.bindingCount);
            h = mix64(h, (uint64_t)m.resourceMask);
            const uint32_t* w = reinterpret_cast<const uint32_t*>(&m.sourceData);
            for (size_t k = 0; k < sizeof m.sourceData / 4; k++)
                if (!(m.source == VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT && (k == 2 || k == 3)))   // (its pointer's words)
                    h = mix64(h, w[k]);
        }
    hp->sig = h;
    return hp;
}

} // namespace

// ---- the device ----
void desc_heap_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want, void**& tail) {
    if (!desc_heap_wanted()) return;
    auto has = [&](const char* n) {
        for (const auto& e : exts)
            if (!strcmp(e.extensionName, n)) return true;
        return false;
    };
    if (!has(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME)) return void(off_reason = "the device has no VK_EXT_descriptor_heap");
    // (Vulkan 1.3 here: VK_KHR_maintenance5 (or VK_KHR_extended_flags) beside it)
    if (!has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) return void(off_reason = "the device has no VK_KHR_maintenance5");
    bool listed = false;
    for (const char* w : want) listed |= !strcmp(w, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    if (!listed) want.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    want.push_back(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
    *tail = &heap_feat, tail = &heap_feat.pNext;
    *tail = &m5_feat, tail = &m5_feat.pNext;
    ext_ok = true;
}
void desc_heap_features_resolved(bool shader_objects, bool buffer_device_address) {
    heap_feat.descriptorHeapCaptureReplay = VK_FALSE;
    if (!ext_ok) return;
    if (!heap_feat.descriptorHeap) off_reason = "descriptorHeap not supported";
    else if (!m5_feat.maintenance5) off_reason = "maintenance5 not supported";
    else if (!shader_objects) off_reason = "it needs shader objects (RX_VK_SHADER_OBJECTS)";
    else if (!buffer_device_address) off_reason = "it needs buffer device addresses";
    else if (getenv("RX_VK_PARALLEL") && atoi(getenv("RX_VK_PARALLEL")) > 1) off_reason = "RX_VK_PARALLEL records on several threads";
    else feat_ok = true;
    if (!feat_ok) heap_feat.descriptorHeap = VK_FALSE;   // (the extension enabled, its feature not)
}

void desc_heap_device_created() {
    if (!desc_heap_wanted()) return;
    if (!feat_ok) return void(logf("vk: descriptor heap off: %s", off_reason));
    p_write_res = (PFN_vkWriteResourceDescriptorsEXT)vkGetDeviceProcAddr(dev.device, "vkWriteResourceDescriptorsEXT");
    p_write_smp = (PFN_vkWriteSamplerDescriptorsEXT)vkGetDeviceProcAddr(dev.device, "vkWriteSamplerDescriptorsEXT");
    p_bind_res = (PFN_vkCmdBindResourceHeapEXT)vkGetDeviceProcAddr(dev.device, "vkCmdBindResourceHeapEXT");
    p_bind_smp = (PFN_vkCmdBindSamplerHeapEXT)vkGetDeviceProcAddr(dev.device, "vkCmdBindSamplerHeapEXT");
    p_push_data = (PFN_vkCmdPushDataEXT)vkGetDeviceProcAddr(dev.device, "vkCmdPushDataEXT");
    if (!p_write_res || !p_write_smp || !p_bind_res || !p_bind_smp || !p_push_data)
        return void(logf("vk: descriptor heap off: its entry points are missing"));
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &props;
    vkGetPhysicalDeviceProperties2(dev.physical, &p2);
    isz = std::max<uint64_t>(props.imageDescriptorSize, props.imageDescriptorAlignment);
    ssz = std::max<uint64_t>(props.samplerDescriptorSize, props.samplerDescriptorAlignment);
    bsz = std::max<uint64_t>(props.bufferDescriptorSize, props.bufferDescriptorAlignment);
    if (isz > 64 || ssz > 16 || bsz > 16 || 64 % isz || 16 % bsz)   // (the ring's layout: 16-byte buffer and 64-byte image places)
        return void(logf("vk: descriptor heap off: descriptor sizes %llu / %llu / %llu do not fit its layout", (unsigned long long)isz,
                         (unsigned long long)ssz, (unsigned long long)bsz));
    isz = 64, bsz = 16;   // (the strides the mappings use)
    if (const char* e = getenv("RX_VK_DESC_HEAP_RING_KB")) ring_slot_bytes = (uint64_t)std::clamp(atoi(e), 64, 16384) << 10;
    region_base = (uint64_t)kRingSlots * ring_slot_bytes;
    tail_at = region_base + (uint64_t)kHeapRegions * kHeapSlots * isz;
    // (an id up to 0xFFFFF from the last region's base stays inside the heap, in the zero tail)
    const uint64_t res_used = region_base + ((uint64_t)(kHeapRegions - 1) * kHeapSlots + (1u << 20)) * isz;
    if (!make_heap(res_heap, res_used, props.minResourceHeapReservedRange, VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
        !make_heap(smp_heap, 2ull * kHeapSamplers * ssz, props.minSamplerHeapReservedRange, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return void(logf("vk: descriptor heap off: its memory was not made"));
    if (res_heap.size > props.maxResourceHeapSize || smp_heap.size > props.maxSamplerHeapSize)
        return void(logf("vk: descriptor heap off: heaps over the device's limits"));
    // (every entry a null descriptor: what the driver writes for one, copied; all zeros on RADV)
    uint8_t null_img[64]{};
    VkResourceDescriptorInfoEXT ri{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    ri.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    VkHostAddressRangeEXT dst{null_img, (size_t)isz};
    bool zero = true;
    if (dev.null_descriptor && p_write_res(dev.device, 1, &ri, &dst) == VK_SUCCESS)
        for (uint8_t b : null_img) zero &= b == 0;
    if (zero) {
        std::memset(res_heap.map, 0, res_heap.reserved_at);
    } else {
        std::memset(res_heap.map, 0, region_base);
        for (uint64_t at = region_base; at + isz <= res_heap.reserved_at; at += isz) std::memcpy(res_heap.map + at, null_img, isz);
    }
    std::memset(smp_heap.map, 0, smp_heap.reserved_at);
    {   // (sampler entries before a scan writes them: linear, clamped)
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sc.magFilter = sc.minFilter = VK_FILTER_LINEAR, sc.maxLod = 1000.0f;
        uint8_t d[16]{};
        VkHostAddressRangeEXT sd{d, (size_t)ssz};
        if (p_write_smp(dev.device, 1, &sc, &sd) == VK_SUCCESS)
            for (uint32_t i = 0; i < 2 * kHeapSamplers; i++) std::memcpy(smp_heap.map + i * ssz, d, ssz);
    }
    for (auto& e : entries) e.reset(new HeapEntry[kHeapSlots]);
    // (every classic descriptor command, in every file, unbinds the heaps of its command buffer: counted, the heaps bound again)
    orig_push_set = vkx::vkCmdPushDescriptorSetKHR, orig_bind_sets = vkx::vkCmdBindDescriptorSets, orig_push_consts = vkx::vkCmdPushConstants;
    if (orig_push_set) vkx::vkCmdPushDescriptorSetKHR = wrap_push_set;
    if (orig_bind_sets) vkx::vkCmdBindDescriptorSets = wrap_bind_sets;
    if (orig_push_consts) vkx::vkCmdPushConstants = wrap_push_consts;
    heap_ready = true;
    logf("vk: descriptor heap ready: %u slots x %u view types (%llu B each), %u samplers x 2 (%llu B), rings %d x %llu KB; resource heap %llu MB "
         "(%s), sampler heap %llu KB; push data %llu B (RX_VK_DESC_HEAP_CHECK=%d)", kHeapSlots, kHeapRegions, (unsigned long long)isz,
         kHeapSamplers, (unsigned long long)ssz, kRingSlots, (unsigned long long)(ring_slot_bytes >> 10), (unsigned long long)(res_heap.size >> 20),
         zero ? "null descriptors are zeros" : "null descriptors copied", (unsigned long long)(smp_heap.size >> 10),
         (unsigned long long)props.maxPushDataSize, desc_heap_check() ? 1 : 0);
}

bool desc_heap_on() {
    return heap_ready;
}

uint32_t desc_heap_push_base() {
    return (so_push_size() + 7) & ~7u;
}

const HeapProgram* desc_heap_program(const Program& p) {
    if (!heap_ready) return nullptr;
    std::lock_guard lk(prog_mu);
    ProgEntry& e = progs[&p];
    if (e.nmods && same(e, p)) return e.hp.get();
    e = ProgEntry{};
    e.key = p.key, e.variant = p.variant, e.nmods = (uint32_t)std::min<size_t>(p.stages.size(), 8);
    for (uint32_t i = 0; i < e.nmods; i++) e.mods[i] = p.stages[i].module;
    const char* why = "";
    e.hp = make_program(p, &why);
    if (!e.hp) {
        static std::atomic<int> told{0};
        if (told++ < 40) logf("vk: descriptor heap: program %016llx keeps push descriptors (%s)", (unsigned long long)p.key, why);
    }
    return e.hp.get();
}

// ---- the recording ----
void desc_heap_begin_recording(int slot, uint64_t value, uint64_t completed) {
    if (!heap_ready) return;
    rec_slot = std::clamp(slot, 0, kRingSlots - 1);
    rec_value = value, rec_completed = completed;
    rec_serial++;
    ring_used = 0;
    bound_cmd = VK_NULL_HANDLE;
    if (value != last_submission) {   // (a new submission, not a part of one: registrations come between submissions)
        last_submission = value;
        drain_invalidations();
    }
}
void desc_heap_bind(VkCommandBuffer cmd) {
    const uint64_t ep = classic_epoch.load(std::memory_order_relaxed);
    if (cmd == bound_cmd && ep == bound_epoch) return;
    VkBindHeapInfoEXT rb{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    rb.heapRange = {res_heap.address, res_heap.size}, rb.reservedRangeOffset = res_heap.reserved_at, rb.reservedRangeSize = res_heap.reserved;
    VkBindHeapInfoEXT sb{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
    sb.heapRange = {smp_heap.address, smp_heap.size}, sb.reservedRangeOffset = smp_heap.reserved_at, sb.reservedRangeSize = smp_heap.reserved;
    p_bind_res(cmd, &rb);
    p_bind_smp(cmd, &sb);
    bound_cmd = cmd, bound_epoch = ep;
}
bool desc_heap_classic_since(VkCommandBuffer cmd) {
    return cmd != bound_cmd || classic_epoch.load(std::memory_order_relaxed) != bound_epoch;
}
bool desc_heap_ring_alloc(uint32_t bytes, uint32_t* index, uint8_t** at) {
    bytes = (bytes + 63) & ~63u;
    if (ring_used + bytes > ring_slot_bytes) {
        desc_heap_count(HC_RING_FULL);
        static std::atomic<int> told{0};
        if (told++ < 8)
            logf("vk: descriptor heap: a recording's ring is full (%llu KB, RX_VK_DESC_HEAP_RING_KB): the draw is left out",
                 (unsigned long long)(ring_slot_bytes >> 10));
        return false;
    }
    const uint64_t off = (uint64_t)rec_slot * ring_slot_bytes + ring_used;
    ring_used += bytes;
    *index = (uint32_t)(off / 64);
    *at = res_heap.map + off;
    return true;
}
void desc_heap_push(VkCommandBuffer cmd, const void* data, uint32_t bytes) {
    VkPushDataInfoEXT pi{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
    pi.offset = 0;
    pi.data = {data, bytes};
    p_push_data(cmd, &pi);
}

HeapEntry& desc_heap_entry(uint32_t region, uint32_t slot) {
    return entries[region][slot];
}
HeapSamplerEntry& desc_heap_sampler_entry(bool compare, uint32_t id) {
    return samp_entries[compare ? 1 : 0][id];
}
void desc_heap_use(HeapEntry& e) {
    e.last_value = rec_value, e.last_rec = rec_serial;
}
void desc_heap_use(HeapSamplerEntry& e) {
    e.last_value = rec_value, e.last_rec = rec_serial;
}
HeapPut desc_heap_put_kind(uint64_t last_value, uint32_t last_rec) {
    if (!last_value) return HeapPut::InPlace;
    if (last_rec == rec_serial && last_value == rec_value) return HeapPut::InStream;
    if (last_value > rec_completed) rec_completed = std::max(rec_completed, replay_completed_value());
    return last_value > rec_completed ? HeapPut::Upload : HeapPut::InPlace;
}

bool desc_heap_encode_image(VkImageView view, bool storage, uint8_t out[64]) {
    VkResourceDescriptorInfoEXT ri{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    ri.type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    VkImageViewUsageCreateInfo uci{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkImageDescriptorInfoEXT ii{VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT};
    std::memset(out, 0, 64);
    if (view) {
        ViewCi c;
        {
            std::lock_guard lk(view_mu);
            const auto it = view_cis.find(view);
            if (it == view_cis.end()) return false;
            c = it->second;
        }
        uci.usage = c.usage;
        vi.pNext = &uci;
        vi.image = c.image, vi.viewType = c.type, vi.format = c.format, vi.components = c.cm, vi.subresourceRange = c.range;
        ii.pView = &vi;
        ii.layout = VK_IMAGE_LAYOUT_GENERAL;
        ri.data.pImage = &ii;
    } else if (!dev.null_descriptor) {
        return true;   // (zeros: what RDNA reads as nothing)
    }
    VkHostAddressRangeEXT dst{out, (size_t)isz};
    return p_write_res(dev.device, 1, &ri, &dst) == VK_SUCCESS;
}
bool desc_heap_encode_sampler(VkSampler s, uint8_t out[16]) {
    SamplerCi c;
    {
        std::lock_guard lk(samp_mu);
        const auto it = sampler_cis.find(s);
        if (it == sampler_cis.end()) return false;
        c = it->second;
    }
    VkSamplerReductionModeCreateInfo rm{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    c.ci.pNext = nullptr;
    if (c.has_reduction) rm.reductionMode = c.reduction, c.ci.pNext = &rm;
    if (c.custom_border) {   // (a custom border colour needs an index registered with the heap: not yet; transparent black)
        c.ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        desc_heap_count(HC_BORDER);
    }
    std::memset(out, 0, 16);
    VkHostAddressRangeEXT dst{out, (size_t)ssz};
    return p_write_smp(dev.device, 1, &c.ci, &dst) == VK_SUCCESS;
}
bool desc_heap_encode_buffer(VkBuffer b, VkDeviceSize offset, VkDeviceSize range, bool storage, uint8_t out[16]) {
    std::memset(out, 0, 16);
    VkResourceDescriptorInfoEXT ri{VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT};
    ri.type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    VkDeviceAddressRangeEXT ar{};
    if (b && range && range != VK_WHOLE_SIZE) {
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = b;
        const VkDeviceAddress a = vkGetBufferDeviceAddress(dev.device, &da);
        if (!a) return false;
        ar.address = a + offset, ar.size = range;
        ri.data.pAddressRange = &ar;
    } else if (!dev.null_descriptor) {
        return true;   // (zeros)
    }
    // (a buffer descriptor is made from its address, size and type alone: kept by them, the driver asked once. RX_VK_DESC_HEAP_BUFCACHE=0: not)
    static const bool cache_on = !getenv("RX_VK_DESC_HEAP_BUFCACHE") || atoi(getenv("RX_VK_DESC_HEAP_BUFCACHE")) != 0;
    struct Cached { uint64_t addr, size; uint32_t type; uint8_t bytes[16]; };
    static thread_local Cached* cache = nullptr;   // (on the heap, not in TLS: a big TLS block in the .so slows every thread_local of it)
    if (cache_on && !cache) cache = new Cached[4096]();
    Cached* c = nullptr;
    if (cache_on) {
        const uint64_t h = (ar.address * 0x9E3779B97F4A7C15ull) ^ (ar.size * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t)ri.type;
        c = &cache[(h >> 52) & 4095];
        if (c->addr == ar.address && c->size == ar.size && c->type == (uint32_t)ri.type) {
            std::memcpy(out, c->bytes, bsz);
            desc_heap_count(HC_BUF_CACHED);
            return true;
        }
    }
    VkHostAddressRangeEXT dst{out, (size_t)bsz};
    uint8_t tmp[16]{};
    if (c) dst.address = tmp;
    if (p_write_res(dev.device, 1, &ri, &dst) != VK_SUCCESS) return false;
    desc_heap_count(HC_BUF_WRITTEN);
    if (c) {
        c->addr = ar.address, c->size = ar.size, c->type = (uint32_t)ri.type;
        std::memcpy(c->bytes, tmp, bsz);
        std::memcpy(out, tmp, bsz);
    }
    return true;
}
uint64_t desc_heap_image_offset(uint32_t region, uint32_t slot) {
    return region_base + ((uint64_t)region * kHeapSlots + slot) * isz;
}
uint64_t desc_heap_sampler_offset(bool compare, uint32_t id) {
    return ((compare ? kHeapSamplers : 0u) + (uint64_t)id) * ssz;
}
uint8_t* desc_heap_resource_map() { return res_heap.map; }
uint8_t* desc_heap_sampler_map() { return smp_heap.map; }
VkBuffer desc_heap_resource_buffer() { return res_heap.buffer; }
VkBuffer desc_heap_sampler_buffer() { return smp_heap.buffer; }

void desc_heap_entry_texture(uint32_t region, uint32_t slot, uint32_t old_id, uint32_t new_id) {
    if (old_id == new_id) return;
    const uint32_t rs = region << 20 | slot;
    if (old_id) {
        const auto it = by_texture.find(old_id);
        if (it != by_texture.end()) {
            std::erase(it->second, rs);
            if (it->second.empty()) by_texture.erase(it);
        }
    }
    if (new_id) by_texture[new_id].push_back(rs);
}

void desc_heap_count(HeapCount c, int64_t n) {
    counts[c].fetch_add(n, std::memory_order_relaxed);
}
void desc_heap_note_view(uint32_t slot, uint32_t region, uint32_t ver) {
    static std::unique_ptr<uint32_t[]> seen_ver(new uint32_t[kHeapSlots]());
    static std::unique_ptr<uint8_t[]> seen_mask(new uint8_t[kHeapSlots]());
    if (seen_ver[slot] != ver) seen_ver[slot] = ver, seen_mask[slot] = 0;
    const uint8_t bit = (uint8_t)(1u << region);
    if (seen_mask[slot] & bit) return;
    if (seen_mask[slot]) {
        desc_heap_count(HC_TWO_VIEWS);
        static std::atomic<int> told{0};
        if (told++ < 30) logf("vk: descriptor heap: slot %#x (version %u) sampled as view types %#x and %u", slot, ver, seen_mask[slot], region);
    }
    seen_mask[slot] |= bit;
}

// ---- hooks ----
std::atomic<int64_t> presents_seen{0};
void desc_heap_registered(int id) {
    // (RX_VK_DESC_HEAP_REGLOG=<file>, heap on or off: "present id" per game registration, for the id ring's size and the rate)
    static FILE* reglog = [] {
        const char* e = getenv("RX_VK_DESC_HEAP_REGLOG");
        FILE* f = e && *e ? fopen(e, "w") : nullptr;
        if (f) setvbuf(f, nullptr, _IOLBF, 0);   // (the run ends with the process killed)
        return f;
    }();
    if (reglog) {
        static std::mutex m;
        std::lock_guard lk(m);
        fprintf(reglog, "%lld %d\n", (long long)presents_seen.load(std::memory_order_relaxed), id);
    }
    if (!heap_ready) return;
    std::lock_guard lk(inval_mu);
    registered.push_back(id);
}
void desc_heap_texture_retired(uint32_t id) {
    if (!heap_ready) return;
    std::lock_guard lk(inval_mu);
    retired.push_back(id);
}
void desc_heap_view_made(VkImageView view, const VkImageViewCreateInfo& ci, VkImageUsageFlags usage) {
    if (!desc_heap_wanted() || !view) return;
    std::lock_guard lk(view_mu);
    view_cis[view] = {ci.image, ci.viewType, ci.format, ci.components, ci.subresourceRange, usage};
}
void desc_heap_view_destroyed(VkImageView view) {
    if (!desc_heap_wanted() || !view) return;
    std::lock_guard lk(view_mu);
    view_cis.erase(view);
}
void desc_heap_sampler_made(VkSampler s, const VkSamplerCreateInfo& ci) {
    if (!desc_heap_wanted() || !s) return;
    SamplerCi c{};
    c.ci = ci;
    c.ci.pNext = nullptr;
    for (const VkBaseInStructure* n = (const VkBaseInStructure*)ci.pNext; n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO)
            c.has_reduction = true, c.reduction = ((const VkSamplerReductionModeCreateInfo*)n)->reductionMode;
    c.custom_border = ci.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT || ci.borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT;
    std::lock_guard lk(samp_mu);
    sampler_cis[s] = c;
}
void desc_heap_frame() {
    presents_seen.fetch_add(1, std::memory_order_relaxed);
    if (!heap_ready || !desc_heap_log()) return;
    static int64_t frames = 0;
    static int64_t last[HC_COUNT]{};
    if (++frames % 600) return;
    char line[1400];
    int n = snprintf(line, sizeof line, "vk: descriptor heap, last 600 presents:");
    for (int i = 0; i < HC_COUNT && n < 1300; i++) {
        const int64_t v = counts[i].load(std::memory_order_relaxed);
        n += snprintf(line + n, sizeof line - n, " %s %lld%s", kCountNames[i], (long long)(v - last[i]), i + 1 < HC_COUNT ? "," : "");
        last[i] = v;
    }
    logf("%s", line);
}

#endif   // VK_EXT_descriptor_heap

} // namespace mkwvk
