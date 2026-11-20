// Dispatches and the buffer ops (vk_compute.h). The compute half of gpu/legacy/vk/vk_shader.cpp translate_into, Env and
// vk_bank.cpp's compute bank (cs_layout, driver_bank_fill's stage 5), on the step-B interface of the graphics programs
// (vk_shader.cpp): handles read by the shader from c0, storage buffers and global memory by address, uniform buffers
// through an address table; translated from the code alone (docs/direct/shader-contract.md, "Compute programs"). What
// differs from the frozen back end: the grid is NumWorkgroups (no CPU read of an indirect dispatch's counts, no copy
// into c0), the 0x880 pointer table is not made (global memory is translated by address), and the networks run as the
// game has them (the matrix multiplies in software on this card): no stand-ins (plan rule 8).
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <vector>

#include "vk_marks.h"
#include "os.h"
#include "vk_passtime.h"
#include "vk_pstats.h"
#include "vk_check.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/control_flow.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/translate_program.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

#include "vk_cmd.h"
#include "vk_compute.h"
#include "vk_draw.h"
#include "vk_pool.h"
#include "vk_shader.h"
#include "vk_shader_parts.h"
#include "vk_work.h"
#include "vk_cache_file.h"

namespace Shader::Optimization {   // (collect_shader_info_pass.cpp: NVN2 compute's storage buffer table, this thread's program)
extern thread_local u32 g_nvn2_compute_ssbo_base;
extern thread_local u32 g_nvn2_compute_ssbo_count;
extern thread_local u32 g_nvn2_compute_layout_end;
}  // namespace Shader::Optimization

#ifndef OUNCE_RENDERER_COMPUTE_STAMP
#define OUNCE_RENDERER_COMPUTE_STAMP 0ull
#endif

namespace hsd {
double seconds_since_device();                                         // (vk_draw.cpp)

namespace {

using clk = std::chrono::steady_clock;
namespace parts = shader_parts;

PFN_vkCmdDispatch pDispatch = nullptr;
PFN_vkCmdDispatchIndirect pDispatchIndirect = nullptr;
PFN_vkCmdCopyBuffer pCopyBuffer = nullptr;
PFN_vkCmdFillBuffer pFillBuffer = nullptr;
void load_functions() {
    static std::once_flag once;
    std::call_once(once, [] {
        pDispatch = (PFN_vkCmdDispatch)vkGetDeviceProcAddr(dev.device, "vkCmdDispatch");
        pDispatchIndirect = (PFN_vkCmdDispatchIndirect)vkGetDeviceProcAddr(dev.device, "vkCmdDispatchIndirect");
        pCopyBuffer = (PFN_vkCmdCopyBuffer)vkGetDeviceProcAddr(dev.device, "vkCmdCopyBuffer");
        pFillBuffer = (PFN_vkCmdFillBuffer)vkGetDeviceProcAddr(dev.device, "vkCmdFillBuffer");
        // (no cooperative matrices on this card (RDNA2): the networks' matrix multiplies in a shader function on subgroup
        // shuffles, as the frozen back end runs them here, gpu/legacy/vk/vk_coopmat.cpp software_multiplies)
        Shader::Backend::SPIRV::mma_software = true;
    });
}

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {   // (vk_shader.cpp's: the same code keys)
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// <stamp compute>
// ---- NVN2 compute's c0 (vk_bank.cpp CsLayout: control section +0xa9 push words, +0xb1 / b2 / b3 / b6 slot counts) ----
constexpr uint32_t kControlSize = 0xd8, kBankMax = 0x1000;
struct Layout {
    uint32_t ubo = 0, ssbo = 0, tex = 0, img = 0, push = 0;
    uint32_t ubo_at() const { return 0x10 + push; }
    uint32_t ssbo_at() const { return ubo_at() + 16 * ubo; }
    uint32_t tex_at() const { return ssbo_at() + 16 * ssbo; }
    uint32_t img_at() const { return tex_at() + 8 * tex; }
    uint32_t end() const { return img_at() + 8 * img; }
};
Layout layout_of(const uint8_t* ctl) {
    Layout l;
    l.ubo = ctl[0xb1], l.ssbo = ctl[0xb2], l.tex = ctl[0xb3], l.img = ctl[0xb6];
    l.push = ((uint32_t)ctl[0xa9] * 4 + 15) & ~15u;
    return l;
}
// </stamp compute>
}  // namespace

// ---- a list's compute state ----
struct ListComp {
    nvn_prog prog{};
    bool has_prog = false;
    nvn_p_bind_buf ubo[16]{}, ssbo[16]{};
    uint32_t tex[128]{}, img[32]{};
    uint64_t tex_src[128]{}, img_src[32]{};   // (deferred binds: the game's array's slot, read at the submit)
    uint8_t push[0x400]{};
    uint32_t push_n = 0;
    uint32_t samp_binds = 0;   // (separate samplers bound for compute: counted)
    std::vector<std::pair<uint64_t, uint64_t>> updates;   // (UPDATE_UB ranges in the list: a compute uniform buffer among them is counted)
    struct Fixup {
        uint8_t* dst;
        uint64_t src;
    };
    std::vector<Fixup> fixups;
    // (what the command buffer being recorded has)
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkShaderEXT bound = VK_NULL_HANDLE;
};

namespace {

// ---- skips and counts ----
enum Reason {
    D_DRAWN, D_INDIRECT, D_NO_PROGRAM, D_NOT_READ, D_NOT_COMPUTE, D_TRANSLATING, D_FAILED, D_VARIANT, D_UBO_MEM, D_ARENA,
    D_ARGS_MEM, D_EMPTY, D_COUNT
};
const char* const kReasons[D_COUNT] = {
    "recorded", "of them indirect", "no compute program bound", "program not read yet", "program has no compute stage",
    "variant being translated", "variant does not translate", "variant cannot be dispatched", "a uniform buffer not in a region",
    "the frame arena full", "indirect arguments not in a region", "empty grid"};
struct TStats {   // (the recording thread's own, summed by compute_log: as vk_draw.cpp's)
    std::atomic<int64_t> by_kind[GPU_LIST_KINDS][D_COUNT];
    std::atomic<int64_t> ticks, past_range, updated_ubo, buf_ops[4];
    int64_t seen_by_kind[GPU_LIST_KINDS][D_COUNT];
    int64_t seen[8];
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
double ticks_per_ns() {
    static const double r = [] {
        const auto a = std::chrono::steady_clock::now();
        const uint64_t t = __builtin_ia32_rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - a).count();
        return (double)(__builtin_ia32_rdtsc() - t) / ns;
    }();
    return r;
}
inline void bump(std::atomic<int64_t>& c, int64_t n = 1) { c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
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
int64_t take_buf(int i) {
    int64_t n = 0;
    for (TStats* t = tstats_all.load(std::memory_order_acquire); t; t = t->next) {
        const int64_t v = t->buf_ops[i].load(std::memory_order_relaxed);
        n += v - t->seen[4 + i], t->seen[4 + i] = v;
    }
    return n;
}
std::atomic<int64_t> st_codes{0}, st_translated{0}, st_failed{0}, st_from_disk{0}, st_cs_objects{0}, st_object_failed{0}, st_translate_ns{0},
    st_object_ns{0}, st_slots{0}, st_fixups{0}, st_misses{0}, st_variants{0}, st_samp_binds{0};
std::atomic<double> last_miss_s{0};
std::atomic<int64_t> st_waited{0}, st_prefetched{0}, st_bumped{0}, st_read_here{0};
std::atomic<int64_t> st_from_binary{0}, st_binary_failed{0};

uint32_t word_of(const uint8_t* bank, uint32_t n, uint32_t off) {
    uint32_t v = 0;
    if (off + 4 <= n) std::memcpy(&v, bank + off, 4);
    return v;
}

// ---- programs: one variant per code key (translated from the code alone), slots per NVN serial ----
/// A texture or storage image the variant reads through a c0 word, and what its translation took it for: the dispatch
/// checks the bound one against it (check_bound)
struct Expect {
    uint16_t offset;   // (the handle's c0 word)
    uint8_t want, dim_fixed, desc_type;
    bool depth_float, image;
};
/// A handle the program uses both as an integer and as a float texture or image (texture_pass.cpp IntegerByUse): its
/// code is translated once for each answer, and a dispatch picks the variant of the bound one
struct Choice {
    uint16_t offset;   // (its c0 word)
    uint8_t image, c0;   // (a storage image's; read from c0, as every dispatch's handles are)
};
constexpr size_t kChoicesMax = 4;   // (the variants of one code: 2^choices; past this many, the rest are taken for floats)
struct Variant {
    uint64_t key = 0;
    // (translated: kept in the cache file, all but the SPIR-V, which is let go once the object is made)
    std::vector<Choice> choices;   // (the code's, the same in each of its variants)
    uint32_t mask = 0;             // (bit i: choices[i] taken for an integer one)
    std::vector<uint32_t> spirv;
    uint64_t object = 0;   // (its shader object's key in the driver binaries)
    std::vector<uint8_t> c1;
    Shader::Info info;
    uint32_t wg[3]{1, 1, 1}, shared = 0;
    // (made)
    std::vector<Expect> expects;
    VkShaderEXT cs = VK_NULL_HANDLE;
    VkDeviceAddress c1_addr = 0;
    uint32_t bank_bytes = 16;
    struct Ubo {
        uint8_t index;
        uint32_t bytes;
    };
    std::vector<Ubo> ubos;
    const char* skip = nullptr;
    bool no_binary = false;   // (finish: kept, but the driver has not its binary: translated again)
    bool told = false;   // (its first dispatch logged)
    uint32_t lay_ssbo_at = 0, lay_ssbo = 0;   // (its layout's storage buffer table, for that log)
};
/// A code's variants, by mask (one, but for a code with choices)
struct VariantSet {
    std::vector<Variant*> by_mask;
};
struct Family {   // (one compute code: every program slot with it shares its variants)
    uint64_t key = 0;
    Layout lay;
    nvn_stage_data sd{};
    std::atomic<VariantSet*> variants{nullptr};
    std::atomic<bool> loaded{false}, failed{false};
    std::mutex mu;   // (queued, waiting, and loaded's change)
    work::TicketRef queued;   // (its translation's job, to bump when a dispatch needs it)
    std::vector<std::atomic<int>*> waiting;   // (other slots of the code, made ready when it has been looked up in the cache)
};
std::mutex families_mu;
std::unordered_map<uint64_t, Family*> families;
struct Slot {
    uint64_t serial = 0;
    nvn_prog prog{};
    std::atomic<int> state{0};   // (0 being read, 1 a compute program, 2 none, 3 its code not readable)
    Family* fam = nullptr;
    work::TicketRef scan;   // (its reading, queued at P1 when the game set it)
};
constexpr uint32_t kSlots = 1u << 17;
std::unique_ptr<std::atomic<Slot*>[]> table;
std::mutex table_mu;
uint32_t slot_hash(uint64_t serial) { return (uint32_t)((serial * 0x9E3779B97F4A7C15ull) >> 47) & (kSlots - 1); }
Slot* slot_find(uint64_t serial) {
    if (!table) return nullptr;
    for (uint32_t k = slot_hash(serial), n = 0; n < kSlots; k = (k + 1) & (kSlots - 1), n++) {
        Slot* s = table[k].load(std::memory_order_acquire);
        if (!s) return nullptr;
        if (s->serial == serial) return s;
    }
    return nullptr;
}

// ---- the cache file (ounce_renderer_compute.bin, vk_cache_file.h: a record by code key) ----
constexpr uint64_t kDiskMagic = 0x3243445348ull;   // ('HSDC2')
constexpr uint64_t kDiskVersion = 4;   // (the record's layout, disk_save: a change to it changes this)
CacheFile disk;
/// The cache's stamp: the compute translation's code (OUNCE_RENDERER_COMPUTE_STAMP, renderer/meson/shader_stamp.py: the
/// regions marked "stamp compute" here and "stamp shared" in vk_shader.cpp, the translator and the compiler and flags
/// that build it), the record format (kDiskVersion), the device (parts::device_stamp, the loop cap among it), and the
/// matrix multiplies' form
uint64_t disk_stamp() {
    const uint64_t s[4] = {OUNCE_RENDERER_COMPUTE_STAMP, parts::device_stamp(), kDiskVersion, Shader::Backend::SPIRV::mma_software ? 1ull : 0ull};
    return hash_bytes(s, sizeof s);
}
void disk_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        load_functions();   // (mma_software: in the stamp)
        const uint64_t h[2] = {kDiskMagic, disk_stamp()};
        disk.open(parts::program_dir() + "ounce_renderer_compute.bin", "compute cache", h, sizeof h);
    });
}
template <class T> void put(std::vector<uint8_t>& o, const T& v) { o.insert(o.end(), (const uint8_t*)&v, (const uint8_t*)&v + sizeof v); }
template <class T> bool get(const uint8_t*& p, const uint8_t* e, T& v) {
    if ((size_t)(e - p) < sizeof v) return false;
    std::memcpy(&v, p, sizeof v), p += sizeof v;
    return true;
}
void disk_save(const Variant& v) {
    disk_init();
    if (!disk.on()) return;
    std::vector<uint8_t> o;
    put(o, v.mask), put(o, (uint64_t)v.choices.size());
    for (const Choice& c : v.choices) put(o, c);
    put(o, v.wg), put(o, v.shared), put(o, v.object);
    put(o, (uint64_t)v.c1.size());
    o.insert(o.end(), v.c1.begin(), v.c1.end());
    parts::info_put(o, v.info);
    disk.append(v.key, o.data(), o.size());
}
bool disk_read(const CacheFile::Rec& d, Variant& v) {
    std::vector<uint8_t> data;
    if (!disk.read(d, data)) return false;
    const uint8_t* p = data.data();
    const uint8_t* e = p + data.size();
    uint64_t n = 0;
    if (!get(p, e, v.mask) || !get(p, e, n) || n > kChoicesMax) return false;
    v.choices.resize(n);
    for (Choice& c : v.choices)
        if (!get(p, e, c)) return false;
    if (!get(p, e, v.wg) || !get(p, e, v.shared) || !get(p, e, v.object) || !get(p, e, n) || n > (uint64_t)(e - p)) return false;
    v.c1.assign(p, p + n), p += n;
    return parts::info_get(p, e, v.info);
}

// <stamp compute>
// ---- the translator's questions (vk_shader.cpp Env, compute), answered from the program's code alone ----
// No dispatch state reaches a translation: c0's words read 0, so a texture's type is its instruction's (Color2D where the
// instruction has none, a size query's), and whether a texture or storage image is an integer one is decided by how the
// program uses its texels (Environment::IntegerFromUse, texture_pass.cpp IntegerByUse). The grid's words are
// NumWorkgroups. A dispatch checks the bound handles against that (check_bound): one that contradicts it is an error
struct Code {
    uint64_t key = 0;
    std::vector<u128> code;   // (the native instructions)
    std::vector<uint8_t> c1;
    uint32_t local[3]{}, shared = 0, local_mem = 0;
    Layout lay;
};
/// The compute stage's native instructions up to the program's end, and what its control section says of it
bool parse_code(const uint8_t* ctl, const std::vector<uint8_t>& area, Code& c) {
    uint32_t len, c1o;
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4), std::memcpy(&c1o, ctl + 0x40, 4), std::memcpy(&st, ctl + 0x56, 2);
    if (st != 5 || c1o > area.size()) return false;
    c.key = hash_bytes(area.data(), area.size(), hash_bytes(ctl, kControlSize));
    using Shader::Ampere::Instruction;
    Shader::Ampere::ProgramCaptureState capture;
    for (uint32_t i = 0; (i + 1) * sizeof(Instruction) <= len && (i + 1) * sizeof(Instruction) <= area.size(); i++) {
        Instruction insn;
        std::memcpy(&insn, area.data() + i * sizeof(Instruction), sizeof insn);
        c.code.push_back({uint64_t(insn.words[0]) | uint64_t(insn.words[1]) << 32, uint64_t(insn.words[2]) | uint64_t(insn.words[3]) << 32});
        const auto r = capture.Observe(insn, i);
        if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            static std::atomic<int> told{0};
            if (told++ < 40) logf("direct: compute program %016llx: a branch target outside the program at +%#x", (unsigned long long)c.key, i * 16);
            return false;
        }
        if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    c.c1.assign(area.begin() + c1o, area.end());
    std::memcpy(c.local, ctl + 0x58, 12);
    std::memcpy(&c.shared, ctl + 0x64, 4);
    std::memcpy(&c.local_mem, ctl + 0x68, 4);   // (the QMD's local memory per thread)
    c.lay = layout_of(ctl);
    return true;
}
bool read_code(const nvn_stage_data& sd, Code& c) {
    uint8_t ctl[kControlSize];
    std::vector<uint8_t> area;
    return parts::read_stage(sd, ctl, area) && parse_code(ctl, area, c);
}

class Env final : public Shader::Environment {
public:
    /// order: the code's choices (keys, sorted) and mask the answers this translation takes for them (none: all floats)
    Env(const Code& c, const std::vector<uint64_t>& order, uint32_t mask) : nvn{c}, order{order}, mask{mask} {
        stage = Shader::Stage::Compute;
        start_address = 0;
        is_proprietary_driver = true;
    }
    u64 ReadInstruction(u32 address) override { throw Shader::LogicError("a 64-bit instruction read at {:#x}", address); }
    u128 ReadInstruction128(u32 address) override {
        if (address % 16 || address / 16 >= nvn.code.size()) throw Shader::RuntimeError("instruction read past the program at {:#x}", address);
        return nvn.code[address / 16];
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        uint32_t v = 0;
        if (index == 1 && (size_t)offset + 4 <= nvn.c1.size()) std::memcpy(&v, nvn.c1.data() + offset, 4);
        return v;   // (c0, the driver bank, and the game's buffers: not the program's own, read 0)
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return Shader::TexturePixelFormat{}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    bool IntegerFromUse() const override { return true; }
    bool IntegerChoice(u64 key) override {
        mixed.insert(key);
        const auto it = std::find(order.begin(), order.end(), key);
        return it != order.end() && (mask >> (it - order.begin()) & 1);
    }
    std::set<uint64_t> mixed;   // (the code's choices, as the texture pass met them)
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }   // (NVN2 compute: c0)
    u32 LocalMemorySize() const override { return nvn.local_mem; }
    u32 SharedMemorySize() const override { return nvn.shared; }
    std::array<u32, 3> WorkgroupSize() const override {
        return {std::max(1u, nvn.local[0]), std::max(1u, nvn.local[1]), std::max(1u, nvn.local[2])};
    }
    bool HasHLEMacroState() const override { return true; }   // (the grid's words are replaced, GetReplaceConstBuffer)
    bool HasNativeInstructions() const override { return true; }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {   // (step B: a word of c0's storage buffer table)
        const uint32_t base = nvn.lay.ssbo_at();
        return index == 0 && offset >= base && offset < base + 16 * nvn.lay.ssbo && (offset - base) % 16 == 0;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        if (bank != 0) return std::nullopt;
        switch (offset) {
        case 0x0: return Shader::ReplaceConstant::NumWorkgroupsX;
        case 0x4: return Shader::ReplaceConstant::NumWorkgroupsY;
        case 0x8: return Shader::ReplaceConstant::NumWorkgroupsZ;
        default: return std::nullopt;
        }
    }
    void Dump(u64, u64) override {}

private:
    const Code& nvn;
    const std::vector<uint64_t>& order;
    uint32_t mask;
};

struct Translation {
    Shader::ObjectPool<Shader::IR::Inst> insts;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Flow::Block> flow_blocks;
    Shader::IR::Program program;
};

/// The c0 reads the grid fold did not take (a non-constant offset, or one inside the grid's words read otherwise): the
/// dispatch's bank holds the grid only for direct dispatches (counted per variant)
int grid_reads_left(const Shader::IR::Program& p) {
    int n = 0;
    for (const Shader::IR::Block* b : p.post_order_blocks)
        for (const Shader::IR::Inst& i : b->Instructions()) {
            const auto op = i.GetOpcode();
            if (op != Shader::IR::Opcode::GetCbufU32 && op != Shader::IR::Opcode::GetCbufF32 && op != Shader::IR::Opcode::GetCbufU32x2 &&
                op != Shader::IR::Opcode::GetCbufU8 && op != Shader::IR::Opcode::GetCbufS8 && op != Shader::IR::Opcode::GetCbufU16 &&
                op != Shader::IR::Opcode::GetCbufS16)
                continue;
            const Shader::IR::Value bank = i.Arg(0), off = i.Arg(1);
            if (!bank.IsImmediate() || bank.U32() != 0) continue;
            if (!off.IsImmediate() || off.U32() < 0xC) n++;
        }
    return n;
}

std::atomic<int64_t> st_grid_left{0}, st_coopmat{0};

/// One variant of the code (order, mask: Env's); mixed: the choices the translation met
bool translate_variant(Variant& v, const Code& c, const std::vector<uint64_t>& order, uint32_t mask, std::vector<uint64_t>* mixed) {
    try {
        Env env{c, order, mask};
        Shader::Optimization::g_nvn2_compute_ssbo_base = c.lay.ssbo ? c.lay.ssbo_at() : 0;
        Shader::Optimization::g_nvn2_compute_ssbo_count = c.lay.ssbo ? c.lay.ssbo : 16;
        Shader::Optimization::g_nvn2_compute_layout_end = 0;   // (no 0x880 pointer table: global memory by address)
        struct Reset {
            ~Reset() {
                Shader::Optimization::g_nvn2_compute_ssbo_base = 0, Shader::Optimization::g_nvn2_compute_ssbo_count = 16;
                Shader::Optimization::g_nvn2_compute_layout_end = 0;
            }
        } reset;
        auto t = std::make_unique<Translation>();
        Shader::Ampere::Flow::CFG cfg{env, t->flow_blocks, false};
        t->program = Shader::Ampere::TranslateProgram(t->insts, t->blocks, env, cfg, parts::translate_info());
        if (const int left = grid_reads_left(t->program)) {
            st_grid_left.fetch_add(1, std::memory_order_relaxed);
            logf("direct: compute program %016llx reads c0's grid words %d times otherwise than by a constant offset: an indirect "
                 "dispatch gives them 0 there", (unsigned long long)c.key, left);
        }
        Shader::RuntimeInfo rt{};
        Shader::Backend::Bindings bindings{};
        v.spirv = Shader::Backend::SPIRV::EmitSPIRV(parts::stage_profile(VK_SHADER_STAGE_COMPUTE_BIT), rt, t->program, bindings);
        v.info = t->program.info;
        v.c1 = c.c1;
        v.wg[0] = t->program.workgroup_size[0], v.wg[1] = t->program.workgroup_size[1], v.wg[2] = t->program.workgroup_size[2];
        v.shared = t->program.shared_memory_size;
        if (v.info.uses_cooperative_matrix) st_coopmat.fetch_add(1, std::memory_order_relaxed);
        if (mixed) mixed->assign(env.mixed.begin(), env.mixed.end());
        v.mask = mask;
        for (size_t i = 0; i < order.size(); i++)
            v.choices.push_back({(uint16_t)order[i], (uint8_t)(order[i] >> 63), (uint8_t)((order[i] >> 32 & 0x7fffffffu) == 0)});
        return true;
    } catch (const std::exception& e) {
        static std::atomic<int> told{0};
        if (told++ < 30) logf("direct: compute program %016llx not translated: %s", (unsigned long long)c.key, e.what());
        return false;
    }
}

// </stamp compute>

/// What a dispatch checks of the handles the variant reads by a c0 word: that the bound texture or image is what the
/// translation from the code alone took it for (its type where the translation asked for one, rectangle, buffer, and
/// integer or not), canonicalised as its descriptor
void expects_of(Variant& v) {
    v.expects.clear();
    for (const auto& d : v.info.texture_descriptors) {
        if (d.count != 1 || d.handle_program != ~0u || d.has_secondary || d.cbuf_index != 0) continue;
        Expect x{};
        x.offset = (uint16_t)d.cbuf_offset, x.dim_fixed = d.type_fixed, x.desc_type = (uint8_t)d.type;
        x.depth_float = d.is_depth && !d.is_integer;
        x.want = parts::answer_canonical((uint8_t)((uint8_t)Shader::TextureType::Color2D | (d.is_integer ? 0x80 : 0)), x.dim_fixed, x.desc_type, x.depth_float);
        v.expects.push_back(x);
    }
    for (const auto& d : v.info.image_descriptors) {
        if (d.count != 1 || d.handle_program != ~0u || d.cbuf_index != 0) continue;
        Expect x{};
        x.offset = (uint16_t)d.cbuf_offset, x.image = true, x.want = d.is_integer ? 0x80 : 0;
        v.expects.push_back(x);
    }
}

std::atomic<int64_t> st_contradicted{0};
/// The bound handles against the variant's expects: one that contradicts the translation is sampled or stored wrong: an
/// error, logged once a program, word and answer with the texture, counted, and a strict run's event
void check_bound(const Variant& v, const uint8_t* bank, uint32_t n) {
    for (const Expect& x : v.expects) {
        const uint32_t h = word_of(bank, n, x.offset);
        if (!h) continue;
        const uint8_t live = parts::answer_of(h);
        const uint8_t bound = x.image ? (uint8_t)(live & 0x80) : parts::answer_canonical(live, x.dim_fixed, x.desc_type, x.depth_float);
        if (bound == x.want) continue;
        st_contradicted.fetch_add(1, std::memory_order_relaxed);
        static std::mutex mu;
        static std::unordered_set<uint64_t> told;
        {
            std::lock_guard lk(mu);
            if (!told.insert(v.key ^ ((uint64_t)x.offset << 32 | (uint64_t)x.image << 16 | bound)).second) continue;
        }
        char tex[256];
        pool_describe(h & 0xFFFFF, tex, sizeof tex);
        char where[200];
        std::snprintf(where, sizeof where, "compute program %016llx %s c0+%#x handle %#x: bound type %#x%s, translated for %#x%s",
                      (unsigned long long)v.key, x.image ? "image" : "texture", x.offset, h, live & 0x7f, live & 0x80 ? " integer" : "",
                      x.want & 0x7f, x.want & 0x80 ? " integer" : "");
        logf("direct: error: a bound texture contradicts the translation from the code alone: %s; %s", where, tex);
        if (strict_on()) strict_event("a bound texture contradicts the translation from the code alone", "%s", where);
    }
}

// ---- the shader object (vk_shader.cpp finish's, one compute stage) ----
/// The variant's uniform buffers (by device address: the table's address pushed at kTablePush, entries in its descriptors'
/// order), then its shader object (none for a variant no dispatch can run). A kept variant (no SPIR-V) is made from this
/// driver's binary; with none, no_binary: it is translated again
bool finish(Variant& v, const Layout& lay) {
    const Shader::Info& info = v.info;
    if (v.spirv.empty() && v.object && !parts::object_has(v.object)) return v.no_binary = true, false;
    for (const auto& c : info.constant_buffer_descriptors) {
        if (c.count != 1) v.skip = v.skip ? v.skip : "a uniform buffer array";
        const uint32_t used = c.index < info.constant_buffer_used_sizes.size() ? info.constant_buffer_used_sizes[c.index] : 0;
        v.ubos.push_back({(uint8_t)c.index, used});
    }
    if (!info.storage_buffers_descriptors.empty() && !info.ssbo_bda) v.skip = v.skip ? v.skip : "storage buffers not by address";
    if (!info.texture_buffer_descriptors.empty()) v.skip = v.skip ? v.skip : "a texel buffer";
    if (!info.image_buffer_descriptors.empty()) v.skip = v.skip ? v.skip : "an image buffer";
    {   // (c0's copy: what the program reads of it, and the layout's tables at least)
        const uint32_t used = info.constant_buffer_used_sizes.empty() ? 0 : info.constant_buffer_used_sizes[0];
        v.bank_bytes = std::clamp<uint32_t>((std::max(used, lay.end()) + 15) & ~15u, 16, kBankMax);
    }
    v.lay_ssbo_at = lay.ssbo_at(), v.lay_ssbo = lay.ssbo;
    expects_of(v);
    if (kTablePush + 8 > caps.max_push) v.skip = v.skip ? v.skip : "push data over the device's limit";
    if (v.skip) {
        logf("direct: compute program %016llx cannot be dispatched: %s (skipped, counted)", (unsigned long long)v.key, v.skip);
        return true;
    }
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sz{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    sz.requiredSubgroupSize = 32;
    load_functions();
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    pool_shader_layout(ci);
    ci.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    ci.codeSize = v.spirv.size() * 4;
    ci.pCode = v.spirv.data();
    ci.pName = "main";
    if (caps.sg32_stages & VK_SHADER_STAGE_COMPUTE_BIT) ci.pNext = &sz;
    const auto t0 = clk::now();
    if (!v.spirv.empty()) v.object = parts::object_key(ci, (caps.sg32_stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0);
    VkResult r = VK_ERROR_UNKNOWN;
    bool from_binary = false;
    {   // (its binary, when this driver made it before from the same create info: no compile)
        const std::vector<uint8_t> bin = parts::object_binary(v.object);
        if (!bin.empty()) {
            VkShaderCreateInfoEXT bi = ci;
            bi.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT, bi.codeSize = bin.size(), bi.pCode = bin.data();
            r = vkCreateShadersEXT(dev.device, 1, &bi, nullptr, &v.cs);
            from_binary = r == VK_SUCCESS;
            if (!from_binary) v.cs = VK_NULL_HANDLE, st_binary_failed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!from_binary && v.spirv.empty()) return v.no_binary = true, false;
    if (!from_binary) r = vkCreateShadersEXT(dev.device, 1, &ci, nullptr, &v.cs);
    const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count();
    st_object_ns.fetch_add(ns, std::memory_order_relaxed);
    if (ns > 2'000'000'000ll)
        logf("direct: compute program %016llx: its shader object took %.1f s%s", (unsigned long long)v.key, ns / 1e9,
             info.uses_cooperative_matrix ? " (matrix multiplies in software)" : "");
    if (r != VK_SUCCESS) {
        st_object_failed.fetch_add(1, std::memory_order_relaxed);
        logf("direct: compute program %016llx: its shader object not made (%d)", (unsigned long long)v.key, (int)r);
        return false;
    }
    st_cs_objects.fetch_add(1, std::memory_order_relaxed);
    object_made(1, "a compute shader object");
    if (from_binary) st_from_binary.fetch_add(1, std::memory_order_relaxed);
    else parts::object_keep(v.object, v.cs);
    return true;
}

/// (a worker, or the precompile) the code's variants: from the cache when it holds all of them and this driver has their
/// binaries, else translated (once, then once more for each answer of its choices), their objects compiled and the
/// records kept. Null: the code does not translate or an object is not made
VariantSet* make_variants(const Code& code, const Layout& lay, bool* kept) {
    disk_init();
    {
        auto set = std::make_unique<VariantSet>();
        size_t want = 1;
        for (const CacheFile::Rec& d : disk.find(code.key)) {
            auto v = std::make_unique<Variant>();
            v->key = code.key;
            if (!disk_read(d, *v) || v->mask >= (1u << v->choices.size())) break;
            want = (size_t)1 << v->choices.size();
            if (set->by_mask.size() < want) set->by_mask.resize(want, nullptr);
            const uint32_t mask = v->mask;
            if (set->by_mask[mask] || !finish(*v, lay)) break;
            set->by_mask[mask] = v.release();
        }
        bool whole = !set->by_mask.empty();
        for (Variant* v : set->by_mask) whole = whole && v;
        if (whole) {
            st_from_disk.fetch_add((int64_t)set->by_mask.size(), std::memory_order_relaxed);
            if (kept) *kept = true;
            return set.release();
        }
        for (Variant* v : set->by_mask)   // (not all of them: none is used)
            if (v) {
                if (v->cs) vkDestroyShaderEXT(dev.device, v->cs, nullptr);
                delete v;
            }
    }
    const bool save = !disk.has(code.key);   // (records kept whose binaries this driver has not: the same again)
    const auto t1 = clk::now();
    auto set = std::make_unique<VariantSet>();
    std::vector<uint64_t> mixed;
    bool ok = true;
    for (uint32_t mask = 0, n = 1; mask < n && ok; mask++) {
        auto v = std::make_unique<Variant>();
        v->key = code.key;
        static const std::vector<uint64_t> none;
        ok = mask ? translate_variant(*v, code, mixed, mask, nullptr) : translate_variant(*v, code, none, 0, &mixed);
        if (ok && mask == 0 && !mixed.empty()) {   // (the first translation found its choices: it is translated again with them)
            if (mixed.size() > kChoicesMax) {
                logf("direct: compute program %016llx uses %zu textures or images both as integer and float ones: the last %zu are "
                     "taken for floats", (unsigned long long)code.key, mixed.size(), mixed.size() - kChoicesMax);
                mixed.resize(kChoicesMax);
            }
            n = 1u << mixed.size();
            v = std::make_unique<Variant>();
            v->key = code.key;
            ok = translate_variant(*v, code, mixed, 0, nullptr);
        }
        ok = ok && finish(*v, lay);
        if (!ok) break;
        if (save) disk_save(*v);
        std::vector<uint32_t>().swap(v->spirv);
        set->by_mask.push_back(v.release());
    }
    st_translate_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t1).count(), std::memory_order_relaxed);
    (ok ? st_translated : st_failed).fetch_add(ok ? (int64_t)set->by_mask.size() : 1, std::memory_order_relaxed);
    if (!ok) {
        for (Variant* v : set->by_mask) {
            if (v->cs) vkDestroyShaderEXT(dev.device, v->cs, nullptr);
            delete v;
        }
        return nullptr;
    }
    if (kept) *kept = false;
    return set.release();
}

void publish(Family& f, VariantSet* set) {
    for (Variant* v : set->by_mask) {
        v->c1_addr = parts::put_c1(v->c1, v->info.constant_buffer_used_sizes.size() > 1 ? v->info.constant_buffer_used_sizes[1] : 0);
        st_variants.fetch_add(1, std::memory_order_relaxed);
        mem_account_main(MO_M_SHADERS, (int64_t)(sizeof *v + v->c1.capacity()));
    }
    f.variants.store(set, std::memory_order_release);
}

/// The variant of the set for the handles the list has bound at its choices' words (a deferred bind's from the game's
/// array now)
Variant* pick(const VariantSet& set, const ListComp& c, const Layout& lay) {
    if (set.by_mask.size() == 1) return set.by_mask[0];
    const std::vector<Choice>& choices = set.by_mask[0]->choices;
    uint32_t mask = 0;
    for (size_t i = 0; i < choices.size(); i++) {
        const Choice& x = choices[i];
        const uint32_t at = x.image ? lay.img_at() : lay.tex_at(), lim = x.image ? 32 : 128;
        if (!x.c0 || x.offset < at || (x.offset - at) % 8 || (x.offset - at) / 8 >= lim) continue;
        const uint32_t k = (x.offset - at) / 8;
        uint32_t h = x.image ? c.img[k] : c.tex[k];
        if (const uint64_t src = x.image ? c.img_src[k] : c.tex_src[k]) {
            uint64_t w = 0;
            host->gpu_read(src, &w, 8);
            h = (uint32_t)w;
        }
        if (h && (parts::answer_of(h) & 0x80)) mask |= 1u << i;
    }
    return mask < set.by_mask.size() ? set.by_mask[mask] : nullptr;
}

/// (a worker) a program's stages read: its compute stage's code key and its family; the first slot of a code makes its
/// variant (from the cache, or translated now, when the game sets it, so a load's programs are made side by side)
void scan_slot(Slot* s) {
    int cs = -1;
    uint8_t ctl[kControlSize];
    std::vector<uint8_t> area;
    uint64_t key = 0;
    for (uint32_t i = 0; i < s->prog.nstage && i < 6; i++) {
        if (!parts::read_stage(s->prog.sd[i], ctl, area)) return s->state.store(3, std::memory_order_release);
        uint16_t st;
        std::memcpy(&st, ctl + 0x56, 2);
        if (st == 5) {
            cs = (int)i, key = hash_bytes(area.data(), area.size(), hash_bytes(ctl, kControlSize));
            break;
        }
    }
    if (cs < 0) return s->state.store(2, std::memory_order_release);
    bool fresh = false;
    {
        std::lock_guard lk(families_mu);
        auto [it, made] = families.try_emplace(key, nullptr);
        if (made) {
            Family* f = new Family;
            f->key = key, f->sd = s->prog.sd[cs], f->lay = layout_of(ctl);
            it->second = f;
            st_codes.fetch_add(1, std::memory_order_relaxed);
        }
        s->fam = it->second, fresh = made;
    }
    if (fresh) pack_check_set(key, "compute program");
    Family* f = s->fam;
    if (!fresh) {   // (the code's variant is made by the slot that saw it first: this one is ready when it is queued)
        std::lock_guard lk(f->mu);
        if (!f->loaded.load(std::memory_order_acquire)) return f->waiting.push_back(&s->state);
        return s->state.store(1, std::memory_order_release);
    }
    work::TicketRef t = work::make([f] {
        Code code;
        VariantSet* v = read_code(f->sd, code) && code.key == f->key ? make_variants(code, f->lay, nullptr) : nullptr;
        if (v) publish(*f, v);
        else f->failed.store(true, std::memory_order_release);
    });
    {
        std::lock_guard lk(f->mu);
        f->queued = t;
        f->loaded.store(true, std::memory_order_release);
        for (std::atomic<int>* w : f->waiting) w->store(1, std::memory_order_release);
        f->waiting.clear();
    }
    s->state.store(1, std::memory_order_release);
    st_prefetched.fetch_add(1, std::memory_order_relaxed);
    work::push(t, work::P1);
}
Slot* slot_make(const nvn_prog& p) {
    std::lock_guard lk(table_mu);
    if (!table) {
        table.reset(new std::atomic<Slot*>[kSlots]);
        for (uint32_t i = 0; i < kSlots; i++) table[i].store(nullptr, std::memory_order_relaxed);
    }
    if (Slot* s = slot_find(p.serial)) return s;
    if (st_slots.load(std::memory_order_relaxed) >= (int64_t)(kSlots * 3 / 4)) return nullptr;
    Slot* s = new Slot;
    s->serial = p.serial, s->prog = p;
    s->scan = work::make([s] { scan_slot(s); });
    for (uint32_t k = slot_hash(p.serial);; k = (k + 1) & (kSlots - 1))
        if (!table[k].load(std::memory_order_relaxed)) {
            table[k].store(s, std::memory_order_release);
            break;
        }
    st_slots.fetch_add(1, std::memory_order_relaxed);
    work::push(s->scan, work::P1);
    return s;
}

// ---- c0 ----
/// The bank as NVN2 lays it out for this dispatch (vk_bank.cpp driver_bank_fill, stage 5); deferred words noted at their place
void bank_fill(ListComp& c, const Layout& l, const uint32_t grid[3], uint8_t* d, uint32_t n, bool note) {
    std::memset(d, 0, n);
    auto put32 = [&](uint32_t off, uint32_t v) {
        if (off + 4 <= n) std::memcpy(d + off, &v, 4);
    };
    for (uint32_t i = 0; i < 3; i++) put32(4 * i, grid[i]);
    for (uint32_t o = 0; o + 4 <= l.push; o += 4) {
        uint32_t v = 0;
        if (o + 4 <= c.push_n) std::memcpy(&v, c.push + o, 4);
        put32(0x10 + o, v);
    }
    auto entry = [&](uint32_t off, const nvn_p_bind_buf& b) { put32(off, (uint32_t)b.addr), put32(off + 4, (uint32_t)(b.addr >> 32)), put32(off + 8, (uint32_t)b.size); };
    for (uint32_t i = 0; i < std::min(l.ubo, 16u); i++) entry(l.ubo_at() + 16 * i, c.ubo[i]);
    for (uint32_t i = 0; i < std::min(l.ssbo, 16u); i++) entry(l.ssbo_at() + 16 * i, c.ssbo[i]);
    for (uint32_t i = 0; i < l.tex; i++) {
        const uint32_t off = l.tex_at() + 8 * i, k = std::min(i, 127u);
        put32(off, c.tex[k]);
        if (note && c.tex_src[k] && off + 4 <= n) c.fixups.push_back({d + off, c.tex_src[k]});
    }
    for (uint32_t i = 0; i < l.img; i++) {
        const uint32_t off = l.img_at() + 8 * i, k = std::min(i, 31u);
        put32(off, c.img[k]);
        if (note && c.img_src[k] && off + 4 <= n) c.fixups.push_back({d + off, c.img_src[k]});
    }
}

/// debug.skipcs=<key prefix>[+<prefix>...]: those programs' dispatches left out (a hang's diagnosis)
bool skipped_by_dbg(uint64_t key) {
    static const std::string list = [] {
        const char* v = dbg_value(SET_DBG_SKIPCS);
        return std::string(v ? v : "");
    }();
    if (list.empty()) return false;
    char hex[24];
    std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)key);
    for (size_t at = 0; at < list.size();) {
        const size_t e = list.find('+', at);
        const std::string k = list.substr(at, e == std::string::npos ? std::string::npos : e - at);
        if (!k.empty() && !std::strncmp(hex, k.c_str(), k.size())) return true;
        at = e == std::string::npos ? list.size() : e + 1;
    }
    return false;
}

ListComp& ctx(List* l) {
    if (!l->cctx) l->cctx = new ListComp;
    return *l->cctx;
}

void strict_dispatch(List* l, Reason r);
void count(List* l, Reason r) {
    bump(me().by_kind[l->info.kind][r]);
    if (strict_on() && r != D_DRAWN && r != D_INDIRECT && r != D_EMPTY) strict_dispatch(l, r);   // (M6b; an empty grid is NVN's no-op too)
}

/// A dispatch not recorded, for strict mode: its program as the list's state names it and its list
void strict_dispatch(List* l, Reason r) {
    const uint64_t serial = l->cctx && l->cctx->has_prog ? l->cctx->prog.serial : 0;
    char w[160];
    std::snprintf(w, sizeof w, "dispatch: program serial %#llx; list kind %u lr %#llx", (unsigned long long)serial, l->info.kind, (unsigned long long)l->info.lr);
    if (r == D_NOT_READ || r == D_TRANSLATING) strict_program_miss((std::string(kReasons[r]) + "; " + w).c_str());
    else strict_event(kReasons[r], "%s", w);
}

/// The first dispatch of each variant, named (its grid, its list, what it binds; its images as bound)
void tell_first(Variant& v, const List* l, const uint8_t* bank, uint32_t n, const uint32_t grid[3], bool indirect) {
    if (v.told) return;
    v.told = true;
    static const bool log_compute = dbg_log(SET_DBG_LOGCOMPUTE);
    if (!log_compute) return;
    char b[2000];
    int k = std::snprintf(b, sizeof b, "direct: compute %016llx first dispatched in a %s list (lr %#llx): %s %ux%ux%u, workgroup %ux%ux%u, "
                          "shared %u B, %zu ubos, %zu ssbos, %zu textures, %zu images%s%s:",
                          (unsigned long long)v.key, l->info.kind == GPU_LIST_COMPUTE ? "compute" : l->info.kind == GPU_LIST_LAYER ? "layer" : "other",
                          (unsigned long long)l->info.lr, indirect ? "indirect" : "grid", grid[0], grid[1], grid[2], v.wg[0], v.wg[1], v.wg[2], v.shared,
                          v.info.constant_buffer_descriptors.size(), v.info.storage_buffers_descriptors.size(), v.info.texture_descriptors.size(),
                          v.info.image_descriptors.size(), v.info.uses_cooperative_matrix ? ", matrix multiplies" : "",
                          v.info.uses_global_memory ? ", global memory" : "");
    for (uint32_t i = 0; i < 16 && k < 1500; i++)   // (the storage buffers as c0's table names them)
        if (word_of(bank, n, v.lay_ssbo_at + 16 * i) || word_of(bank, n, v.lay_ssbo_at + 16 * i + 4))
            if (i < v.lay_ssbo)
                k += std::snprintf(b + k, sizeof b - k, " [ssbo%u %#llx+%#x]", i,
                                   (unsigned long long)(word_of(bank, n, v.lay_ssbo_at + 16 * i) | (uint64_t)word_of(bank, n, v.lay_ssbo_at + 16 * i + 4) << 32),
                                   word_of(bank, n, v.lay_ssbo_at + 16 * i + 8));
    for (const auto& im : v.info.image_descriptors) {
        if (k > 1700) break;
        const uint32_t h = im.cbuf_index == 0 ? word_of(bank, n, im.cbuf_offset) : 0;
        char e[240];
        pool_describe(h & 0xFFFFF, e, sizeof e);
        k += std::snprintf(b + k, sizeof b - k, " [img c%u+%#x %s%s %s]", im.cbuf_index, im.cbuf_offset,
                           im.format == Shader::ImageFormat::Typeless ? "formatted" : "raw", im.is_written ? " written" : "", e);
    }
    logf("%s", b);
}

/// (debug.drawlog=<present>, the per-draw trace's dispatch lines) every dispatch of that frame: program, grid or
/// arguments, the storage and uniform buffers, each texture and image as its handle names it (pool_describe)
void trace_dispatch(const Variant& v, const List* l, const uint8_t* bank, uint32_t n, const uint32_t grid[3], uint64_t args) {
    static std::atomic<int> told{0};
    if (told++ > 1500) return;
    const ListComp& c = *l->cctx;
    char b[6000];
    int k = std::snprintf(b, sizeof b, "direct: drawlog dispatch lr %#llx kind %u prog %016llx ", (unsigned long long)l->info.lr, l->info.kind,
                          (unsigned long long)v.key);
    if (args) k += std::snprintf(b + k, sizeof b - k, "indirect %#llx", (unsigned long long)args);
    else k += std::snprintf(b + k, sizeof b - k, "grid %ux%ux%u", grid[0], grid[1], grid[2]);
    for (int i = 0; i < 16 && k < 4000; i++)
        if (c.ssbo[i].addr) {
            k += std::snprintf(b + k, sizeof b - k, " s%d=%#llx+%#llx", i, (unsigned long long)c.ssbo[i].addr, (unsigned long long)c.ssbo[i].size);
            if (v.info.uses_global_memory && c.ssbo[i].size && c.ssbo[i].size <= 0x40) {   // (a table of pointers: each in layer 0 or not)
                uint64_t w[8]{};
                host->gpu_read(c.ssbo[i].addr, w, std::min<uint64_t>(c.ssbo[i].size, sizeof w));
                for (int j = 0; j < 8 && k < 4000; j++) {
                    if (!w[j]) continue;
                    k += std::snprintf(b + k, sizeof b - k, " [+%d %#llx%s]", 8 * j, (unsigned long long)w[j],
                                       claims_bound(w[j], w[j] + 16) ? "" : " not in a region");
                }
            }
        }
    for (int i = 0; i < 16 && k < 4000; i++)
        if (c.ubo[i].addr) k += std::snprintf(b + k, sizeof b - k, " u%d=%#llx+%#llx", i, (unsigned long long)c.ubo[i].addr, (unsigned long long)c.ubo[i].size);
    for (const auto& t : v.info.texture_descriptors) {
        if (k > 5000) break;
        const uint32_t h = t.cbuf_index == 0 ? word_of(bank, n, t.cbuf_offset) : 0;
        char e[320];
        pool_describe(h & 0xFFFFF, e, sizeof e);
        k += std::snprintf(b + k, sizeof b - k, " tex c%u+%#x=%#x(%s)", t.cbuf_index, t.cbuf_offset, h, e);
    }
    for (const auto& im : v.info.image_descriptors) {
        if (k > 5600) break;
        const uint32_t h = im.cbuf_index == 0 ? word_of(bank, n, im.cbuf_offset) : 0;
        char e[320];
        pool_describe(h & 0xFFFFF, e, sizeof e);
        k += std::snprintf(b + k, sizeof b - k, " img c%u+%#x%s=%#x(%s)", im.cbuf_index, im.cbuf_offset, im.is_written ? " written" : "", h, e);
    }
    log_long(b, "direct: drawlog dispatch");
}

}  // namespace

void compute_state(List* l, uint32_t op, const void* a, uint32_t size) {
    switch (op) {
    case NOP_BIND_PROGRAM: {
        nvn_p_bind_program b{};
        std::memcpy(&b, a, std::min<size_t>(sizeof b, size));
        if (!(b.stages & NVN_SHADER_STAGE_COMPUTE_BIT)) return;
        ListComp& c = ctx(l);
        if (b.prog) c.prog = *b.prog;
        c.has_prog = b.prog && b.prog->nstage > 0;
        return;
    }
    case NOP_BIND_UBO: case NOP_BIND_SSBO: {
        nvn_p_bind_buf b{};
        std::memcpy(&b, a, std::min<size_t>(sizeof b, size));
        if (b.stage != 5 || b.index < 0 || b.index >= 16) return;
        (op == NOP_BIND_UBO ? ctx(l).ubo : ctx(l).ssbo)[b.index] = b;
        return;
    }
    case NOP_BIND_BUFS: {
        nvn_p_bind_bufs b{};
        std::memcpy(&b, a, std::min<size_t>(sizeof b, size));
        if (b.stage != 5) return;
        ListComp& c = ctx(l);
        for (int i = 0; i < b.n && i < 16; i++) {
            const int index = b.first + i;
            if (index < 0 || index >= 16) continue;
            nvn_p_bind_buf& u = (b.ssbo ? c.ssbo : c.ubo)[index];
            u.stage = 5, u.index = index, u.addr = b.r[i].address, u.size = b.r[i].size;
        }
        return;
    }
    case NOP_BIND_TEX: case NOP_BIND_IMG: case NOP_BIND_SAMP: {
        nvn_p_bind_handles b{};
        std::memcpy(&b, a, std::min<size_t>(sizeof b, size));
        if (b.stage != 5 || b.first < 0) return;
        ListComp& c = ctx(l);
        if (op == NOP_BIND_SAMP) {
            c.samp_binds++, st_samp_binds.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const bool tex = op == NOP_BIND_TEX;
        const int lim = tex ? 128 : 32;
        const int n = std::clamp(std::min(b.n, lim - b.first), 0, 32);
        for (int k = 0; k < n; k++) {
            (tex ? c.tex : c.img)[b.first + k] = (uint32_t)b.h[k];
            (tex ? c.tex_src : c.img_src)[b.first + k] = b.src ? b.src + 8ull * k : 0;
        }
        return;
    }
    case NOP_PUSH_CONST: {
        nvn_p_push_const u{};
        if (size < sizeof u) return;
        std::memcpy(&u, a, sizeof u);
        if (u.stage != 5 || !u.size || u.size > size - sizeof u || (uint64_t)u.offset + u.size > sizeof ListComp::push) return;
        ListComp& c = ctx(l);
        std::memcpy(c.push + u.offset, (const uint8_t*)a + sizeof u, u.size);
        c.push_n = std::max<uint32_t>(c.push_n, u.offset + u.size);
        return;
    }
    case NOP_UPDATE_UB: {
        if (!l->cctx) return;   // (only a list that binds compute state keeps them)
        nvn_p_update_ub u{};
        if (size < sizeof u) return;
        std::memcpy(&u, a, sizeof u);
        l->cctx->updates.push_back({u.dst, u.size});
        return;
    }
    default: return;
    }
}

void compute_dispatch(List* l, uint32_t op, const void* data, uint32_t size, CmdOf cmd) {
    const uint64_t t0 = __builtin_ia32_rdtsc();
    struct Timer {
        uint64_t t0;
        ~Timer() { bump(me().ticks, (int64_t)(__builtin_ia32_rdtsc() - t0)); }
    } timer{t0};
    load_functions();
    const bool indirect = op == NOP_DISPATCH_INDIRECT;
    uint32_t grid[3] = {0, 0, 0};
    uint64_t args = 0;
    if (indirect) {
        nvn_p_dispatch_indirect d{};
        std::memcpy(&d, data, std::min<size_t>(sizeof d, size));
        args = d.addr;
    } else {
        nvn_p_dispatch d{};
        std::memcpy(&d, data, std::min<size_t>(sizeof d, size));
        grid[0] = (uint32_t)std::max(0, d.x), grid[1] = (uint32_t)std::max(0, d.y), grid[2] = (uint32_t)std::max(0, d.z);
        if (!grid[0] || !grid[1] || !grid[2]) return count(l, D_EMPTY);
    }
    if (!pool_on() || !l->cctx || !l->cctx->has_prog) return count(l, D_NO_PROGRAM);
    ListComp& c = *l->cctx;
    Slot* slot = slot_find(c.prog.serial);
    if (!slot) slot = slot_make(c.prog);
    if (!slot) return count(l, D_NOT_READ);
    // (a miss at the game's start waits (plan rule 2: load time only; a load list's LUT is computed once there); later it is
    // skipped and counted)
    const bool wait = !draw_scene_up();
    const auto wait_until = clk::now() + std::chrono::seconds(120);
    const work::Class now = wait ? work::PW : work::P0;   // (before the scene, the game's load waits for it)
    // (still queued with the load's programs: read here, or waited for. A dispatch the game makes once has no later frame)
    if (slot->state.load(std::memory_order_acquire) == 0 && work::run_here(slot->scan)) st_read_here.fetch_add(1, std::memory_order_relaxed);
    while (wait && slot->state.load(std::memory_order_acquire) == 0 && clk::now() < wait_until) std::this_thread::yield();
    const int state = slot->state.load(std::memory_order_acquire);
    if (state == 0) return count(l, D_NOT_READ);
    if (state != 1) return count(l, D_NOT_COMPUTE);
    Family& fam = *slot->fam;
    VariantSet* set = fam.variants.load(std::memory_order_acquire);
    if (!set) {   // (its translation is still queued or running: needed now)
        if (fam.failed.load(std::memory_order_acquire)) return count(l, D_FAILED);
        st_misses.fetch_add(1, std::memory_order_relaxed);
        last_miss_s.store(seconds_since_device(), std::memory_order_relaxed);
        work::TicketRef t;
        {
            std::lock_guard lk(fam.mu);
            t = fam.queued;
        }
        if (t && work::bump(t, now)) st_bumped.fetch_add(1, std::memory_order_relaxed);
        if (t && wait && work::run_here(t)) st_read_here.fetch_add(1, std::memory_order_relaxed);   // (at the start: waited for all the same)
        while (wait && !(set = fam.variants.load(std::memory_order_acquire)) && !fam.failed.load(std::memory_order_acquire) && clk::now() < wait_until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!set) set = fam.variants.load(std::memory_order_acquire);
        if (!set) return count(l, fam.failed.load(std::memory_order_acquire) ? D_FAILED : D_TRANSLATING);
        st_waited.fetch_add(1, std::memory_order_relaxed);
    }
    Variant* v = pick(*set, c, fam.lay);
    if (!v) return count(l, D_VARIANT);
    if (v->skip || !v->cs) return count(l, D_VARIANT);
    if (skipped_by_dbg(v->key)) return count(l, D_VARIANT);
    VkCommandBuffer cb = cmd(l);   // (and the list's slot, whose arena the bank goes to)
    // (the uniform buffers' addresses: c0's copy in the arena, c1, the game's in its region)
    uint8_t* map;
    VkDeviceAddress bank_addr, table_addr;
    if (!draw_arena(l->slot, v->bank_bytes, map, bank_addr)) return count(l, D_ARENA);
    const size_t fix_at = c.fixups.size();
    bank_fill(c, fam.lay, grid, map, v->bank_bytes, true);
    check_bound(*v, map, v->bank_bytes);
    VkDeviceAddress entries[32];
    const size_t nu = std::min<size_t>(v->ubos.size(), 32);
    for (size_t i = 0; i < nu; i++) {
        const Variant::Ubo& u = v->ubos[i];
        if (u.index == 0) entries[i] = bank_addr;
        else if (u.index == 1) entries[i] = v->c1_addr ? v->c1_addr : draw_zero_address();
        else if (u.index == 2 || u.index >= 3 + 16) entries[i] = draw_zero_address();
        else {
            const nvn_p_bind_buf& b = c.ubo[u.index - 3];
            if (!b.addr) {
                entries[i] = draw_zero_address();
                continue;
            }
            const uint64_t bsize = std::max<uint64_t>(b.size, 16);
            if (u.bytes > bsize) bump(me().past_range);
            for (const auto& up : c.updates)
                if (up.first < b.addr + bsize && up.first + up.second > b.addr) {
                    bump(me().updated_ubo);
                    break;
                }
            if (!claims_address(b.addr, bsize, entries[i])) return c.fixups.resize(fix_at), count(l, D_UBO_MEM);
        }
    }
    for (int i = 0; i < 16; i++) {   // (the storage buffers the program reaches by address: in their claims' regions)
        VkDeviceAddress unused;
        if (c.ssbo[i].addr) claims_address(c.ssbo[i].addr, std::max<uint64_t>(c.ssbo[i].size, 16), unused);
    }
    uint8_t* tmap;
    if (!draw_arena(l->slot, std::max<uint32_t>(8, 8 * (uint32_t)nu), tmap, table_addr)) return c.fixups.resize(fix_at), count(l, D_ARENA);
    std::memcpy(tmap, entries, 8 * nu);
    VkBuffer abuf = VK_NULL_HANDLE;
    VkDeviceSize aoff = 0;
    if (indirect && ((args & 3) || !claims_span(args, 12, abuf, aoff))) return c.fixups.resize(fix_at), count(l, D_ARGS_MEM);
    uint8_t push[kTablePush + 8]{};
    {
        const float one = 1.0f;
        const VkDeviceAddress wt = draw_window_table();
        std::memcpy(push + 24, &one, 4);
        std::memcpy(push + 88, &wt, 8);
        std::memcpy(push + kTablePush, &table_addr, 8);
    }
    // (recorded)
    if (c.cb != cb) c.cb = cb, c.bound = VK_NULL_HANDLE, pool_bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE);
    if (c.bound != v->cs) {
        const VkShaderStageFlagBits st = VK_SHADER_STAGE_COMPUTE_BIT;
        vkCmdBindShadersEXT(cb, 1, &st, &v->cs);
        c.bound = v->cs;
    }
    pool_push(cb, push, sizeof push);
    const uint32_t mk = marks_on() ? mark_before(cb, indirect ? "dispatch indirect" : "dispatch", v->key, l->info.kind, l->info.lr, grid[0], grid[1]) : 0;   // 
    uint32_t ps = ~0u;
    if (pstats_on()) {   // (M6h: debug.pstats)
        PstatWhat w;
        w.what = indirect ? "dispatch indirect" : "dispatch", w.key = v->key, w.codes[0] = v->key, w.kind = l->info.kind, w.lr = l->info.lr;
        w.count = grid[0], w.instances = grid[1], w.extra = grid[2];
        ps = pstat_before(cb, w);
    }
    if (indirect) pDispatchIndirect(cb, abuf, aoff);
    else pDispatch(cb, grid[0], grid[1], grid[2]);
    l->after_dispatch = true;
    if (pstats_on()) pstat_after(cb, ps);
    if (passtime_on()) {
        PassLabel pl;
        pl.what = PT_DISPATCH, pl.kind = l->info.kind, pl.lr = l->info.lr, pl.w = grid[0], pl.h = grid[1], pl.targets = grid[2], pl.prog = v->key;
        passtime_end(cb, pl);
    }
    if (marks_on()) mark_after(cb, mk);
    draw_push_invalidate(l);   // (the push data is the command buffer's: the next draw pushes its own again)
    tell_first(*v, l, map, v->bank_bytes, grid, indirect);
    if (draw_log_frame()) trace_dispatch(*v, l, map, v->bank_bytes, grid, indirect ? args : 0);
    else if (static const uint64_t watch = [] {   // (debug.writer=<hex>: every dispatch whose storage buffers cover it)
                 const char* w = dbg_value(SET_DBG_WRITER);
                 return w ? strtoull(w, nullptr, 16) : 0ull;
             }(); watch) {
        bool hit = false;
        for (int i = 0; i < 16 && !hit; i++)
            if (c.ssbo[i].addr <= watch && watch < c.ssbo[i].addr + std::max<uint64_t>(c.ssbo[i].size, 1)) {
                logf("direct: writer: present %llu, s%d covers %#llx:", (unsigned long long)present_count(), i, (unsigned long long)watch);
                trace_dispatch(*v, l, map, v->bank_bytes, grid, indirect ? args : 0);
                hit = true;
            }
        // (M4g: and the storage images whose texture is at that address: a texture written through another one over its memory)
        char want[40];
        std::snprintf(want, sizeof want, "texture %#llx ", (unsigned long long)watch);
        for (size_t j = 0; j < v->info.image_descriptors.size() && !hit; j++) {
            const auto& im = v->info.image_descriptors[j];
            const uint32_t h = im.cbuf_index == 0 ? word_of(map, v->bank_bytes, im.cbuf_offset) : 0;
            char e[320];
            pool_describe(h & 0xFFFFF, e, sizeof e);
            if (std::strstr(e, want)) {
                logf("direct: writer: present %llu, image %zu is %#llx:", (unsigned long long)present_count(), j, (unsigned long long)watch);
                trace_dispatch(*v, l, map, v->bank_bytes, grid, indirect ? args : 0);
                hit = true;
            }
        }
    }
    count(l, D_DRAWN);
    if (indirect) count(l, D_INDIRECT);
}

void compute_buffer_op(List* l, uint32_t op, const void* data, uint32_t size, CmdOf cmd) {
    load_functions();
    if (op == NOP_COPY_B2B) {
        nvn_p_copy_bb p{};
        std::memcpy(&p, data, std::min<size_t>(sizeof p, size));
        VkBuffer sb, db;
        VkDeviceSize so, dof;
        if (const char* w = dbg_value(SET_DBG_WRITER)) {   // (debug.writer=<hex>: buffer copies over it)
            const uint64_t at = strtoull(w, nullptr, 16);
            if (p.dst <= at && at < p.dst + p.size)
                logf("direct: writer: present %llu, copy %#llx -> %#llx (%#llx bytes) in a list of kind %u (lr %#llx)",
                     (unsigned long long)present_count(), (unsigned long long)p.src, (unsigned long long)p.dst, (unsigned long long)p.size,
                     l->info.kind, (unsigned long long)l->info.lr);
        }
        if (!p.size || !claims_span(p.src, p.size, sb, so) || !claims_span(p.dst, p.size, db, dof)) {
            if (p.size)
                strict_event("buffer copies skipped (not in a region)", "copy %#llx -> %#llx (%#llx bytes), list kind %u lr %#llx",
                             (unsigned long long)p.src, (unsigned long long)p.dst, (unsigned long long)p.size, l->info.kind,
                             (unsigned long long)l->info.lr);
            return bump(me().buf_ops[1]);
        }
        VkCommandBuffer cb = cmd(l);
        const VkBufferCopy r{so, dof, p.size};
        pCopyBuffer(cb, sb, db, 1, &r);
        l->wait |= kAfterTransfer;
        return bump(me().buf_ops[0]);
    }
    nvn_p_clear_buf p{};
    std::memcpy(&p, data, std::min<size_t>(sizeof p, size));
    VkBuffer db;
    VkDeviceSize dof;
    if (!p.size || (p.dst & 3) || (p.size & 3) || !claims_span(p.dst, p.size, db, dof)) {
        if (p.size)
            strict_event("buffer clears skipped (unaligned or not in a region)", "clear %#llx (%#llx bytes), list kind %u lr %#llx",
                         (unsigned long long)p.dst, (unsigned long long)p.size, l->info.kind, (unsigned long long)l->info.lr);
        return bump(me().buf_ops[3]);
    }
    VkCommandBuffer cb = cmd(l);
    pFillBuffer(cb, db, dof, p.size, p.value);
    l->wait |= kAfterTransfer;
    bump(me().buf_ops[2]);
}

void compute_invalidate(List* l) {
    if (l->cctx) l->cctx->bound = VK_NULL_HANDLE;
}

void compute_list_free(List* l) {
    delete l->cctx;
    l->cctx = nullptr;
}

void compute_list_submitted(const List* l) {
    std::vector<const List*> work{l};
    while (!work.empty()) {
        const List* x = work.back();
        work.pop_back();
        for (const FrameOp& o : x->ops)
            if (o.op == NOP_CALL && o.callee) work.push_back(o.callee);
        if (!x->cctx || x->cctx->fixups.empty()) continue;
        for (const ListComp::Fixup& f : x->cctx->fixups) {
            uint64_t h = 0;
            host->gpu_read(f.src, &h, 8);
            const uint32_t lo = (uint32_t)h;
            std::memcpy(f.dst, &lo, 4);
        }
        st_fixups.fetch_add((int64_t)x->cctx->fixups.size(), std::memory_order_relaxed);
    }
}

void compute_program_set(const nvn_prog* p) {
    if (!p || !pool_on() || !p->nstage) return;
    nvn_prog copy;
    std::memcpy(&copy, p, sizeof copy);
    load_functions();
    slot_make(copy);
}

int64_t compute_objects_made() { return st_cs_objects.load(std::memory_order_relaxed); }

PreResult compute_precompile(const uint8_t* ctl, const uint8_t* area, uint32_t area_n) {
    load_functions();
    const std::vector<uint8_t> bytes(area, area + area_n);
    Code code;
    if (!parse_code(ctl, bytes, code)) return PRE_FAILED;
    bool kept = false;
    VariantSet* set = make_variants(code, code.lay, &kept);
    if (!set) return PRE_FAILED;
    for (Variant* v : set->by_mask) {   // (made for their binaries: destroyed again)
        if (v->cs) vkDestroyShaderEXT(dev.device, v->cs, nullptr);
        delete v;
    }
    delete set;
    return kept ? PRE_KEPT : PRE_MADE;
}

uint64_t compute_cache_stamp() {
    load_functions();
    return disk_stamp();
}

void compute_cache_index() {
    disk_init();
    disk.index_now();
}

void compute_log(double frames) {
    if (const char* pk = dbg_value(SET_DBG_PEEK)) {   // (debug.peek=<guest address>: 64 KB there, nonzero bytes and a checksum)
        const uint64_t at = strtoull(pk, nullptr, 16);
        static std::vector<uint8_t> b(64 << 10);
        const bool ok = host->gpu_read(at, b.data(), b.size()) != 0;
        uint64_t nz = 0, sum = 0;
        for (uint8_t x : b) nz += x != 0, sum = sum * 31 + x;
        logf("direct: peek %#llx: %s %llu of %zu bytes nonzero, sum %016llx", (unsigned long long)at, ok ? "read," : "unreadable,",
             (unsigned long long)nz, b.size(), (unsigned long long)sum);
    }
    static char out[4096];
    const size_t cap = sizeof out;
    static const char* const kinds[GPU_LIST_KINDS] = {"unclassified", "load", "job", "layer", "gate", "compute", "head", "immediate"};
    int k = std::snprintf(out, cap, "dispatches a frame (%.3f ms recording them, all threads; uniform buffers a program may read past their "
                          "bound range %.1f, compute uniform buffers the list updates inline %.1f; buffer copies %.1f (%.1f skipped), clears %.1f "
                          "(%.1f skipped))",
                          take(&TStats::ticks, 0) / ticks_per_ns() / 1e6 / frames, take(&TStats::past_range, 1) / frames,
                          take(&TStats::updated_ubo, 2) / frames, take_buf(0) / frames, take_buf(1) / frames, take_buf(2) / frames, take_buf(3) / frames);
    for (int kind = 0; kind < GPU_LIST_KINDS; kind++) {
        bool any = false;
        for (int r = 0; r < D_COUNT; r++) {
            const int64_t n = take_kind(kind, r);
            if (!n) continue;
            k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "%s %s: %s %.2f", any ? "," : ";", any ? "" : kinds[kind], kReasons[r], n / frames);
            any = true;
        }
    }
    logf("direct: %s", out);
    const size_t queued = work::queued();
    logf("direct: compute programs (since the start): %lld slots, %lld codes, %lld variants (%lld translated in %.1f s, %lld from the cache, "
         "%lld failed), shader objects %lld (%.1f s, %lld failed; %lld from their binaries, %lld binaries refused), %lld with matrix multiplies, %lld reading the grid otherwise; bound textures that contradict the translation %lld; misses %.1f "
         "a frame, %zu jobs waiting, the last %.2f s after the device; deferred words copied %.1f a frame; separate compute samplers bound %lld; dispatches that waited for their variant at the start %lld; made when set %lld; moved ahead of the load's work %lld; programs read at their first dispatch %lld",
         (long long)st_slots.load(), (long long)st_codes.load(), (long long)st_variants.load(), (long long)st_translated.load(),
         st_translate_ns.load() / 1e9, (long long)st_from_disk.load(), (long long)st_failed.load(), (long long)st_cs_objects.load(),
         st_object_ns.load() / 1e9, (long long)st_object_failed.load(), (long long)st_from_binary.load(), (long long)st_binary_failed.load(), (long long)st_coopmat.load(), (long long)st_grid_left.load(), (long long)st_contradicted.load(),
         st_misses.exchange(0) / frames, queued, last_miss_s.load(), st_fixups.exchange(0) / frames, (long long)st_samp_binds.load(), (long long)st_waited.load(), (long long)st_prefetched.load(),
         (long long)st_bumped.load(), (long long)st_read_here.load());
}

}  // namespace hsd
