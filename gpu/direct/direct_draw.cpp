// A draw (direct_draw.h). The state's conversion is gpu/vk/vk_draw.cpp's (dynamic_state, so_state, so_bind, push_bytes,
// topology, vertex_format, target_write_mask) and the calls gpu/vk/eng_job.cpp's (on_draw), without the replay's diff caches
// (each command buffer sets everything once: RADV inherits no dynamic state) and without its special cases (the shadow and
// cascade depth biases, the EQ bias, footprints: plan rule 8).
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "direct_cmd.h"
#include "direct_draw.h"
#include "direct_heap.h"
#include "direct_image.h"
#include "direct_shader.h"
#include "direct_state.h"

namespace hsd {
bool mapped_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& map,
                   VkDeviceAddress& address);   // (direct_heap.cpp)
bool layer0_address(uint64_t gpu, uint64_t size, VkDeviceAddress& a);   // (direct_pools.cpp)

// ---- the frame arena ----
struct Arena {
    struct Chunk {
        uint8_t* map = nullptr;
        VkDeviceAddress address = 0;
        uint64_t size = 0;
    };
    std::vector<Chunk> chunks;
    size_t cur = 0;
    uint64_t used = 0;
};
void arena_reset(Arena* a) {
    if (a) a->cur = 0, a->used = 0;
}

namespace {

constexpr uint64_t kChunk = 4ull << 20;
std::atomic<int64_t> st_chunks{0};

bool arena_alloc(CmdSlot* slot, uint32_t bytes, uint8_t*& map, VkDeviceAddress& addr) {
    if (!slot) return false;
    if (!slot->arena) slot->arena = new Arena;
    Arena& a = *slot->arena;
    bytes = (bytes + 63) & ~63u;
    if (bytes > kChunk) return false;
    while (a.cur < a.chunks.size() && a.used + bytes > a.chunks[a.cur].size) a.cur++, a.used = 0;
    if (a.cur == a.chunks.size()) {
        Arena::Chunk c;
        VkBuffer b;
        VkDeviceMemory m;
        if (!mapped_buffer(kChunk, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, b, m, c.map, c.address)) return false;
        c.size = kChunk;
        a.chunks.push_back(c);
        a.used = 0;
        st_chunks.fetch_add(1, std::memory_order_relaxed);
    }
    Arena::Chunk& c = a.chunks[a.cur];
    map = c.map + a.used, addr = c.address + a.used;
    a.used += bytes;
    return true;
}

// ---- what a draw reads besides the arena: the window table (BDA), the zero buffer ----
uint64_t* window_table = nullptr;
VkDeviceAddress window_table_address = 0, null_window = 0, zero_address = 0;
VkBuffer zero_buffer = VK_NULL_HANDLE;
constexpr uint32_t kWindows = 512, kTableBytes = 8 * 520 + 128;

// ---- skips, by reason ----
enum Reason {
    R_DRAWN, R_MESH, R_INDIRECT, R_NO_TARGETS, R_CONDITION, R_DISABLED, R_TOPOLOGY, R_QUADS, R_NO_PROGRAM, R_NOT_PARSED,
    R_NOT_GRAPHICS, R_TRANSLATING, R_FAILED, R_STEP_A, R_VERTEX_MEM, R_INDEX_MEM, R_INDEX_U8, R_UBO_MEM, R_ARENA,
    R_RESOLVED_JOB, R_RESOLVED_LOAD, R_RESOLVED_OTHER, R_COUNT
};
const char* const kReasons[R_COUNT] = {
    "drawn", "mesh draws (M4)", "indirect draws (M4)", "no render targets set in the list", "under a render condition (M5)",
    "rendering disabled", "a topology Vulkan lacks", "quads (indices made on the CPU)", "no program bound in the list",
    "program not read yet", "program not vertex + fragment", "variant being translated", "variant does not translate",
    "step A cannot draw the variant", "a vertex buffer not in layer 0", "the index buffer not in layer 0", "8-bit indices",
    "a uniform buffer not in layer 0", "the frame arena full", "resolved in a job list (no command buffer: M3)",
    "resolved in a load-time list (M3)", "resolved in an unclassified list"};
// (M3b: the counters a draw bumps are the recording thread's own: a global atomic added to by every draw on every recording
// thread was a shared cache line. draw_log sums them, each block's last sum kept to report the change)
struct TStats {
    std::atomic<int64_t> by_kind[HSD_LIST_KINDS][R_COUNT];
    std::atomic<int64_t> ns, banks, tables, ub_copies, ub_reads, ub_bytes, past_range;
    int64_t seen_by_kind[HSD_LIST_KINDS][R_COUNT];   // (draw_log's: the sums it reported last)
    int64_t seen[7];
    TStats* next = nullptr;
};
std::atomic<TStats*> tstats_all{nullptr};
thread_local TStats* tstats = nullptr;
TStats& me() {
    if (!tstats) {
        TStats* t = new TStats();
        t->next = tstats_all.load(std::memory_order_acquire);
        while (!tstats_all.compare_exchange_weak(t->next, t, std::memory_order_release, std::memory_order_acquire)) {}
        tstats = t;
    }
    return *tstats;
}
inline void bump(std::atomic<int64_t>& c, int64_t n = 1) { c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
/// (draw_log, the Presentation Thread) the change of a thread counter since it last read it, summed over the threads
enum { T_NS, T_BANKS, T_TABLES, T_UB_COPIES, T_UB_READS, T_UB_BYTES, T_PAST };
int64_t take(std::atomic<int64_t> TStats::*f, int slot) {
    int64_t n = 0;
    for (TStats* t = tstats_all.load(std::memory_order_acquire); t; t = t->next) {
        const int64_t v = (t->*f).load(std::memory_order_relaxed);
        n += v - t->seen[slot], t->seen[slot] = v;
    }
    return n;
}
int64_t take_kind(int k, int r) {
    int64_t n = 0;
    for (TStats* t = tstats_all.load(std::memory_order_acquire); t; t = t->next) {
        const int64_t v = t->by_kind[k][r].load(std::memory_order_relaxed);
        n += v - t->seen_by_kind[k][r], t->seen_by_kind[k][r] = v;
    }
    return n;
}
std::atomic<int64_t> st_ub_updates{0}, st_gated[HSD_LIST_KINDS];   // (M3a: gated draws drawn)
std::atomic<int64_t> st_fixups{0}, st_fixup_lists{0}, st_ssbo_unbound{0};// (the variants' step A reasons: static strings, a few)
struct StepA {
    std::atomic<const char*> why{nullptr};
    std::atomic<int64_t> n[HSD_LIST_KINDS];
};
StepA step_a[16];
void count_step_a(const char* why, uint32_t kind) {
    for (StepA& s : step_a) {
        const char* have = s.why.load(std::memory_order_acquire);
        if (!have) {
            const char* want = nullptr;
            if (!s.why.compare_exchange_strong(want, why)) have = want;
            else have = why;
        }
        if (have == why) return void(s.n[kind].fetch_add(1, std::memory_order_relaxed));
    }
}
// (the layer and gate lists' draws, by the list's return address: which pass a UI draw is in)
struct Site {
    std::atomic<uint64_t> lr{0};
    std::atomic<int64_t> n[R_COUNT];
};
Site sites[24];
void count_site(uint64_t lr, Reason r) {
    for (Site& s : sites) {
        uint64_t have = s.lr.load(std::memory_order_acquire);
        if (!have) {
            uint64_t want = 0;
            have = s.lr.compare_exchange_strong(want, lr) ? lr : want;
        }
        if (have == lr) return void(s.n[r].fetch_add(1, std::memory_order_relaxed));
    }
}

struct Fixup {
    uint8_t* dst;
    uint64_t src;
    uint32_t bound;   // (HOS_DBG=deferredcheck: the word in the game's array when it was bound)
};
// (M3a, HOS_DBG=deferredcheck: how many deferred words change between their bind and the submit, and since the last submit
// that read the same word)
const bool deferred_check = dbg("deferredcheck");
std::mutex deferred_mu;
std::unordered_map<uint64_t, uint32_t> deferred_last;
std::atomic<int64_t> st_def_bind{0}, st_def_frame{0}, st_def_words{0};
struct Dyn {
    VkViewport vp;
    VkRect2D sc;
    float blend[4];
    uint32_t sref[2], scmp[2], swm[2];
    float bias[3];
    VkBool32 bias_en;
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPrimitiveTopology topo;
    VkBool32 restart, dt, dw;
    VkCompareOp dop;
    VkBool32 st;
    VkStencilOp sops[2][3];
    VkCompareOp scmpop[2];
    VkBool32 discard;
    VkPolygonMode poly;
    VkBool32 clamp, a2c;
    VkSampleCountFlagBits samples;
    uint32_t n;
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
};
struct BankCopy {
    uint32_t gen = ~0u, bytes = 0;
    VkDeviceAddress addr = 0;
};
}  // namespace

struct ListDraw {
    DState s;
    // (what the command buffer being recorded has: set again from nothing for each new one)
    VkCommandBuffer cb = VK_NULL_HANDLE;
    bool heap = false, dyn_valid = false, vi_valid = false, vb_valid = false, ib_valid = false, push_valid = false;
    VkShaderEXT vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    bool shaders = false;
    uint32_t vb_bound = 0;   // (the vertex bindings given a buffer in this command buffer)
    Dyn dyn;
    uint64_t vi_sig = 0;
    VkBuffer vb[16];
    VkDeviceSize vo[16];
    VkBuffer ib = VK_NULL_HANDLE;
    VkDeviceSize io = 0;
    VkIndexType it = VK_INDEX_TYPE_UINT16;
    uint8_t push[256];
    uint32_t push_n = 0;
    // (the list's: in its slot's arena, good while the list lives)
    BankCopy bank[kBankStages][2];
    const DrawProg* table_prog = nullptr;
    uint32_t table_ubo_gen = ~0u, table_bank_gen[kBankStages]{~0u, ~0u};
    VkDeviceAddress table_addr = 0;
    std::vector<VkDeviceAddress> table_entries;
    std::vector<Fixup> fixups;
    // (inline uniform updates, in order: later draws of the list read copies of their buffers with them applied)
    struct Update {
        uint64_t dst;
        uint32_t size, at;   // (its bytes at update_bytes[at])
    };
    std::vector<Update> updates;
    std::vector<uint8_t> update_bytes;
    struct UbCopy {
        uint64_t addr, size;
        size_t nupdates;
        VkDeviceAddress copy;
    };
    std::vector<UbCopy> ub_copies;
    // (M3b) a buffer the list updates: its bytes as the list's updates leave them, read once from guest memory where no update
    // covers them, the updates applied as they come; a draw's copy is [0, the program's used size) of it
    struct Shadow {
        uint64_t addr = 0, size = 0;
        std::vector<uint8_t> bytes;
        uint64_t valid = 0;     // (bytes [0, valid) hold the buffer: read and updated)
        size_t applied = 0;     // (the list's updates applied to them)
    };
    std::vector<Shadow> shadows;
    // (M3b) the game's uniform buffers' device addresses, per stage and index, kept while the binding is the same
    struct UboAddr {
        uint64_t addr = 0, size = 0;
        VkDeviceAddress dev = 0;
    };
    UboAddr ubo_addr[kBankStages][16];
    // (M3b) the vertex buffers' window spans, per stream, kept while the binding is the same; the last index buffer's
    struct Span {
        uint64_t addr = 0, size = 0;
        VkBuffer b = VK_NULL_HANDLE;
        VkDeviceSize o = 0;
    };
    Span vb_span[16], ib_span;
    // (the last pick, and what it was made from)
    const DrawProg* pick = nullptr;
    uint32_t pick_prog_gen = 0, pick_vtx_gen = 0, pick_bank_gen[kBankStages]{};
    uint64_t pick_ct = 0;
};

namespace {

ListDraw& ctx(List* l) {
    if (!l->dctx) l->dctx = new ListDraw;
    return *l->dctx;
}

VkCompareOp compare_op(uint32_t f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
VkStencilOp stencil_op(uint32_t o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
VkBlendFactor blend_factor(uint32_t f) {
    switch (f) {
    case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO;
    case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp blend_op(uint32_t e) {
    switch (e) {
    case NVN_BLEND_EQUATION_SUB: return VK_BLEND_OP_SUBTRACT;
    case NVN_BLEND_EQUATION_REVERSE_SUB: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case NVN_BLEND_EQUATION_MIN: return VK_BLEND_OP_MIN;
    case NVN_BLEND_EQUATION_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
/// Vulkan's topology; false: none (line loops, quad strips, patches); quads apart
bool topology(int prim, VkPrimitiveTopology& t) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
    case NVN_DRAW_PRIMITIVE_LINES: t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; return true;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; return true;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; return true;
    default: return false;
    }
}
VkFormat vertex_format(int f) {
    static const VkFormat r8[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT};
    static const VkFormat r16[] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT};
    static const VkFormat r32[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT};
    static const VkFormat rg8[] = {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT};
    static const VkFormat rg16[] = {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT};
    static const VkFormat rg32[] = {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT};
    static const VkFormat rgb8[] = {VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT};
    static const VkFormat rgb16[] = {VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT};
    static const VkFormat rgb32[] = {VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT};
    static const VkFormat rgba8[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT};
    static const VkFormat rgba16[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT};
    static const VkFormat rgba32[] = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT};
    static const struct { int lo; const VkFormat* f; } ranges[] = {
        {0x01, r8}, {0x05, r16}, {0x0A, r32}, {0x0D, rg8}, {0x11, rg16}, {0x16, rg32},
        {0x19, rgb8}, {0x1D, rgb16}, {0x22, rgb32}, {0x25, rgba8}, {0x29, rgba16}, {0x2E, rgba32}};
    static const int counts[] = {4, 5, 3, 4, 5, 3, 4, 5, 3, 4, 5, 3};
    for (int i = 0; i < 12; i++)
        if (f >= ranges[i].lo && f < ranges[i].lo + counts[i]) return ranges[i].f[f - ranges[i].lo];
    if (f >= 0x51 && f <= 0x68) {   // (*_UI2F / *_I2F: integers the shader converts)
        static const VkFormat us[12] = {VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R8G8_UINT,
                                        VK_FORMAT_R16G16_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R8G8B8_UINT,
                                        VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R8G8B8A8_UINT,
                                        VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R32G32B32A32_UINT};
        static const VkFormat ss[12] = {VK_FORMAT_R8_SINT, VK_FORMAT_R16_SINT, VK_FORMAT_R32_SINT, VK_FORMAT_R8G8_SINT,
                                        VK_FORMAT_R16G16_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R8G8B8_SINT,
                                        VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R8G8B8A8_SINT,
                                        VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32G32B32A32_SINT};
        return ((f - 0x51) & 1 ? ss : us)[(f - 0x51) / 2];
    }
    switch (f) {
    case NVN_FORMAT_RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case NVN_FORMAT_RGB10A2UI: case NVN_FORMAT_RGB10A2_UI2F: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case NVN_FORMAT_RGB10A2SN: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case NVN_FORMAT_RGB10A2I: case NVN_FORMAT_RGB10A2_I2F: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    case NVN_FORMAT_R11G11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case NVN_FORMAT_BGRA8: case NVN_FORMAT_BGRX8: return VK_FORMAT_B8G8R8A8_UNORM;
    case NVN_FORMAT_RGBX8: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}
/// 3-component 8/16-bit vertex formats this GPU does not fetch: read 4 (the 4th past the attribute)
VkFormat supported_vertex_format(VkFormat f) {
    static std::once_flag once;
    static VkFormat known[256];
    std::call_once(once, [] {
        for (int i = 0; i < 256; i++) {
            const VkFormat g = (VkFormat)i;
            VkFormatProperties p{};
            vkGetPhysicalDeviceFormatProperties(dev.physical, g, &p);
            VkFormat out = g;
            if (!(p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) switch (g) {
                case VK_FORMAT_R8G8B8_UNORM: out = VK_FORMAT_R8G8B8A8_UNORM; break;
                case VK_FORMAT_R8G8B8_SNORM: out = VK_FORMAT_R8G8B8A8_SNORM; break;
                case VK_FORMAT_R8G8B8_UINT: out = VK_FORMAT_R8G8B8A8_UINT; break;
                case VK_FORMAT_R8G8B8_SINT: out = VK_FORMAT_R8G8B8A8_SINT; break;
                case VK_FORMAT_R16G16B16_SFLOAT: out = VK_FORMAT_R16G16B16A16_SFLOAT; break;
                case VK_FORMAT_R16G16B16_UNORM: out = VK_FORMAT_R16G16B16A16_UNORM; break;
                case VK_FORMAT_R16G16B16_SNORM: out = VK_FORMAT_R16G16B16A16_SNORM; break;
                case VK_FORMAT_R16G16B16_UINT: out = VK_FORMAT_R16G16B16A16_UINT; break;
                case VK_FORMAT_R16G16B16_SINT: out = VK_FORMAT_R16G16B16A16_SINT; break;
                default: break;
                }
            known[i] = out;
        }
    });
    return (uint32_t)f < 256 ? known[f] : f;
}
uint8_t color_type(VkFormat f) {   // (Shader::AttributeType: 0 float, 1 signed, 2 unsigned)
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 2;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32A32_SINT: return 1;
    default: return 0;
    }
}
uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

/// The command buffer's first draw: everything set from nothing
void cb_fresh(ListDraw& d, VkCommandBuffer cb) {
    if (d.cb == cb) return;
    d.cb = cb;
    d.heap = d.dyn_valid = d.vi_valid = d.vb_valid = d.ib_valid = d.push_valid = d.shaders = false;
    d.vb_bound = 0;
}

void set_dynamic(ListDraw& x, VkCommandBuffer cb, const DState& s, const PassRec& p, VkPrimitiveTopology topo, const DrawProg& prog) {
    Dyn d;
    std::memset(&d, 0, sizeof d);
    // (the viewport upside down: NVN's NDC has +y up, Vulkan's down; none set: the whole target)
    float vx = s.viewport[0], vy = s.viewport[1], vw = s.viewport[2], vh = s.viewport[3];
    if (vw == 0.0f && vh == 0.0f) vx = 0, vy = 0, vw = (float)p.w, vh = (float)p.h;
    d.vp = {vx, vy + vh, vw, -vh, std::clamp(s.depth_near, 0.0f, 1.0f), std::clamp(s.depth_far, 0.0f, 1.0f)};
    d.sc = {{0, 0}, {p.w, p.h}};
    if (s.scissor[2] > 0 && s.scissor[3] > 0) {
        const int32_t x0 = std::max(0, s.scissor[0]), y0 = std::max(0, s.scissor[1]);
        const int32_t x1 = std::min<int32_t>(s.scissor[0] + s.scissor[2], (int32_t)p.w);
        const int32_t y1 = std::min<int32_t>(s.scissor[1] + s.scissor[3], (int32_t)p.h);
        d.sc = {{std::min<int32_t>(x0, (int32_t)p.w), std::min<int32_t>(y0, (int32_t)p.h)}, {(uint32_t)std::max(0, x1 - x0), (uint32_t)std::max(0, y1 - y0)}};
    }
    std::memcpy(d.blend, s.blend_color, sizeof d.blend);
    for (int f = 0; f < 2; f++) d.sref[f] = s.stencil_ref[f] & 0xFF, d.scmp[f] = s.stencil_value_mask[f] & 0xFF, d.swm[f] = s.stencil_mask[f] & 0xFF;
    d.bias[0] = s.poly_offset[1], d.bias[1] = s.poly_offset[2], d.bias[2] = s.poly_offset[0];
    d.bias_en = s.poly.offset_en != 0;
    d.cull = s.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : s.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT
           : s.poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    static const bool nocull = dbg("nocull");   // (HOS_DBG=nocull, a diagnosis: no face culled)
    if (nocull) d.cull = VK_CULL_MODE_NONE;
    d.front = s.poly.front == NVN_FRONT_FACE_CCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;   // (the flipped viewport keeps the winding)
    d.topo = topo;
    d.restart = s.restart && topo != VK_PRIMITIVE_TOPOLOGY_POINT_LIST && topo != VK_PRIMITIVE_TOPOLOGY_LINE_LIST &&
                topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && topo != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
                topo != VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY && topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    const VkImageAspectFlags da = p.depth.aspect;   // (M3a: a job's secondary has formats, no images)
    d.dt = (da & VK_IMAGE_ASPECT_DEPTH_BIT) && s.ds.dt;
    d.dw = (da & VK_IMAGE_ASPECT_DEPTH_BIT) && s.ds.dw;
    d.dop = compare_op(s.ds.df);
    d.st = (da & VK_IMAGE_ASPECT_STENCIL_BIT) && s.ds.st;
    d.sops[0][0] = stencil_op(s.ds.fail_f), d.sops[0][1] = stencil_op(s.ds.zpass_f), d.sops[0][2] = stencil_op(s.ds.zfail_f);
    d.sops[1][0] = stencil_op(s.ds.fail_b), d.sops[1][1] = stencil_op(s.ds.zpass_b), d.sops[1][2] = stencil_op(s.ds.zfail_b);
    d.scmpop[0] = compare_op(s.ds.sf_f), d.scmpop[1] = compare_op(s.ds.sf_b);
    d.discard = s.raster_discard;
    d.poly = s.poly.mode == 0 ? VK_POLYGON_MODE_POINT : s.poly.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    d.clamp = s.depth_clamp, d.a2c = s.ms.a2c != 0;
    d.samples = p.samples;
    d.n = (uint32_t)p.n;
    for (int i = 0; i < p.n; i++) {
        const nvn_blend_st& b = s.blend[i];
        const bool has = p.color[i].format != VK_FORMAT_UNDEFINED;
        d.en[i] = has && (s.color.blend_en >> i & 1) ? VK_TRUE : VK_FALSE;
        d.eq[i] = {blend_factor(b.src_rgb), blend_factor(b.dst_rgb), blend_op(b.eq_rgb), blend_factor(b.src_a), blend_factor(b.dst_a), blend_op(b.eq_a)};
        // (a target the fragment stage leaves unwritten: no write (Maxwell keeps its texels); and only the components it writes)
        const uint32_t written = prog.frag_components[i];
        d.wm[i] = has && (prog.frag_stores >> i & 1) ? (s.chmask.mask >> (4 * i)) & 0xF & (written ? written : 0xFu) : 0;
    }
    const bool all = !x.dyn_valid;
    const Dyn& o = x.dyn;
#define CH(f) (all || std::memcmp(&d.f, &o.f, sizeof d.f))
    if (all) {   // (state no NVN call sets: once a command buffer)
        vkCmdSetLineWidth(cb, 1.0f);
        vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
        if (caps.alpha_to_one) vkCmdSetAlphaToOneEnableEXT(cb, VK_FALSE);
        if (caps.logic_op) vkCmdSetLogicOpEnableEXT(cb, VK_FALSE);
        if (caps.provoking_vertex) vkCmdSetProvokingVertexModeEXT(cb, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
    }
    if (CH(vp)) vkCmdSetViewportWithCount(cb, 1, &d.vp);
    if (CH(sc)) vkCmdSetScissorWithCount(cb, 1, &d.sc);
    if (CH(blend)) vkCmdSetBlendConstants(cb, d.blend);
    if (CH(sref)) vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, d.sref[0]), vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, d.sref[1]);
    if (CH(scmp)) vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, d.scmp[0]), vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, d.scmp[1]);
    if (CH(swm)) vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, d.swm[0]), vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, d.swm[1]);
    if (CH(bias)) vkCmdSetDepthBias(cb, d.bias[0], d.bias[1], d.bias[2]);
    if (CH(bias_en)) vkCmdSetDepthBiasEnable(cb, d.bias_en);
    if (CH(cull)) vkCmdSetCullMode(cb, d.cull);
    if (CH(front)) vkCmdSetFrontFace(cb, d.front);
    if (CH(topo)) vkCmdSetPrimitiveTopology(cb, d.topo);
    if (CH(restart)) vkCmdSetPrimitiveRestartEnable(cb, d.restart);
    if (CH(dt)) vkCmdSetDepthTestEnable(cb, d.dt);
    if (CH(dw)) vkCmdSetDepthWriteEnable(cb, d.dw);
    if (CH(dop)) vkCmdSetDepthCompareOp(cb, d.dop);
    if (CH(st)) vkCmdSetStencilTestEnable(cb, d.st);
    if (CH(sops) || CH(scmpop)) {
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, d.sops[0][0], d.sops[0][1], d.sops[0][2], d.scmpop[0]);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, d.sops[1][0], d.sops[1][1], d.sops[1][2], d.scmpop[1]);
    }
    if (CH(discard)) vkCmdSetRasterizerDiscardEnable(cb, d.discard);
    if (CH(poly)) vkCmdSetPolygonModeEXT(cb, d.poly);
    if (CH(clamp)) vkCmdSetDepthClampEnableEXT(cb, d.clamp);
    if (CH(a2c)) vkCmdSetAlphaToCoverageEnableEXT(cb, d.a2c);
    if (CH(samples)) {
        const VkSampleMask mask[2] = {~0u, ~0u};
        vkCmdSetRasterizationSamplesEXT(cb, d.samples);
        vkCmdSetSampleMaskEXT(cb, d.samples, mask);
    }
    if (d.n) {
        const bool same_n = !all && o.n == d.n;
        if (!same_n || std::memcmp(d.en, o.en, sizeof(VkBool32) * d.n)) vkCmdSetColorBlendEnableEXT(cb, 0, d.n, d.en);
        if (!same_n || std::memcmp(d.eq, o.eq, sizeof(VkColorBlendEquationEXT) * d.n)) vkCmdSetColorBlendEquationEXT(cb, 0, d.n, d.eq);
        if (!same_n || std::memcmp(d.wm, o.wm, sizeof(VkColorComponentFlags) * d.n)) vkCmdSetColorWriteMaskEXT(cb, 0, d.n, d.wm);
    }
#undef CH
    x.dyn = d;
    x.dyn_valid = true;
}

/// The vertex input the program reads (so_state's), set when it changed; the streams it binds in used
void set_vertex_input(ListDraw& x, VkCommandBuffer cb, const DState& s, const DrawProg& prog, bool used[16]) {
    VkVertexInputBindingDescription2EXT vb[16];
    VkVertexInputAttributeDescription2EXT va[16];
    uint32_t nb = 0, na = 0;
    std::memset(vb, 0, sizeof vb), std::memset(va, 0, sizeof va);
    bool zero = false;
    for (uint32_t i = 0; i < 16; i++) {
        if (!(prog.vs_loads >> i & 1)) continue;
        const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&s.vattr[i]);
        const VkFormat f = (int)i < s.nvattr ? vertex_format(a.fmt) : VK_FORMAT_UNDEFINED;
        VkVertexInputAttributeDescription2EXT& d = va[na++];
        d.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
        d.location = i;
        if (f == VK_FORMAT_UNDEFINED || a.stream >= 15) {   // (an unset attribute reads (0, 0, 0, 1))
            d.binding = 15, d.format = VK_FORMAT_R32_SFLOAT, d.offset = 0;
            zero = true;
            continue;
        }
        d.binding = a.stream, d.format = supported_vertex_format(f), d.offset = a.offset & 0x3FFF;
        used[a.stream] = true;
    }
    for (uint32_t b = 0; b < 15; b++)
        if (used[b]) {
            VkVertexInputBindingDescription2EXT& d = vb[nb++];
            d.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
            d.binding = b, d.stride = s.vstream[b].stride;
            d.inputRate = s.vstream[b].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            d.divisor = s.vstream[b].divisor > 1 && caps.vertex_divisor ? s.vstream[b].divisor : 1;
        }
    if (zero) {
        VkVertexInputBindingDescription2EXT& d = vb[nb++];
        d.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
        d.binding = 15, d.stride = 0, d.inputRate = VK_VERTEX_INPUT_RATE_VERTEX, d.divisor = 1;
        used[15] = true;
    }
    uint64_t sig = 0xcbf29ce484222325ull ^ (nb << 8 | na);
    for (uint32_t i = 0; i < nb; i++) sig = mix(sig, (uint64_t)vb[i].binding << 48 ^ (uint64_t)vb[i].stride << 16 ^ vb[i].inputRate ^ (uint64_t)vb[i].divisor << 32);
    for (uint32_t i = 0; i < na; i++) sig = mix(sig, (uint64_t)va[i].location << 56 ^ (uint64_t)va[i].binding << 48 ^ (uint64_t)va[i].format << 16 ^ va[i].offset);
    if (!x.vi_valid || x.vi_sig != sig) {
        vkCmdSetVertexInputEXT(cb, nb, vb, na, va);
        x.vi_sig = sig, x.vi_valid = true;
    }
}

/// A graphics stage's driver bank (c0 / c2) in the list's arena, made again when the stage's binds changed; its deferred words
/// noted for the submit
bool bank_copy(List* l, ListDraw& x, int stage, int bank, uint32_t bytes, VkDeviceAddress& out) {
    BankCopy& c = x.bank[stage][bank == 2];
    if (c.addr && c.gen == x.s.bank_gen[stage] && c.bytes >= bytes) return out = c.addr, true;
    uint8_t* map;
    VkDeviceAddress a;
    if (!arena_alloc(l->slot, bytes, map, a)) return false;
    bank_fill(x.s, bank, stage, map, bytes);
    if (bank == 2 && x.s.deferred[stage]) {   // (the handles the game's arrays hold at its submit: copied then)
        auto note = [&](uint32_t off, uint64_t src) {
            if (!src || off + 4 > bytes) return;
            uint64_t h = 0;
            if (deferred_check) host->gpu_read(src, &h, 8);
            x.fixups.push_back({map + off, src, (uint32_t)h});
        };
        for (uint32_t i = 0; i < 128; i++) {
            if (i < 32) note(0x20 + 8 * i, x.s.tex_src[stage][i]);
            note(0x168 + 8 * i, x.s.tex_src[stage][i]);
            note(0x568 + 8 * i, x.s.samp_src[stage][i]);
            if (i < 9) note(0x120 + 8 * i, x.s.img_src[stage][i]);
        }
    }
    if (bank == 0)   // (the storage buffers the shader reaches by address: bound in layer 0 ahead)
        for (int i = 0; i < 16; i++) {
            const nvn_p_bind_buf& b = x.s.ssbo[stage][i];
            VkDeviceAddress unused;
            if (b.addr && !layer0_address(b.addr, std::max<uint64_t>(b.size, 16), unused)) st_ssbo_unbound.fetch_add(1, std::memory_order_relaxed);
        }
    c.gen = x.s.bank_gen[stage], c.bytes = bytes, c.addr = a;
    bump(me().banks);
    return out = a, true;
}

/// (M3b) guest [addr, addr + size) in its layer-0 window, through a span kept while it is the same
bool span_of(ListDraw::Span& c, uint64_t addr, uint64_t size, VkBuffer& b, VkDeviceSize& o) {
    if (c.b && c.addr == addr && c.size >= size) return b = c.b, o = c.o, true;
    if (!layer0_span(addr, size, b, o)) return false;
    c.addr = addr, c.size = size, c.b = b, c.o = o;
    return true;
}

/// (M3b) the list's shadow of a buffer its updates write, brought up to n bytes and to every update so far
const uint8_t* shadow_of(ListDraw& x, uint64_t addr, uint64_t size, uint64_t n) {
    ListDraw::Shadow* sh = nullptr;
    for (ListDraw::Shadow& c : x.shadows)
        if (c.addr == addr && c.size == size) sh = &c;
    if (!sh) {
        sh = &x.shadows.emplace_back();
        sh->addr = addr, sh->size = size;
    }
    if (sh->valid < n) {   // (the part not read yet: guest memory as it is now, then the updates over it)
        sh->bytes.resize(n);
        if (!host->gpu_read(addr + sh->valid, sh->bytes.data() + sh->valid, n - sh->valid))
            std::memset(sh->bytes.data() + sh->valid, 0, n - sh->valid);
        bump(me().ub_reads);
        for (size_t i = 0; i < sh->applied; i++) {
            const ListDraw::Update& up = x.updates[i];
            const uint64_t lo = std::max(up.dst, addr + sh->valid), hi = std::min(up.dst + up.size, addr + n);
            if (lo < hi) std::memcpy(sh->bytes.data() + (lo - addr), x.update_bytes.data() + up.at + (lo - up.dst), hi - lo);
        }
        sh->valid = n;
    }
    for (; sh->applied < x.updates.size(); sh->applied++) {
        const ListDraw::Update& up = x.updates[sh->applied];
        const uint64_t lo = std::max(up.dst, addr), hi = std::min(up.dst + up.size, addr + sh->valid);
        if (lo < hi) std::memcpy(sh->bytes.data() + (lo - addr), x.update_bytes.data() + up.at + (lo - up.dst), hi - lo);
    }
    return sh->bytes.data();
}

/// The address a uniform buffer of the variant reads (0 and a reason: none)
Reason ubo_address(List* l, ListDraw& x, const DrawProg& p, const DrawProg::Ubo& u, VkDeviceAddress& a) {
    if (u.stage >= kBankStages) return R_STEP_A;
    if (u.index == 0 || u.index == 2) return bank_copy(l, x, u.stage, u.index, u.bytes, a) ? R_DRAWN : R_ARENA;
    if (u.index == 1) return a = p.c1[u.stage] ? p.c1[u.stage] : zero_address, R_DRAWN;
    if (u.index >= 3 + 16) return a = zero_address, R_DRAWN;
    const nvn_p_bind_buf& b = x.s.ubo[u.stage][u.index - 3];
    if (!b.addr) return a = zero_address, R_DRAWN;
    const uint64_t size = std::max<uint64_t>(b.size, 16);
    // (the contract's open item: a program that may read past the bound range reads the guest memory after it through the
    // address, where Maxwell reads zeros; counted)
    if (u.bytes > size) bump(me().past_range);
    bool updated = false;
    for (const ListDraw::Update& up : x.updates) updated |= up.dst < b.addr + size && up.dst + up.size > b.addr;
    if (!updated) {
        ListDraw::UboAddr& c = x.ubo_addr[u.stage][u.index - 3];
        if (c.dev && c.addr == b.addr && c.size == size) return a = c.dev, R_DRAWN;
        if (!layer0_address(b.addr, size, a)) return R_UBO_MEM;
        c.addr = b.addr, c.size = size, c.dev = a;
        return R_DRAWN;
    }
    // (a copy with the list's updates so far: the bytes the program reads of it (at least the 16 of a row), zeros past the bound
    // range as Maxwell reads there)
    const uint64_t want = std::max<uint64_t>(16, ((uint64_t)(u.bytes ? u.bytes : size) + 15) & ~15ull);
    // (a dynamically indexed one (Eden's used size 64 KB) gets its bound range only: zeros to 64 KB a copy are not worth it)
    const uint64_t n = std::min<uint64_t>(size, want), total = want >= (64u << 10) ? n : want;
    for (const ListDraw::UbCopy& c : x.ub_copies)   // (made for these updates already)
        if (c.addr == b.addr && c.size >= total && c.nupdates == x.updates.size()) return a = c.copy, R_DRAWN;
    if (n > (64u << 10)) return R_UBO_MEM;
    uint8_t* map;
    if (!arena_alloc(l->slot, (uint32_t)std::max(n, total), map, a)) return R_ARENA;
    std::memcpy(map, shadow_of(x, b.addr, size, n), n);
    if (total > n) std::memset(map + n, 0, total - n);
    x.ub_copies.push_back({b.addr, std::max(n, total), x.updates.size(), a});
    bump(me().ub_copies);
    bump(me().ub_bytes, (int64_t)n);
    return R_DRAWN;
}

/// The address table and the pushed addresses (push data from 120); false: a reason
Reason bind_data(List* l, ListDraw& x, const DrawProg& p, uint8_t push[256]) {
    const bool same = x.table_prog == &p && x.table_ubo_gen == x.s.ubo_gen && x.table_bank_gen[0] == x.s.bank_gen[0] &&
                      x.table_bank_gen[1] == x.s.bank_gen[1];
    if (!same) {
        x.table_entries.resize(p.ubos.size());
        for (size_t i = 0; i < p.ubos.size(); i++) {
            const Reason r = ubo_address(l, x, p, p.ubos[i], x.table_entries[i]);
            if (r != R_DRAWN) return x.table_prog = nullptr, r;
        }
        uint8_t* map;
        VkDeviceAddress a;
        if (!arena_alloc(l->slot, std::max<uint32_t>(8, 8 * (uint32_t)p.ubos.size()), map, a)) return x.table_prog = nullptr, R_ARENA;
        std::memcpy(map, x.table_entries.data(), 8 * p.ubos.size());
        x.table_addr = a, x.table_prog = &p, x.table_ubo_gen = x.s.ubo_gen;
        x.table_bank_gen[0] = x.s.bank_gen[0], x.table_bank_gen[1] = x.s.bank_gen[1];
        bump(me().tables);
    }
    std::memcpy(push + kTablePush, &x.table_addr, 8);
    for (uint32_t i = 0; i < p.direct_ubos && i < x.table_entries.size(); i++) std::memcpy(push + 192 + 8 * i, &x.table_entries[i], 8);
    for (const DrawProg::Pushed& q : p.pushed) {
        VkDeviceAddress a = 0;
        for (size_t i = 0; i < p.ubos.size(); i++)
            if (p.ubos[i].stage == q.stage && p.ubos[i].index == q.index) a = x.table_entries[i];
        if (!a) {   // (a buffer only a texture reads: not in the table)
            const DrawProg::Ubo u{q.stage, q.index, 0xA00};
            const Reason r = ubo_address(l, x, p, u, a);
            if (r != R_DRAWN) return r;
        }
        std::memcpy(push + q.push_off, &a, 8);
    }
    return R_DRAWN;
}

/// HOS_DBG=drawlog:<present>: the recording lists' draws of the frame after that present, each with what it was drawn with
bool drawlog_on() {
    static const int64_t at = [] {
        const char* v = dbg_value("drawlog");
        return v ? atoll(v) : -1;
    }();
    return at >= 0 && (int64_t)present_count() == at;
}
void drawlog(const List* l, const char* what, const DrawProg* p, const Dyn* d, uint32_t count) {
    if (!drawlog_on()) return;
    static std::atomic<int> n{0};
    if (n++ > 400) return;
    char b[1400];
    int k = std::snprintf(b, sizeof b, "direct: drawlog lr %#llx rt %llu: %s", (unsigned long long)l->info.lr, (unsigned long long)l->pass.set_rt_at, what);
    if (p)
        k += std::snprintf(b + k, sizeof b - k, " prog %016llx vs_loads %#x stores %#x comp %x push %u ubos %zu pushed %zu ra %d", (unsigned long long)p->key,
                           p->vs_loads, p->frag_stores, p->frag_components[0], p->push_bytes, p->ubos.size(), p->pushed.size(), p->uses_render_area);
    if (p) {
        for (const auto& u : p->ubos)
            k += std::snprintf(b + k, sizeof b - k, " u%u.%u%s%llx", u.stage, u.index, u.index >= 3 ? "=" : "",
                               u.index >= 3 && u.stage < kBankStages ? (unsigned long long)l->dctx->s.ubo[u.stage][u.index - 3].addr : 0ull);
        for (const auto& q : p->pushed) k += std::snprintf(b + k, sizeof b - k, " p%u.%u@%u", q.stage, q.index, q.push_off);
        const DState& s = l->dctx->s;
        for (int i = 0; i < 16 && k < 1000; i++)
            if (p->vs_loads >> i & 1) {
                const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&s.vattr[i]);
                k += std::snprintf(b + k, sizeof b - k, " a%d fmt %#x s%u off %u stride %u buf %#llx+%llu", i, a.fmt, a.stream, a.offset,
                                   a.stream < 16 ? s.vstream[a.stream].stride : 0, a.stream < 16 ? (unsigned long long)s.vbuf[a.stream].address : 0ull,
                                   a.stream < 16 ? (unsigned long long)s.vbuf[a.stream].size : 0ull);
            }
        k += std::snprintf(b + k, sizeof b - k, " discard %d poly %u front %u", (int)s.raster_discard, s.poly.mode, s.poly.front);
        for (const auto& t : p->tex_words) {
            const uint32_t h = t.bank < 3 ? bank_word(s, t.bank, t.stage, t.offset) : 0;
            char e[320];
            heap_describe(h & 0xFFFFF, e, sizeof e);
            k += std::snprintf(b + k, sizeof b - k, " tex c%u+%#x=%#x(%s)", t.bank, t.offset, h, e);
        }
    }
    if (d)
        k += std::snprintf(b + k, sizeof b - k, " n %u vp %.0f,%.0f %.0fx%.0f z %.2f-%.2f sc %d,%d %ux%u wm0 %x en0 %u eq0 %d/%d/%d cull %u dt %u dw %u st %u count %u samples %u",
                           d->n, d->vp.x, d->vp.y, d->vp.width, d->vp.height, d->vp.minDepth, d->vp.maxDepth, d->sc.offset.x, d->sc.offset.y,
                           d->sc.extent.width, d->sc.extent.height, d->n ? d->wm[0] : 0, d->n ? d->en[0] : 0, d->n ? (int)d->eq[0].srcColorBlendFactor : 0,
                           d->n ? (int)d->eq[0].dstColorBlendFactor : 0, d->n ? (int)d->eq[0].colorBlendOp : 0, d->cull, d->dt, d->dw, d->st, count, (unsigned)d->samples);
    logf("%s", b);
}

void count(List* l, Reason r) {
    if (r == R_DRAWN) l->n_drawn++;
    else if (r == R_MESH) l->n_mesh++;
    else if (r == R_STEP_A) l->n_step_a++;
    if (r != R_DRAWN && l->info.kind != HSD_LIST_JOB && l->info.kind != HSD_LIST_LOAD) drawlog(l, kReasons[r], nullptr, nullptr, 0);
    bump(me().by_kind[l->info.kind][r]);
    if (l->info.kind == HSD_LIST_LAYER || l->info.kind == HSD_LIST_GATE) count_site(l->info.lr, r);
}

}  // namespace

bool draw_log_frame() { return drawlog_on(); }

void draw_state(List* l, uint32_t op, const void* data, uint32_t size) {
    switch (op) {   // (direct_state.cpp's ops: a list's state is made at its first one)
    case NOP_VIEWPORTS: case NOP_SCISSOR: case NOP_DEPTH_RANGE: case NOP_BIND_PROGRAM: case NOP_BIND_STATE: case NOP_BIND_VATTR:
    case NOP_BIND_VSTREAM: case NOP_BIND_VBUF: case NOP_BIND_BUFS: case NOP_BIND_UBO: case NOP_BIND_SSBO: case NOP_BIND_TEX:
    case NOP_BIND_SAMP: case NOP_BIND_IMG: case NOP_STENCIL: case NOP_POLY_OFFSET: case NOP_BLEND_COLOR: case NOP_PRIMITIVE_RESTART:
    case NOP_DEPTH_CLAMP: case NOP_RASTER_DISCARD: case NOP_RENDER_ENABLE: case NOP_RENDER_COND:
        state_emit(ctx(l).s, op, data, size);
        return;
    case NOP_UPDATE_UB: {   // (the bytes kept for the list's later draws: plan, Data a draw reads)
        nvn_p_update_ub u;
        if (size < sizeof u) return;
        std::memcpy(&u, data, sizeof u);
        const uint64_t n = std::min<uint64_t>(u.size, size - sizeof u);
        ListDraw& x = ctx(l);
        x.updates.push_back({u.dst, (uint32_t)n, (uint32_t)x.update_bytes.size()});
        x.update_bytes.insert(x.update_bytes.end(), (const uint8_t*)data + sizeof u, (const uint8_t*)data + sizeof u + n);
        x.s.ubo_gen++;
        st_ub_updates.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    default: return;
    }
}

void draw_list_free(List* l) {
    delete l->dctx;
    l->dctx = nullptr;
}

void draw_emit(List* l, uint32_t op, const void* data, uint32_t size, bool recording) {
    const int64_t t0 = host->now_ns();
    struct Timer {
        int64_t t0;
        ~Timer() { bump(me().ns, host->now_ns() - t0); }
    } timer{t0};
    if (op == NOP_DRAW_MESH) return count(l, R_MESH);
    if (op == NOP_DRAW_INDIRECT) return count(l, R_INDIRECT);
    if (!heap_on()) return count(l, R_NOT_GRAPHICS);
    ListDraw& x = ctx(l);
    const DState& s = x.s;
    nvn_p_draw dr{};
    nvn_p_draw_idx di{};
    if (op == NOP_DRAW) std::memcpy(&dr, data, std::min<size_t>(sizeof dr, size));
    else std::memcpy(&di, data, std::min<size_t>(sizeof di, size));
    const int prim = op == NOP_DRAW ? dr.prim : di.prim;
    const PassRec& p = l->pass;
    const bool records = l->rec;   // (M3a: jobs with their pass's formats too)
    if (records && !p.noted) return count(l, R_NO_TARGETS);
    const bool gated = s.conditional;   // (M3a: a render condition is not evaluated: the draw is drawn, counted; M5)
    if (!s.render_enabled) return count(l, R_DISABLED);
    VkPrimitiveTopology topo;
    if (prim == NVN_DRAW_PRIMITIVE_QUADS) return count(l, R_QUADS);
    if (!topology(prim, topo)) return count(l, R_TOPOLOGY);
    uint8_t ct[8]{};   // (the targets' colour types; a list with none of its own (a job): float, as every logged pass)
    if (records)
        for (int i = 0; i < p.n; i++) ct[i] = color_type(p.color[i].format);
    ShaderMiss why = SM_NONE;
    const DrawProg* prog;
    uint64_t ctw;
    std::memcpy(&ctw, ct, 8);
    if (x.pick && x.pick_prog_gen == s.prog_gen && x.pick_vtx_gen == s.vtx_gen && x.pick_bank_gen[0] == s.bank_gen[0] &&
        x.pick_bank_gen[1] == s.bank_gen[1] && x.pick_ct == ctw) {   // (nothing the pick reads changed since the last draw)
        prog = x.pick;
    } else {
        prog = shader_pick(s, ct, why);
        x.pick = prog, x.pick_prog_gen = s.prog_gen, x.pick_vtx_gen = s.vtx_gen, x.pick_ct = ctw;
        x.pick_bank_gen[0] = s.bank_gen[0], x.pick_bank_gen[1] = s.bank_gen[1];
    }
    if (!prog) {
        static const Reason map[SM_COUNT] = {R_DRAWN, R_NO_PROGRAM, R_NOT_PARSED, R_NOT_GRAPHICS, R_TRANSLATING, R_FAILED, R_STEP_A};
        return count(l, map[why]);
    }
    if (prog->skip) {
        count_step_a(prog->skip, l->info.kind);
        return count(l, R_STEP_A);
    }
    if (op == NOP_DRAW_IDX && di.itype == NVN_INDEX_TYPE_UNSIGNED_BYTE && !caps.index_uint8) return count(l, R_INDEX_U8);
    if (!recording || !l->cur) {   // (resolved this far; no command buffer to record it in at this stage)
        return count(l, l->info.kind == HSD_LIST_JOB ? R_RESOLVED_JOB : l->info.kind == HSD_LIST_LOAD ? R_RESOLVED_LOAD : R_RESOLVED_OTHER);
    }
    VkCommandBuffer cb = l->cur;
    cb_fresh(x, cb);
    // (the buffers first: a draw that cannot have them is skipped before anything is recorded)
    bool used[16]{};
    VkBuffer vb[16];
    VkDeviceSize vo[16];
    for (int b = 0; b < 16; b++) vb[b] = zero_buffer, vo[b] = 0;
    {
        for (uint32_t i = 0; i < 16; i++) {
            if (!(prog->vs_loads >> i & 1) || (int)i >= s.nvattr) continue;
            const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&s.vattr[i]);
            if (a.stream < 15 && vertex_format(a.fmt) != VK_FORMAT_UNDEFINED) used[a.stream] = true;
        }
        for (int b = 0; b < 15; b++) {
            if (!used[b] || !s.vbuf[b].address) continue;
            if (!span_of(x.vb_span[b], s.vbuf[b].address, std::max<uint64_t>(s.vbuf[b].size, 1), vb[b], vo[b])) return count(l, R_VERTEX_MEM);
        }
    }
    VkBuffer ib = VK_NULL_HANDLE;
    VkDeviceSize io = 0;
    VkIndexType it = VK_INDEX_TYPE_UINT16;
    if (op == NOP_DRAW_IDX) {
        const uint32_t unit = di.itype == 0 ? 1 : di.itype == 1 ? 2 : 4;
        if (!span_of(x.ib_span, di.ibuf, (uint64_t)std::max(di.count, 1) * unit, ib, io)) return count(l, R_INDEX_MEM);
        it = di.itype == 0 ? VK_INDEX_TYPE_UINT8 : di.itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    }
    uint8_t push[256];
    std::memset(push, 0, prog->push_bytes);
    if (const Reason r = bind_data(l, x, *prog, push); r != R_DRAWN) return count(l, r);
    {   // (Eden's block: the render area, the down factor, the window table)
        const float one = 1.0f;
        std::memcpy(push + 24, &one, 4);
        std::memcpy(push + 88, &window_table_address, 8);
        if (prog->uses_render_area) {
            const float ra[4] = {(float)p.w, (float)p.h, 0.0f, 0.0f};
            std::memcpy(push, ra, sizeof ra);
        }
    }
    // (recorded)
    if (!x.heap) heap_bind(cb), x.heap = true;
    if (!x.shaders || x.vs != prog->vs || x.fs != prog->fs) {
        // (every graphics stage the device has: the ones the program has not bound to none)
        VkShaderStageFlagBits st[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        VkShaderEXT sh[5] = {prog->vs, prog->fs};
        uint32_t n = 2;
        if (dev.features.tessellationShader)
            st[n] = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, sh[n++] = VK_NULL_HANDLE,
            st[n] = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, sh[n++] = VK_NULL_HANDLE;
        if (dev.features.geometryShader) st[n] = VK_SHADER_STAGE_GEOMETRY_BIT, sh[n++] = VK_NULL_HANDLE;
        vkCmdBindShadersEXT(cb, n, st, sh);
        x.vs = prog->vs, x.fs = prog->fs, x.shaders = true;
    }
    set_dynamic(x, cb, s, p, topo, *prog);
    bool vi_used[16]{};
    set_vertex_input(x, cb, s, *prog, vi_used);
    {
        uint32_t first = 16, last = 0, want = 0;
        for (uint32_t b = 0; b < 16; b++)
            if (vi_used[b]) first = std::min(first, b), last = b, want |= 1u << b;
        if (first < 16 && (!x.vb_valid || (want & ~x.vb_bound) || std::memcmp(x.vb + first, vb + first, sizeof(VkBuffer) * (last - first + 1)) ||
                           std::memcmp(x.vo + first, vo + first, sizeof(VkDeviceSize) * (last - first + 1)))) {
            vkCmdBindVertexBuffers2(cb, first, last - first + 1, vb + first, vo + first, nullptr, nullptr);
            std::memcpy(x.vb + first, vb + first, sizeof(VkBuffer) * (last - first + 1));
            std::memcpy(x.vo + first, vo + first, sizeof(VkDeviceSize) * (last - first + 1));
            for (uint32_t b = first; b <= last; b++) x.vb_bound |= 1u << b;
            x.vb_valid = true;
        }
    }
    if (op == NOP_DRAW_IDX && (!x.ib_valid || x.ib != ib || x.io != io || x.it != it)) {
        vkCmdBindIndexBuffer(cb, ib, io, it);
        x.ib = ib, x.io = io, x.it = it, x.ib_valid = true;
    }
    if (!x.push_valid || x.push_n != prog->push_bytes || std::memcmp(x.push, push, prog->push_bytes)) {
        heap_push(cb, push, prog->push_bytes);
        std::memcpy(x.push, push, prog->push_bytes);
        x.push_n = prog->push_bytes, x.push_valid = true;
    }
    drawlog(l, op == NOP_DRAW_IDX ? "drawn indexed" : "drawn", prog, &x.dyn, op == NOP_DRAW_IDX ? (uint32_t)di.count : (uint32_t)dr.count);
    if (op == NOP_DRAW_IDX)
        vkCmdDrawIndexed(cb, (uint32_t)di.count, (uint32_t)std::max(1, di.instances), 0, di.base_vertex, (uint32_t)di.base_instance);
    else
        vkCmdDraw(cb, (uint32_t)dr.count, (uint32_t)std::max(1, dr.instances), (uint32_t)dr.first, (uint32_t)dr.base_instance);
    count(l, R_DRAWN);
    if (gated) st_gated[l->info.kind].fetch_add(1, std::memory_order_relaxed);
}

void draw_invalidate(List* l) {
    if (l->dctx) l->dctx->cb = VK_NULL_HANDLE;
}

void draw_list_submitted(const List* l) {
    std::vector<const List*> work{l};
    while (!work.empty()) {
        const List* x = work.back();
        work.pop_back();
        for (const FrameOp& o : x->ops)
            if (o.op == NOP_CALL && o.callee) work.push_back(o.callee);
        if (!x->dctx || x->dctx->fixups.empty()) continue;
        for (const Fixup& f : x->dctx->fixups) {
            uint64_t h = 0;
            host->gpu_read(f.src, &h, 8);
            const uint32_t lo = (uint32_t)h;
            std::memcpy(f.dst, &lo, 4);
            if (deferred_check) {
                st_def_words.fetch_add(1, std::memory_order_relaxed);
                if (lo != f.bound) st_def_bind.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard lk(deferred_mu);
                auto [it, made] = deferred_last.try_emplace(f.src, lo);
                if (!made && it->second != lo) st_def_frame.fetch_add(1, std::memory_order_relaxed), it->second = lo;
            }
        }
        st_fixups.fetch_add((int64_t)x->dctx->fixups.size(), std::memory_order_relaxed);
        st_fixup_lists.fetch_add(1, std::memory_order_relaxed);
    }
}

void draw_window_made(uint32_t k, uint64_t a) {
    if (window_table && k < kWindows) __atomic_store_n(&window_table[k], a, __ATOMIC_RELEASE);
}

/// (direct_heap.cpp draw_device_created) the window table: an entry a layer-0 window, the null window past them; the zero buffer
void draw_window_table_init() {
    VkBuffer b;
    VkDeviceMemory m;
    uint8_t* map;
    if (!mapped_buffer(kTableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, b, m, map, window_table_address)) return logf("direct: no window table");
    {   // (the null window: a 4 GB sparse buffer with nothing bound, which reads zeros)
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
        bi.size = (4ull << 30) - (64ull << 10);
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VkBuffer nb;
        if (vkCreateBuffer(dev.device, &bi, nullptr, &nb) == VK_SUCCESS) {
            VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            da.buffer = nb;
            null_window = vkGetBufferDeviceAddress(dev.device, &da);
            st_objects.fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::memset(map, 0, kTableBytes);
    uint64_t* t = (uint64_t*)map;
    for (uint32_t k = 0; k <= kWindows; k++) t[k] = null_window;
    window_table = t;
    {   // (attributes no stream supplies, uniform buffers not bound: 64 KB of zeros)
        VkDeviceMemory zm;
        uint8_t* zmap;
        if (mapped_buffer(64 << 10, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, zero_buffer, zm, zmap, zero_address))
            std::memset(zmap, 0, 64 << 10);
    }
}

void draw_log(double frames) {
    static char out[8192];
    const size_t cap = sizeof out;
    int k = std::snprintf(out, cap, "draws a frame (%.3f ms in the draw path, all threads; %.1f bank copies, %.1f address tables, %.1f "
                          "deferred words copied at submits in %.1f lists, %lld arena chunks, %lld storage buffers not bound)",
                          take(&TStats::ns, T_NS) / 1e6 / frames, take(&TStats::banks, T_BANKS) / frames, take(&TStats::tables, T_TABLES) / frames,
                          st_fixups.exchange(0) / frames, st_fixup_lists.exchange(0) / frames, (long long)st_chunks.load(),
                          (long long)st_ssbo_unbound.load());
    if (deferred_check)
        logf("direct: deferredcheck: %.1f deferred words copied a frame, %.2f changed between bind and submit, %.2f changed since the "
             "last submit that read the same word", st_def_words.exchange(0) / frames, st_def_bind.exchange(0) / frames, st_def_frame.exchange(0) / frames);
    logf("direct: %s; inline uniform updates %.1f a frame, buffer copies with them %.1f (%.1f KB), guest reads for their rest %.1f",
         out, st_ub_updates.exchange(0) / frames, take(&TStats::ub_copies, T_UB_COPIES) / frames,
         take(&TStats::ub_bytes, T_UB_BYTES) / 1024.0 / frames, take(&TStats::ub_reads, T_UB_READS) / frames);
    logf("direct: uniform buffers a program may read past their bound range: %.1f a frame (of the address tables made)",
         take(&TStats::past_range, T_PAST) / frames);    static const char* const kinds[HSD_LIST_KINDS] = {"unclassified", "load", "job", "layer", "gate", "compute", "head"};
    for (int kind = 0; kind < HSD_LIST_KINDS; kind++) {
        k = std::snprintf(out, cap, "draws a frame in %s lists:", kinds[kind]);
        bool any = false;
        for (int r = 0; r < R_COUNT; r++) {
            const int64_t n = take_kind(kind, r);
            if (!n) continue;
            k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "%s %s %.2f", any ? "," : "", kReasons[r], n / frames);
            any = true;
        }
        if (const int64_t g = st_gated[kind].exchange(0))
            k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; of the drawn under a render condition (not evaluated) %.2f", g / frames), any = true;
        if (any) logf("direct: %s", out);
    }
    k = std::snprintf(out, cap, "draws a frame that step A cannot draw, by the variant's reason:");
    for (StepA& s : step_a) {
        const char* why = s.why.load();
        if (!why) break;
        for (int kind = 0; kind < HSD_LIST_KINDS; kind++)
            if (const int64_t n = s.n[kind].exchange(0)) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " [%s] %s %.2f;", why, kinds[kind], n / frames);
    }
    logf("direct: %s", out);
    for (Site& s : sites) {
        const uint64_t lr = s.lr.load();
        if (!lr) break;
        k = std::snprintf(out, cap, "draws a frame in the lists begun from %#llx:", (unsigned long long)lr);
        for (int r = 0; r < R_COUNT; r++)
            if (const int64_t n = s.n[r].exchange(0)) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %s %.2f,", kReasons[r], n / frames);
        logf("direct: %s", out);
    }
    shader_stats(out, cap, frames);
    logf("direct: %s", out);
    heap_stats(out, cap);
    logf("direct: %s", out);
    state_stats(out, cap, frames);
    logf("direct: %s", out);
}

}  // namespace hsd
