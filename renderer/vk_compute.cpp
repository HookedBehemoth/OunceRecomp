// Dispatches and the buffer ops (vk_compute.h). The compute half of gpu/legacy/vk/vk_shader.cpp translate_into, Env and
// vk_bank.cpp's compute bank (cs_layout, driver_bank_fill's stage 5), on the step-B interface of the graphics programs
// (vk_shader.cpp): handles read by the shader from c0, storage buffers and global memory by address, uniform buffers
// through an address table. What differs from the frozen back end: the grid is NumWorkgroups (no CPU read of an indirect
// dispatch's counts, no copy into c0), the 0x880 pointer table is not made (global memory is translated by address), and
// the networks run as the game has them (the matrix multiplies in software on this card): no stand-ins (plan rule 8).
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
#include <unordered_set>
#include <vector>

#include "vk_marks.h"
#include "os.h"
#include "vk_pstats.h"
#include "vk_check.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

#include "vk_cmd.h"
#include "vk_compute.h"
#include "vk_draw.h"
#include "vk_heap.h"
#include "vk_shader.h"
#include "vk_shader_parts.h"
#include "vk_work.h"

namespace Shader::Optimization {   // (collect_shader_info_pass.cpp: NVN2 compute's storage buffer table, this thread's program)
extern thread_local u32 g_nvn2_compute_ssbo_base;
extern thread_local u32 g_nvn2_compute_ssbo_count;
extern thread_local u32 g_nvn2_compute_layout_end;
}  // namespace Shader::Optimization

#ifndef OUNCE_RENDERER_COMPUTE_STAMP
#define OUNCE_RENDERER_COMPUTE_STAMP 0ull
#endif

namespace hsd {
bool layer0_address(uint64_t gpu, uint64_t size, VkDeviceAddress& a);   // (vk_pools.cpp)
bool layer0_bound(uint64_t lo, uint64_t hi);   // (vk_pools.cpp)
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
    "variant being translated", "variant does not translate", "variant cannot be dispatched", "a uniform buffer not in layer 0",
    "the frame arena full", "indirect arguments not in layer 0", "empty grid"};
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

// ---- programs: variants per code key, slots per NVN serial ----
struct Answer {
    uint16_t offset;    // (the handle's c0 word)
    uint8_t want, dim_fixed, desc_type;
    bool depth_float;
};
struct Variant {
    uint64_t key = 0, variant = 0;
    // (translated: kept in the cache file)
    std::vector<uint32_t> spirv;
    std::vector<uint8_t> c1;
    Shader::Info info;
    uint32_t wg[3]{1, 1, 1}, shared = 0;
    std::vector<Answer> answers;
    // (made)
    VkShaderEXT cs = VK_NULL_HANDLE;
    VkDeviceAddress c1_addr = 0;
    uint32_t bank_bytes = 16;
    struct Ubo {
        uint8_t index;
        uint32_t bytes;
    };
    std::vector<Ubo> ubos;
    const char* skip = nullptr;
    bool told = false;   // (its first dispatch logged)
    uint32_t lay_ssbo_at = 0, lay_ssbo = 0;   // (its layout's storage buffer table, for that log)
    Variant* next = nullptr;
};
struct Family {   // (one compute code: every program slot with it shares its variants)
    uint64_t key = 0;
    Layout lay;
    nvn_stage_data sd{};
    std::atomic<Variant*> variants{nullptr};
    std::atomic<bool> loaded{false};
    std::mutex mu;   // (pending, failed, queued, waiting, and loaded's change)
    std::unordered_set<uint64_t> pending, failed;
    std::unordered_map<uint64_t, work::TicketRef> queued;   // (pending translations' jobs, to bump one a dispatch needs)
    std::vector<std::atomic<int>*> waiting;   // (other slots of the code, made ready when its kept variants are)
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

// ---- the cache file (ounce_renderer_compute.bin: records by code key) ----
constexpr uint32_t kDiskMagic = 0x43445348;   // ('HSDC')
constexpr uint64_t kDiskVersion = 2;   // (the record's layout, disk_save: a change to it changes this)
std::mutex disk_mu;
FILE* disk_out = nullptr;
FILE* disk_in = nullptr;
bool disk_on = false;
struct DiskRecord {
    int64_t at;
    size_t size;
};
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_by_key;
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
        const std::string path = parts::program_dir() + "ounce_renderer_compute.bin";
        os_file* lock = os_file_open((path + ".lock").c_str(), OS_READ | OS_WRITE | OS_CREATE, nullptr);   // (held for the process's life)
        if (!lock || !os_file_trylock(lock)) {
            os_file_close(lock);
            return logf("direct: compute cache %s: in use by another game: none", path.c_str());
        }
        const uint64_t stamp = disk_stamp();
        int64_t good = 0, whole = 0;
        size_t records = 0;
        if (FILE* f = fopen(path.c_str(), "rb")) {
            fseeko(f, 0, SEEK_END);
            whole = ftello(f);
            fseeko(f, 0, SEEK_SET);
            uint32_t head[2]{};
            uint64_t kept = 0;
            if (whole >= 16 && fread(head, 4, 2, f) == 2 && fread(&kept, 8, 1, f) == 1 && head[0] == kDiskMagic && kept == stamp) {
                good = 16;
                while (good + 24 <= whole) {
                    uint64_t rec[2];
                    fseeko(f, good, SEEK_SET);
                    if (fread(rec, 8, 2, f) != 2 || rec[1] < 8 || rec[1] > (uint64_t)(whole - good - 16)) break;
                    disk_by_key[rec[0]].push_back({good + 16, (size_t)rec[1]});
                    good += 16 + (int64_t)rec[1];
                    records++;
                }
            }
            fclose(f);
        }
        std::error_code ec;
        if (good && good != whole && (std::filesystem::resize_file(path, (uintmax_t)good, ec), ec)) good = 0;
        if (good) {
            disk_out = fopen(path.c_str(), "ab");
        } else {
            disk_by_key.clear(), records = 0;
            if ((disk_out = fopen(path.c_str(), "wb"))) {
                const uint32_t h[2] = {kDiskMagic, 0};
                fwrite(h, 4, 2, disk_out), fwrite(&stamp, 8, 1, disk_out), fflush(disk_out);
            }
        }
        disk_in = disk_out ? fopen(path.c_str(), "rb") : nullptr;
        disk_on = disk_out && disk_in;
        logf("direct: compute cache %s: %zu variants kept%s", path.c_str(), records, whole && !good ? "; another stamp's file was truncated" : "");
        // (the compute objects' binaries are kept with the graphics ones, by create info: an earlier separate file is removed)
        os_remove((parts::program_dir() + "ounce_renderer_compute_objects.bin").c_str());
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
    if (!disk_on) return;
    std::vector<uint8_t> o;
    put(o, (uint64_t)v.answers.size());
    for (const Answer& a : v.answers) put(o, a);
    put(o, v.wg), put(o, v.shared);
    put(o, (uint64_t)v.spirv.size());
    o.insert(o.end(), (const uint8_t*)v.spirv.data(), (const uint8_t*)(v.spirv.data() + v.spirv.size()));
    put(o, (uint64_t)v.c1.size());
    o.insert(o.end(), v.c1.begin(), v.c1.end());
    parts::info_put(o, v.info);
    const uint64_t head[2] = {v.key, o.size()};
    std::lock_guard lk(disk_mu);
    fseeko(disk_out, 0, SEEK_END);
    fwrite(head, 8, 2, disk_out), fwrite(o.data(), 1, o.size(), disk_out), fflush(disk_out);
}
bool disk_read(const DiskRecord& d, Variant& v) {
    std::vector<uint8_t> data(d.size);
    {
        std::lock_guard lk(disk_mu);
        fseeko(disk_in, d.at, SEEK_SET);
        if (fread(data.data(), 1, data.size(), disk_in) != data.size()) return false;
    }
    const uint8_t* p = data.data();
    const uint8_t* e = p + data.size();
    uint64_t n = 0;
    if (!get(p, e, n) || n > 4096) return false;
    v.answers.resize(n);
    for (Answer& a : v.answers)
        if (!get(p, e, a)) return false;
    if (!get(p, e, v.wg) || !get(p, e, v.shared) || !get(p, e, n) || n > (uint64_t)(e - p) / 4 || !n) return false;
    v.spirv.resize(n);
    std::memcpy(v.spirv.data(), p, n * 4), p += n * 4;
    if (!get(p, e, n) || n > (uint64_t)(e - p)) return false;
    v.c1.assign(p, p + n), p += n;
    return parts::info_get(p, e, v.info);
}

// <stamp compute>
// ---- the translator's questions (vk_shader.cpp Env, compute: handles in c0, the grid folded into NumWorkgroups) ----
struct Code {
    uint64_t key = 0;
    std::vector<uint64_t> code;
    std::vector<uint8_t> c1;
    uint32_t texture_bound = ~0u, local[3]{}, shared = 0;
    Layout lay;
};
/// The compute stage lowered to Maxwell (vk_shader.cpp parse_code; the matrix multiplies kept whole: HMMA_nx2, which the SPIR-V
/// back end runs in software here)
bool lower(const nvn_stage_data& sd, Code& c) {
    uint8_t ctl[kControlSize];
    std::vector<uint8_t> area;
    if (!parts::read_stage(sd, ctl, area)) return false;
    uint32_t len, c1o;
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4), std::memcpy(&c1o, ctl + 0x40, 4), std::memcpy(&st, ctl + 0x56, 2);
    if (st != 5) return false;
    c.key = hash_bytes(area.data(), area.size(), hash_bytes(ctl, kControlSize));
    using Shader::Ampere::Instruction;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    for (uint32_t i = 0; (i + 1) * sizeof(Instruction) <= len; i++) {
        Instruction& insn = native.emplace_back();
        std::memcpy(&insn, area.data() + i * sizeof(Instruction), sizeof insn);
        const auto r = capture.Observe(insn, i);
        if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) return false;
        if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    Shader::Ampere::native_matrix_multiply = true;
    const auto lowered = Shader::Ampere::LowerToMaxwell(native);
    Shader::Ampere::native_matrix_multiply = false;
    if (!lowered) {
        static std::atomic<int> told{0};
        if (told++ < 40)
            logf("direct: compute program %016llx does not lower at +%#x: %s", (unsigned long long)c.key, lowered.error_offset,
                 std::string(lowered.error).c_str());
        return false;
    }
    c.texture_bound = lowered.required_texture_buffer;
    c.code.assign(sizeof(Shader::ProgramHeader) / 8, 0);
    for (const uint64_t instruction : lowered.instructions) {
        if (c.code.size() * 8 % 32 == 0) c.code.push_back(0);   // (the Maxwell scheduling word)
        c.code.push_back(instruction);
    }
    c.code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(c.code, lowered.source_instruction_offsets);
    c.c1.assign(area.begin() + c1o, area.end());
    std::memcpy(c.local, ctl + 0x58, 12);
    std::memcpy(&c.shared, ctl + 0x64, 4);
    c.lay = layout_of(ctl);
    return true;
}

class Env final : public Shader::Environment {
public:
    Env(const Code& c, const uint8_t* bank, uint32_t bank_n) : nvn{c}, c0{bank}, c0_n{bank_n} {
        stage = Shader::Stage::Compute;
        start_address = 0;
        is_proprietary_driver = true;
    }
    u64 ReadInstruction(u32 address) override { return address / 8 < nvn.code.size() ? nvn.code[address / 8] : 0; }
    u32 TranslateProgramAddress(u32 address) const override {
        if (const auto t = Shader::Ampere::NativeProgramAddress(nvn.code, address)) return *t;
        throw Shader::RuntimeError("indirect branch to native address {:#x}, not in the program", address);
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        uint32_t v = 0;
        if (index == 1 && (size_t)offset + 4 <= nvn.c1.size()) std::memcpy(&v, nvn.c1.data() + offset, 4);
        if (index == 0 && offset + 4 <= c0_n && !(offset & 3)) std::memcpy(&v, c0 + offset, 4);
        if (index >= 3) game_words++;   // (step B: a game buffer's word is not read)
        return v;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        questions++;
        return handle ? (Shader::TextureType)(parts::answer_of(handle) & 0x7f) : Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return Shader::TexturePixelFormat{}; }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        questions++;
        return handle && (parts::answer_of(handle) & 0x80);
    }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return nvn.texture_bound != ~0u ? nvn.texture_bound : 0; }   // (NVN2 compute: c0)
    u32 LocalMemorySize() const override { return 0; }
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
    int questions = 0, game_words = 0;

private:
    const Code& nvn;
    const uint8_t* c0;
    uint32_t c0_n;
};

struct Translation {
    Shader::ObjectPool<Shader::IR::Inst> insts;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
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

bool translate_variant(Variant& v, const Code& c, const uint8_t* bank, uint32_t bank_n) {
    try {
        Env env{c, bank, bank_n};
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
        Shader::Maxwell::Flow::CFG cfg{env, t->flow_blocks, static_cast<u32>(sizeof(Shader::ProgramHeader)), false};
        t->program = Shader::Maxwell::TranslateProgram(t->insts, t->blocks, env, cfg, parts::translate_info());
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
        if (env.questions) {   // (the answers its textures were translated with: c0 words, as a dispatch checks them)
            for (const auto& d : v.info.texture_descriptors) {
                if (d.count != 1 || d.handle_program != ~0u || d.has_secondary || d.cbuf_index != 0 || d.cbuf_offset + 4 > bank_n) continue;
                uint32_t h;
                std::memcpy(&h, bank + d.cbuf_offset, 4);
                Answer a{};
                a.offset = (uint16_t)d.cbuf_offset, a.dim_fixed = d.type_fixed, a.desc_type = (uint8_t)d.type;
                a.depth_float = d.is_depth && !d.is_integer;
                const uint8_t raw = h ? parts::answer_of(h) : (uint8_t)((uint8_t)d.type | (d.is_integer ? 0x80 : 0));
                a.want = parts::answer_canonical(raw, a.dim_fixed, a.desc_type, a.depth_float);
                v.answers.push_back(a);
            }
        }
        return true;
    } catch (const std::exception& e) {
        static std::atomic<int> told{0};
        if (told++ < 30) logf("direct: compute program %016llx not translated: %s", (unsigned long long)c.key, e.what());
        return false;
    }
}

// </stamp compute>

// ---- the heap mapping and the shader object (vk_shader.cpp finish's, one compute stage) ----
VkImageViewType view_type(Shader::TextureType t) {
    switch (t) {
    case Shader::TextureType::Color1D: return VK_IMAGE_VIEW_TYPE_1D;
    case Shader::TextureType::ColorArray1D: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case Shader::TextureType::ColorArray2D: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case Shader::TextureType::Color3D: return VK_IMAGE_VIEW_TYPE_3D;
    case Shader::TextureType::ColorCube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case Shader::TextureType::ColorArrayCube: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}
bool handle_is_storage_pointer(const Shader::Info& info, const Shader::TextureDescriptor& d) {
    if (d.handle_program != ~0U) return false;
    for (const auto& sb : info.storage_buffers_descriptors) {
        if (sb.cbuf_index != d.cbuf_index) continue;
        for (uint32_t j = 0; j < sb.count; j++)
            if (d.cbuf_offset >= sb.cbuf_offset + j * 16 && d.cbuf_offset < sb.cbuf_offset + j * 16 + 8) return true;
    }
    return false;
}
VkDescriptorSetAndBindingMappingEXT map_of(uint32_t b, VkSpirvResourceTypeFlagsEXT mask, VkDescriptorMappingSourceEXT src) {
    VkDescriptorSetAndBindingMappingEXT m{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT};
    m.descriptorSet = 0, m.firstBinding = b, m.bindingCount = 1, m.resourceMask = mask, m.source = src;
    return m;
}
VkDescriptorSetAndBindingMappingEXT null_map(uint32_t b, uint32_t stride) {
    VkDescriptorSetAndBindingMappingEXT m = map_of(b, VK_SPIRV_RESOURCE_TYPE_ALL_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT);
    m.sourceData.constantOffset.heapOffset = heap_tail_offset();
    m.sourceData.constantOffset.heapArrayStride = stride;
    m.sourceData.constantOffset.samplerHeapOffset = 0;
    m.sourceData.constantOffset.samplerHeapArrayStride = kSamplerDesc;
    return m;
}

bool finish(Variant& v, const Layout& lay) {
    const Shader::Info& info = v.info;
    std::vector<VkDescriptorSetAndBindingMappingEXT> m;
    uint32_t binding = 0;
    for (const auto& c : info.constant_buffer_descriptors) {
        if (c.count != 1) {
            v.skip = v.skip ? v.skip : "a uniform buffer array";
            m.push_back(null_map(binding, 16));
            binding += c.count;
            continue;
        }
        VkDescriptorSetAndBindingMappingEXT x = map_of(binding, VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_INDIRECT_ADDRESS_EXT);
        x.sourceData.indirectAddress.pushOffset = kTablePush;
        x.sourceData.indirectAddress.addressOffset = 8 * (uint32_t)v.ubos.size();
        m.push_back(x);
        const uint32_t used = c.index < info.constant_buffer_used_sizes.size() ? info.constant_buffer_used_sizes[c.index] : 0;
        v.ubos.push_back({(uint8_t)c.index, used});
        binding += c.count;
    }
    if (!info.storage_buffers_descriptors.empty() && !info.ssbo_bda) v.skip = v.skip ? v.skip : "storage buffers not by address";
    for (const auto& s : info.storage_buffers_descriptors) {
        m.push_back(null_map(binding, 16));
        binding += s.count > 1 ? 1 : s.count;
    }
    for (size_t k = 0; k < info.texture_buffer_descriptors.size(); k++) m.push_back(null_map(binding++, kImageDesc)), v.skip = v.skip ? v.skip : "a texel buffer";
    for (size_t k = 0; k < info.image_buffer_descriptors.size(); k++) m.push_back(null_map(binding++, kImageDesc)), v.skip = v.skip ? v.skip : "an image buffer";
    for (const auto& t : info.texture_descriptors) {
        VkDescriptorSetAndBindingMappingEXT x = map_of(binding++, VK_SPIRV_RESOURCE_TYPE_ALL_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT);
        auto& co = x.sourceData.constantOffset;
        if (handle_is_storage_pointer(info, t)) co.heapOffset = heap_tail_offset(), co.heapArrayStride = 0;
        else co.heapOffset = heap_region_offset(std::min<uint32_t>((uint32_t)view_type(t.type), kHeapRegions - 1)), co.heapArrayStride = kImageDesc;
        m.push_back(x);
    }
    if (!info.texture_descriptors.empty())
        for (const bool cmp : {false, true}) {
            VkDescriptorSetAndBindingMappingEXT x = map_of(cmp ? Shader::Backend::SPIRV::kHeapCompareSamplerBinding : Shader::Backend::SPIRV::kHeapSamplerBinding,
                                                           VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT);
            auto& co = x.sourceData.constantOffset;
            co.heapOffset = heap_sampler_offset(cmp), co.heapArrayStride = kSamplerDesc;
            co.samplerHeapOffset = 0, co.samplerHeapArrayStride = 0;
            m.push_back(x);
        }
    for (const auto& im : info.image_descriptors) {
        VkDescriptorSetAndBindingMappingEXT x = map_of(binding++, VK_SPIRV_RESOURCE_TYPE_ALL_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT);
        x.sourceData.constantOffset.heapOffset = heap_storage_offset(std::min<uint32_t>((uint32_t)view_type(im.type), kHeapRegions - 1));
        x.sourceData.constantOffset.heapArrayStride = kImageDesc;
        m.push_back(x);
    }
    {   // (c0's copy: what the program reads of it, and the layout's tables at least)
        const uint32_t used = info.constant_buffer_used_sizes.empty() ? 0 : info.constant_buffer_used_sizes[0];
        v.bank_bytes = std::clamp<uint32_t>((std::max(used, lay.end()) + 15) & ~15u, 16, kBankMax);
    }
    v.lay_ssbo_at = lay.ssbo_at(), v.lay_ssbo = lay.ssbo;
    v.c1_addr = parts::put_c1(v.c1, info.constant_buffer_used_sizes.size() > 1 ? info.constant_buffer_used_sizes[1] : 0);
    if (kTablePush + 8 > caps.max_push) v.skip = v.skip ? v.skip : "push data over the device's limit";
    VkShaderDescriptorSetAndBindingMappingInfoEXT mi{VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT};
    mi.mappingCount = (uint32_t)m.size(), mi.pMappings = m.data();
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sz{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    sz.requiredSubgroupSize = 32;
    load_functions();
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    ci.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
    ci.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    ci.codeSize = v.spirv.size() * 4;
    ci.pCode = v.spirv.data();
    ci.pName = "main";
    if (caps.sg32_stages & VK_SHADER_STAGE_COMPUTE_BIT) sz.pNext = &mi, ci.pNext = &sz;
    else ci.pNext = &mi;
    const auto t0 = clk::now();
    const uint64_t id = parts::object_key(ci, mi, (caps.sg32_stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0);
    VkResult r = VK_ERROR_UNKNOWN;
    bool from_binary = false;
    {   // (its binary, when this driver made it before from the same create info: no compile)
        const std::vector<uint8_t> bin = parts::object_binary(id);
        if (!bin.empty()) {
            VkShaderCreateInfoEXT bi = ci;
            bi.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT, bi.codeSize = bin.size(), bi.pCode = bin.data();
            r = vkCreateShadersEXT(dev.device, 1, &bi, nullptr, &v.cs);
            from_binary = r == VK_SUCCESS;
            if (!from_binary) v.cs = VK_NULL_HANDLE, st_binary_failed.fetch_add(1, std::memory_order_relaxed);
        }
    }
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
    else parts::object_keep(id, v.cs);
    if (v.skip) logf("direct: compute program %016llx cannot be dispatched: %s (skipped, counted)", (unsigned long long)v.key, v.skip);
    return true;
}

void publish(Family& f, Variant* v) {
    v->variant = 0;
    for (const Answer& a : v->answers) v->variant = (v->variant ^ ((uint64_t)a.offset << 16 | a.want)) * 1099511628211ull;
    Variant* head = f.variants.load(std::memory_order_acquire);
    do v->next = head;
    while (!f.variants.compare_exchange_weak(head, v, std::memory_order_release, std::memory_order_acquire));
    st_variants.fetch_add(1, std::memory_order_relaxed);
    mem_account_main(MO_M_SHADERS, (int64_t)(v->spirv.capacity() * 4));
}

/// One variant of f translated on a worker for these c0 bytes (want: its key in f's pending set, put there by the caller)
void start_variant(Family* f, std::shared_ptr<std::vector<uint8_t>> copy, uint64_t want, work::Class cls) {
    work::TicketRef t = work::make([f, copy, want] {
        const auto t1 = clk::now();
        Code code;
        auto nv = std::make_unique<Variant>();
        nv->key = f->key;
        bool ok = lower(f->sd, code) && code.key == f->key && translate_variant(*nv, code, copy->data(), (uint32_t)copy->size());
        if (ok) disk_save(*nv), ok = finish(*nv, f->lay);
        st_translate_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t1).count(), std::memory_order_relaxed);
        (ok ? st_translated : st_failed).fetch_add(1, std::memory_order_relaxed);
        if (ok) publish(*f, nv.release());
        std::lock_guard lk(f->mu);
        f->pending.erase(want), f->queued.erase(want);
        if (!ok) f->failed.insert(want);
    });
    {
        std::lock_guard lk(f->mu);
        f->queued[want] = t;
    }
    work::push(t, cls);
}

/// (a worker) a program's stages read: its compute stage's code key, its family (whose kept variants the first slot makes)
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
    if (!fresh) {   // (the code's kept variants are made by the slot that saw it first: this one is ready when they are)
        std::lock_guard lk(s->fam->mu);
        if (!s->fam->loaded.load(std::memory_order_acquire)) return s->fam->waiting.push_back(&s->state);
        return s->state.store(1, std::memory_order_release);
    }
    disk_init();
    std::vector<DiskRecord> recs;
    {
        std::lock_guard lk(disk_mu);
        if (const auto it = disk_by_key.find(key); it != disk_by_key.end()) recs = it->second;
    }
    for (const DiskRecord& d : recs) {
        auto v = std::make_unique<Variant>();
        v->key = key;
        if (!disk_read(d, *v) || !finish(*v, s->fam->lay)) continue;
        st_from_disk.fetch_add(1, std::memory_order_relaxed);
        publish(*s->fam, v.release());
    }
    {
        std::lock_guard lk(s->fam->mu);
        s->fam->loaded.store(true, std::memory_order_release);
        for (std::atomic<int>* w : s->fam->waiting) w->store(1, std::memory_order_release);
        s->fam->waiting.clear();
    }
    s->state.store(1, std::memory_order_release);
    // A program with no texture or image handles has one variant, whatever its c0 holds: with none kept it is translated
    // now, when the game sets it at the load, so the load's programs are made side by side instead of one after another
    // as their dispatches come
    if (recs.empty() && !s->fam->lay.tex && !s->fam->lay.img) {
        Family* f = s->fam;
        {
            std::lock_guard lk(f->mu);
            if (!f->pending.insert(f->key).second) return;
        }
        st_prefetched.fetch_add(1, std::memory_order_relaxed);
        const uint32_t n = std::clamp<uint32_t>((f->lay.end() + 15) & ~15u, 16, kBankMax);
        start_variant(f, std::make_shared<std::vector<uint8_t>>(n, 0), f->key, work::P1);
    }
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
uint32_t word_of(const uint8_t* bank, uint32_t n, uint32_t off) {
    uint32_t v = 0;
    if (off + 4 <= n) std::memcpy(&v, bank + off, 4);
    return v;
}
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

/// HOS_DBG=skipcs:<key prefix>[+<prefix>...]: those programs' dispatches left out (a hang's diagnosis)
bool skipped_by_dbg(uint64_t key) {
    static const std::string list = [] {
        const char* v = dbg_value("skipcs");
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
    static const bool log_compute = dbg_log("logcompute");
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
        heap_describe(h & 0xFFFFF, e, sizeof e);
        k += std::snprintf(b + k, sizeof b - k, " [img c%u+%#x %s%s %s]", im.cbuf_index, im.cbuf_offset,
                           im.format == Shader::ImageFormat::Typeless ? "formatted" : "raw", im.is_written ? " written" : "", e);
    }
    logf("%s", b);
}

/// (HOS_DBG=drawlog:<present>, the per-draw trace's dispatch lines) every dispatch of that frame: program, grid or
/// arguments, the storage and uniform buffers, each texture and image as its handle names it (heap_describe)
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
                                       layer0_bound(w[j], w[j] + 16) ? "" : " not bound in layer 0");
                }
            }
        }
    for (int i = 0; i < 16 && k < 4000; i++)
        if (c.ubo[i].addr) k += std::snprintf(b + k, sizeof b - k, " u%d=%#llx+%#llx", i, (unsigned long long)c.ubo[i].addr, (unsigned long long)c.ubo[i].size);
    for (const auto& t : v.info.texture_descriptors) {
        if (k > 5000) break;
        const uint32_t h = t.cbuf_index == 0 ? word_of(bank, n, t.cbuf_offset) : 0;
        char e[320];
        heap_describe(h & 0xFFFFF, e, sizeof e);
        k += std::snprintf(b + k, sizeof b - k, " tex c%u+%#x=%#x(%s)", t.cbuf_index, t.cbuf_offset, h, e);
    }
    for (const auto& im : v.info.image_descriptors) {
        if (k > 5600) break;
        const uint32_t h = im.cbuf_index == 0 ? word_of(bank, n, im.cbuf_offset) : 0;
        char e[320];
        heap_describe(h & 0xFFFFF, e, sizeof e);
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
    if (!heap_on() || !l->cctx || !l->cctx->has_prog) return count(l, D_NO_PROGRAM);
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
    // (c0 as this dispatch has it: the answers are read from it, then it goes to the arena)
    uint8_t bank[kBankMax];
    const uint32_t lay_n = std::clamp<uint32_t>((fam.lay.end() + 15) & ~15u, 16, kBankMax);
    bank_fill(c, fam.lay, grid, bank, lay_n, false);
    auto pick = [&]() -> Variant* {
        for (Variant* x = fam.variants.load(std::memory_order_acquire); x; x = x->next) {
            bool hold = true;
            for (const Answer& a : x->answers) {
                const uint32_t h = word_of(bank, lay_n, a.offset);
                if (h && parts::answer_canonical(parts::answer_of(h), a.dim_fixed, a.desc_type, a.depth_float) != a.want) hold = false;
            }
            if (hold) return x;
        }
        return nullptr;
    };
    Variant* v = pick();
    if (!v) {   // (translated on a worker, once for these answers)
        uint64_t want = fam.key;
        for (uint32_t off = fam.lay.tex_at(); off + 4 <= lay_n && off < fam.lay.img_at(); off += 8)
            if (const uint32_t h = word_of(bank, lay_n, off)) want = (want ^ ((uint64_t)off << 8 | parts::answer_of(h))) * 1099511628211ull;
        st_misses.fetch_add(1, std::memory_order_relaxed);
        last_miss_s.store(seconds_since_device(), std::memory_order_relaxed);
        bool mine;
        {
            std::lock_guard lk(fam.mu);
            if (fam.failed.count(want)) return count(l, D_FAILED);
            mine = fam.pending.insert(want).second;
            // (one still queued with the load's work, a set-time translation among them: needed now)
            if (const auto it = fam.queued.find(want); it != fam.queued.end() && work::bump(it->second, now)) st_bumped.fetch_add(1, std::memory_order_relaxed);
        }
        if (!mine && !wait) return count(l, D_TRANSLATING);   // (at the start, one already being translated is waited for)
        if (mine) start_variant(&fam, std::make_shared<std::vector<uint8_t>>(bank, bank + lay_n), want, now);
        while (wait && !(v = pick()) && clk::now() < wait_until) {
            {
                std::lock_guard lk(fam.mu);
                if (!fam.pending.count(want)) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!v) v = pick();
        if (!v) return count(l, D_TRANSLATING);
        st_waited.fetch_add(1, std::memory_order_relaxed);
    }
    if (v->skip || !v->cs) return count(l, D_VARIANT);
    if (skipped_by_dbg(v->key)) return count(l, D_VARIANT);
    VkCommandBuffer cb = cmd(l);   // (and the list's slot, whose arena the bank goes to)
    // (the uniform buffers' addresses: c0's copy in the arena, c1, the game's in layer 0)
    uint8_t* map;
    VkDeviceAddress bank_addr, table_addr;
    if (!draw_arena(l->slot, v->bank_bytes, map, bank_addr)) return count(l, D_ARENA);
    const size_t fix_at = c.fixups.size();
    bank_fill(c, fam.lay, grid, map, v->bank_bytes, true);
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
            if (!layer0_address(b.addr, bsize, entries[i])) return c.fixups.resize(fix_at), count(l, D_UBO_MEM);
        }
    }
    for (int i = 0; i < 16; i++) {   // (the storage buffers the program reaches by address: bound in layer 0 ahead)
        VkDeviceAddress unused;
        if (c.ssbo[i].addr) layer0_address(c.ssbo[i].addr, std::max<uint64_t>(c.ssbo[i].size, 16), unused);
    }
    uint8_t* tmap;
    if (!draw_arena(l->slot, std::max<uint32_t>(8, 8 * (uint32_t)nu), tmap, table_addr)) return c.fixups.resize(fix_at), count(l, D_ARENA);
    std::memcpy(tmap, entries, 8 * nu);
    VkBuffer abuf = VK_NULL_HANDLE;
    VkDeviceSize aoff = 0;
    if (indirect && ((args & 3) || !layer0_span(args, 12, abuf, aoff))) return c.fixups.resize(fix_at), count(l, D_ARGS_MEM);
    uint8_t push[kTablePush + 8]{};
    {
        const float one = 1.0f;
        const VkDeviceAddress wt = draw_window_table();
        std::memcpy(push + 24, &one, 4);
        std::memcpy(push + 88, &wt, 8);
        std::memcpy(push + kTablePush, &table_addr, 8);
    }
    // (recorded)
    if (c.cb != cb) c.cb = cb, c.bound = VK_NULL_HANDLE, heap_bind(cb);
    if (c.bound != v->cs) {
        const VkShaderStageFlagBits st = VK_SHADER_STAGE_COMPUTE_BIT;
        vkCmdBindShadersEXT(cb, 1, &st, &v->cs);
        c.bound = v->cs;
    }
    heap_push(cb, push, sizeof push);
    const uint32_t mk = marks_on() ? mark_before(cb, indirect ? "dispatch indirect" : "dispatch", v->key, l->info.kind, l->info.lr, grid[0], grid[1]) : 0;   // 
    uint32_t ps = ~0u;
    if (pstats_on()) {   // (M6h: HOS_DBG=pstats)
        PstatWhat w;
        w.what = indirect ? "dispatch indirect" : "dispatch", w.key = v->key, w.codes[0] = v->key, w.kind = l->info.kind, w.lr = l->info.lr;
        w.count = grid[0], w.instances = grid[1], w.extra = grid[2];
        ps = pstat_before(cb, w);
    }
    if (indirect) pDispatchIndirect(cb, abuf, aoff);
    else pDispatch(cb, grid[0], grid[1], grid[2]);
    // (the game puts its barriers before its dispatches only: a draw or copy that follows the last dispatch of a chain has none,
    // because the console's switch from compute to graphics waits by itself. So whatever the list records next waits for these writes)
    l->pending_barrier = true;
    if (pstats_on()) pstat_after(cb, ps);
    if (marks_on()) mark_after(cb, mk);
    draw_push_invalidate(l);   // (the push data is the command buffer's: the next draw pushes its own again)
    tell_first(*v, l, map, v->bank_bytes, grid, indirect);
    if (draw_log_frame()) trace_dispatch(*v, l, map, v->bank_bytes, grid, indirect ? args : 0);
    else if (static const uint64_t watch = [] {   // (HOS_DBG=writer:<hex>: every dispatch whose storage buffers cover it)
                 const char* w = dbg_value("writer");
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
            heap_describe(h & 0xFFFFF, e, sizeof e);
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
    auto barrier = [](VkCommandBuffer cb) {   // (NVN orders them with the 3D engine's work: made visible to what follows)
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1, d.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &d);
    };
    if (op == NOP_COPY_B2B) {
        nvn_p_copy_bb p{};
        std::memcpy(&p, data, std::min<size_t>(sizeof p, size));
        VkBuffer sb, db;
        VkDeviceSize so, dof;
        if (const char* w = dbg_value("writer")) {   // (HOS_DBG=writer:<hex>: buffer copies over it)
            const uint64_t at = strtoull(w, nullptr, 16);
            if (p.dst <= at && at < p.dst + p.size)
                logf("direct: writer: present %llu, copy %#llx -> %#llx (%#llx bytes) in a list of kind %u (lr %#llx)",
                     (unsigned long long)present_count(), (unsigned long long)p.src, (unsigned long long)p.dst, (unsigned long long)p.size,
                     l->info.kind, (unsigned long long)l->info.lr);
        }
        if (!p.size || !layer0_span(p.src, p.size, sb, so) || !layer0_span(p.dst, p.size, db, dof)) {
            if (p.size)
                strict_event("buffer copies skipped (not in layer 0)", "copy %#llx -> %#llx (%#llx bytes), list kind %u lr %#llx",
                             (unsigned long long)p.src, (unsigned long long)p.dst, (unsigned long long)p.size, l->info.kind,
                             (unsigned long long)l->info.lr);
            return bump(me().buf_ops[1]);
        }
        VkCommandBuffer cb = cmd(l);
        const VkBufferCopy r{so, dof, p.size};
        pCopyBuffer(cb, sb, db, 1, &r);
        barrier(cb);
        return bump(me().buf_ops[0]);
    }
    nvn_p_clear_buf p{};
    std::memcpy(&p, data, std::min<size_t>(sizeof p, size));
    VkBuffer db;
    VkDeviceSize dof;
    if (!p.size || (p.dst & 3) || (p.size & 3) || !layer0_span(p.dst, p.size, db, dof)) {
        if (p.size)
            strict_event("buffer clears skipped (unaligned or not in layer 0)", "clear %#llx (%#llx bytes), list kind %u lr %#llx",
                         (unsigned long long)p.dst, (unsigned long long)p.size, l->info.kind, (unsigned long long)l->info.lr);
        return bump(me().buf_ops[3]);
    }
    VkCommandBuffer cb = cmd(l);
    pFillBuffer(cb, db, dof, p.size, p.value);
    barrier(cb);
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
    if (!p || !heap_on() || !p->nstage) return;
    nvn_prog copy;
    std::memcpy(&copy, p, sizeof copy);
    load_functions();
    slot_make(copy);
}

int64_t compute_objects_made() { return st_cs_objects.load(std::memory_order_relaxed); }

void compute_log(double frames) {
    if (const char* pk = dbg_value("peek")) {   // (HOS_DBG=peek:<guest address>: 64 KB there, nonzero bytes and a checksum)
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
         "%lld failed), shader objects %lld (%.1f s, %lld failed; %lld from their binaries, %lld binaries refused), %lld with matrix multiplies, %lld reading the grid otherwise; misses %.1f "
         "a frame, %zu jobs waiting, the last %.2f s after the device; deferred words copied %.1f a frame; separate compute samplers bound %lld; dispatches that waited for their variant at the start %lld; translated when set %lld; moved ahead of the load's work %lld; programs read at their first dispatch %lld",
         (long long)st_slots.load(), (long long)st_codes.load(), (long long)st_variants.load(), (long long)st_translated.load(),
         st_translate_ns.load() / 1e9, (long long)st_from_disk.load(), (long long)st_failed.load(), (long long)st_cs_objects.load(),
         st_object_ns.load() / 1e9, (long long)st_object_failed.load(), (long long)st_from_binary.load(), (long long)st_binary_failed.load(), (long long)st_coopmat.load(), (long long)st_grid_left.load(),
         st_misses.exchange(0) / frames, queued, last_miss_s.load(), st_fixups.exchange(0) / frames, (long long)st_samp_binds.load(), (long long)st_waited.load(), (long long)st_prefetched.load(),
         (long long)st_bumped.load(), (long long)st_read_here.load());
}

}  // namespace hsd
