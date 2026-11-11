// (native-vk, RX_VK_DESC_HEAP=1) VK_EXT_descriptor_heap: the game's texture handles as heap indices
// (work/assets/native/descriptor-heap.md, desc-heap-impl.md).
//
// A program drawn through heap shader objects keeps Eden's binding SPIR-V; its bindings are remapped when its shader objects
// are made (VkShaderDescriptorSetAndBindingMappingInfoEXT):
// - a texture whose handle is a word of a driver bank (c0 / c2) -> HEAP_WITH_INDIRECT_INDEX + useCombinedImageSamplerIndex: the
//   driver reads the NVN handle (texture id | sampler id << 20) from the bank as uploaded for the draw (its address in push data)
//   and samples resource region[view type][id] with sampler region[compare][sampler id]
// - any other texture handle (a user constant buffer, a handle program, two words) -> HEAP_WITH_PUSH_INDEX: the handle as the CPU
//   reads it, in push data
// - uniform / storage buffers and storage images -> HEAP_WITH_PUSH_INDEX into the draw's block of a per-recording ring of buffer
//   (16 B) and image (64 B) descriptors
// The resource heap: the ring, one region of 65,536 image descriptors per view type, a zero tail (ids up to 0xFFFFF from the last
// region read zeros: a null descriptor). The sampler heap: two regions of 4,096 (plain, compare).
//
// Entries are written lazily, by the draw's scan of its handles (vk_draw.cpp heap_texture): an entry no work in flight used is
// written in place on the CPU; one used by submitted work is copied on the GPU by the recording's upload command buffer (before
// the recording, after the work before it); one used earlier in the recording being made is copied in the stream (pass ended,
// barrier, copy, barrier). Game registrations and retired textures invalidate their entries at the next submission's start.
#pragma once

#include <cstdint>
#include <vector>

#include "vk_internal.h"
#include "vk_shader.h"

namespace mkwvk {

struct Texture;

/// RX_VK_DESC_HEAP=1 (read once): the descriptor heap is asked for
bool desc_heap_wanted();
/// RX_VK_DESC_HEAP_CHECK=1: each heap draw resolves its textures and samplers the old way too, and compares
bool desc_heap_check();
/// RX_VK_DESC_HEAP_LOG=1: the heap's counters every 600 presents
bool desc_heap_log();
/// The heap is in use: wanted, the device has it (and shader objects), the heaps made
bool desc_heap_on();

/// (vk_main.cpp create_device) the extensions it needs, when wanted and present; its feature structures chained at *tail
void desc_heap_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want, void**& tail);
/// (after vkGetPhysicalDeviceFeatures2) whether the device has what it needs (its features left enabled or turned off)
void desc_heap_features_resolved(bool shader_objects, bool buffer_device_address);
/// (after vkCreateDevice and load_device) the entry points, the heaps, the classic descriptor commands wrapped
void desc_heap_device_created();

#ifdef VK_EXT_descriptor_heap
constexpr uint32_t kHeapSlots = 1u << 16;   // (image descriptors a view type region holds: the game's pool is 33,068)
constexpr uint32_t kHeapRegions = 7;        // (VkImageViewType 0..6)
constexpr uint32_t kHeapSamplers = 4096;    // (12-bit sampler ids)

/// What a texture element of a heap program reads
struct HeapTex {
    enum Kind : uint8_t { Indirect, PushIndex, Null } kind = Null;
    uint8_t region = 0;       // (its view type)
    bool compare = false;     // (a depth-compare sampler)
    uint16_t push_off = 0;    // (Indirect: its bank's address; PushIndex: its handle, in push data)
    uint16_t bank = 0;        // (Indirect: index into HeapProgram::banks)
    uint32_t cbuf_offset = 0; // (Indirect: the handle word in the bank)
};
/// A program variant's heap layout (made once, from its stages' Shader::Info, in Eden's binding order)
struct HeapProgram {
    uint64_t sig = 0;   // (what its shader objects are made from besides the SPIR-V: the binary cache's key)
    std::vector<std::vector<VkDescriptorSetAndBindingMappingEXT>> maps;   // by p.stages index
    uint32_t block_bytes = 0;       // (the draw's ring block, a multiple of 64)
    uint32_t push_bytes = 0;        // (push data: the classic push constants, then the heap's words)
    std::vector<uint32_t> buf_at;   // per buffer element (build_descriptors' dw().bufs order): its offset in the block
    std::vector<uint32_t> img_at;   // per storage image element: its offset in the block
    std::vector<HeapTex> tex;       // per texture element (build order)
    struct Bank { uint16_t push_off; uint8_t stage; uint8_t index; uint32_t bytes; };
    std::vector<Bank> banks;        // (driver banks the Indirect textures read: uploaded per draw, their addresses pushed)
};
/// p's heap layout; null when the heap is off or p cannot be drawn through it (texel buffers, texture arrays, the mesh fallback,
/// push data over the device's limit): such a program keeps push descriptors and its classic shader objects
const HeapProgram* desc_heap_program(const Program& p);
/// The push data offset of the heap's words (after the classic push constants)
uint32_t desc_heap_push_base();

// ---- the draw path (one recording thread: the heap is off with RX_VK_PARALLEL) ----
/// A recording begins: its ring block, the submission's value and serial (a new submission: invalidations drained)
void desc_heap_begin_recording(int slot, uint64_t value, uint64_t completed);
/// The heaps bound on cmd before a heap draw or dispatch (again after any classic descriptor command)
void desc_heap_bind(VkCommandBuffer cmd);
/// After a heap draw: the classic bindings pushed before are gone (the command buffer is in heap mode)
bool desc_heap_classic_since(VkCommandBuffer cmd);
/// The draw's push data (offset 0: the classic push constants, then the heap's words)
void desc_heap_push(VkCommandBuffer cmd, const void* data, uint32_t bytes);
/// A block of the recording's ring: its push index (64-byte units) and where to write; false: the ring is full
bool desc_heap_ring_alloc(uint32_t bytes, uint32_t* index, uint8_t** at);

/// A resource heap entry's state (region = view type, slot = texture id)
struct HeapEntry {
    uint32_t ver = 0;           // (the slot's registration version it was resolved at)
    uint32_t epoch = ~0u;       // (the texture epoch it was last checked in: a texture is touched once a submission)
    uint64_t gen = ~0ull;       // (texture_destroy_generation, or with RX_VK_TEX_SERIAL a miss's texture_miss_generation)
    Texture* tex = nullptr;     // (what it resolved to; null: the dummy, a null descriptor)
    uint32_t tex_id = 0;
    bool valid = false;         // (resolved since its last invalidation)
    bool sampling = false;      // (its texture's contents are read: draw_sampled)
    VkImageView view = VK_NULL_HANDLE;   // (what the heap holds, or will once its copy runs; null: a null descriptor)
    uint64_t last_value = 0;    // (the submission value of the last recording that used it)
    uint32_t last_rec = 0;      // (that recording's serial)
};
HeapEntry& desc_heap_entry(uint32_t region, uint32_t slot);
struct HeapSamplerEntry {
    uint32_t ver = 0;
    bool valid = false;
    VkSampler sampler = VK_NULL_HANDLE;
    uint64_t last_value = 0;
    uint32_t last_rec = 0;
};
HeapSamplerEntry& desc_heap_sampler_entry(bool compare, uint32_t id);
/// The recording being made uses it
void desc_heap_use(HeapEntry& e);
void desc_heap_use(HeapSamplerEntry& e);

/// How a new descriptor reaches an entry
enum class HeapPut { InPlace, Upload, InStream };
/// What writing entry e needs now (InStream: it was used earlier in this recording)
HeapPut desc_heap_put_kind(uint64_t last_value, uint32_t last_rec);
/// The 64 (image) or 16 (sampler) descriptor bytes for a view / sampler: null view -> a null descriptor
bool desc_heap_encode_image(VkImageView view, bool storage, uint8_t out[64]);
bool desc_heap_encode_sampler(VkSampler s, uint8_t out[16]);
bool desc_heap_encode_buffer(VkBuffer b, VkDeviceSize offset, VkDeviceSize range, bool storage, uint8_t out[16]);
/// Byte offsets in the heaps
uint64_t desc_heap_image_offset(uint32_t region, uint32_t slot);
uint64_t desc_heap_sampler_offset(bool compare, uint32_t id);
uint8_t* desc_heap_resource_map();
uint8_t* desc_heap_sampler_map();
VkBuffer desc_heap_resource_buffer();
VkBuffer desc_heap_sampler_buffer();
/// The reverse map: entry (region, slot) now points at texture id (0: none); was at old_id
void desc_heap_entry_texture(uint32_t region, uint32_t slot, uint32_t old_id, uint32_t new_id);

/// Counters (RX_VK_DESC_HEAP_LOG, the frame line)
enum HeapCount {
    HC_DRAWS, HC_DISPATCHES, HC_CLASSIC, HC_SCANS, HC_RESOLVES, HC_IN_PLACE, HC_UPLOAD, HC_IN_STREAM, HC_SAMPLER_WRITES,
    HC_REG_INVALID, HC_REG_HOT, HC_RETIRED, HC_RING_FULL, HC_CHECK_BAD, HC_CHECK_DRAWS, HC_BANK_UPLOADS, HC_BIG_ID, HC_BORDER, HC_TWO_VIEWS,
    HC_COUNT
};
void desc_heap_count(HeapCount c, int64_t n = 1);
/// (the open question: is a slot sampled as two view types in play?) slot sampled as view type region at registration version ver
void desc_heap_note_view(uint32_t slot, uint32_t region, uint32_t ver);
#endif

/// (vk_replay.cpp) a game registration of texture slot id (its entries go stale at the next submission)
void desc_heap_registered(int id);
/// (vk_texture.cpp destroy_later, the texture lock held) texture id leaves the cache: the entries pointing at it go
void desc_heap_texture_retired(uint32_t id);
/// (vk_texture.cpp make_view) a view made: what it was made from, for the heap's descriptors (no VkImageView in a heap)
void desc_heap_view_made(VkImageView view, const VkImageViewCreateInfo& ci, VkImageUsageFlags usage);
void desc_heap_view_destroyed(VkImageView view);
/// (vk_draw.cpp sampler_make) a sampler made, from ci (its pNext chain copied: border colour, reduction)
void desc_heap_sampler_made(VkSampler s, const VkSamplerCreateInfo& ci);
/// (each present) the counters, every 600 presents with RX_VK_DESC_HEAP_LOG=1
void desc_heap_frame();

} // namespace mkwvk
