// Programs to shader objects (direct_shader.h). Lifted from gpu/vk/vk_shader.cpp (the code's reading and lowering, Env, the
// profile, the graphics translation, the key, the answers, the cache's record format) with its switches as the constants
// the frozen back end runs with (RX_VK_DESC_HEAP_BDA=2: storage buffers by address, wide), and from vk_desc_heap.cpp
// make_program (the heap mapping) and vk_shader_object.cpp so_make (the shader objects). What differs (step A,
// m2-shader-contract.md section 6): uniform buffers are read through addresses (an address table in the frame arena, its
// address pushed), not ring descriptors; a texture whose handle is a game buffer's word reads it there through the buffer's
// pushed address, as a driver bank's; textures and samplers are the registration-time entries. The cache is this back end's
// own file (hookshot_direct_programs.bin), stamped by what translates (HOOKSHOT_DIRECT_SHADER_STAMP, nx2/build.py) and the
// device's answers: a file of another stamp is truncated.
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
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
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/oot_visual_workaround.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

#include "direct_heap.h"
#include "direct_image.h"
#include "direct_shader.h"
#include "direct_state.h"

#ifndef HOOKSHOT_DIRECT_SHADER_STAMP
#define HOOKSHOT_DIRECT_SHADER_STAMP 0ull
#endif

namespace hsd {
double seconds_since_device();   // (direct_draw.cpp)
bool mapped_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& map,
                   VkDeviceAddress& address);   // (direct_heap.cpp)

namespace {

using clk = std::chrono::steady_clock;
constexpr uint32_t kControlMagic = 0x98761234;
constexpr uint32_t kControlSize = 0xd8;
constexpr uint32_t kAmpereHeader = 128;

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// ---- a stage's code (vk_shader.cpp Code, read_code, parse_code; graphics stages) ----
struct Code {
    int stage = -1;   // (NVN stage)
    uint64_t key = 0;
    std::vector<uint64_t> code;   // (header, Maxwell instructions, native address map)
    std::vector<uint8_t> c1;
    uint32_t texture_bound = ~0u;
    bool header_only = false;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
    bool ok = false;
};
bool control_end(const uint8_t* ctl, uint32_t* end_out) {
    uint32_t magic, len, c1o, end;
    uint16_t st;
    std::memcpy(&magic, ctl, 4), std::memcpy(&len, ctl + 0x38, 4), std::memcpy(&c1o, ctl + 0x40, 4), std::memcpy(&end, ctl + 0x44, 4);
    std::memcpy(&st, ctl + 0x56, 2);
    if (magic != kControlMagic || !len || end > (16u << 20) || c1o > end || len > end || st > 7) return false;
    return *end_out = end, true;
}
/// The stage's package read: its control section and code area (nullopt-like: ok false)
bool read_package(const nvn_stage_data& sd, uint8_t ctl[kControlSize], std::vector<uint8_t>& area) {
    uint32_t end = 0;
    if (!sd.control || !sd.data || !host->gpu_read(sd.control, ctl, kControlSize) || !control_end(ctl, &end)) return false;
    area.resize(end);
    return host->gpu_read(sd.data, area.data(), end) != 0;
}
/// The stage's key, NVN stage and header (what a draw's key needs), without lowering it
bool scan_code(const nvn_stage_data& sd, Code& c) {
    uint8_t ctl[kControlSize];
    std::vector<uint8_t> area;
    if (!read_package(sd, ctl, area)) return false;
    uint32_t len;
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4), std::memcpy(&st, ctl + 0x56, 2);
    c.key = hash_bytes(area.data(), area.size(), hash_bytes(ctl, kControlSize));
    c.stage = st;
    if (st != 5) std::memcpy(c.sph, area.data(), std::min<size_t>(sizeof c.sph, area.size()));
    c.header_only = st != 5 && len <= kAmpereHeader + 16;
    return true;
}
/// The stage lowered to Maxwell, as the translator reads it
bool parse_code(const nvn_stage_data& sd, Code& c) {
    uint8_t ctl[kControlSize];
    std::vector<uint8_t> area;
    if (!read_package(sd, ctl, area)) return false;
    uint32_t len, c1o;
    uint16_t st;
    std::memcpy(&len, ctl + 0x38, 4), std::memcpy(&c1o, ctl + 0x40, 4), std::memcpy(&st, ctl + 0x56, 2);
    c.key = hash_bytes(area.data(), area.size(), hash_bytes(ctl, kControlSize));
    c.stage = st;
    const bool graphics = st != 5;
    const uint32_t hdr = graphics ? kAmpereHeader : 0;
    if (graphics) std::memcpy(c.sph, area.data(), std::min<size_t>(sizeof c.sph, area.size()));
    if (graphics && len <= hdr + 16) return c.header_only = true, c.ok = true;
    using Shader::Ampere::Instruction;
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    for (uint32_t i = 0; hdr + (i + 1) * sizeof(Instruction) <= len; i++) {
        Instruction& insn = native.emplace_back();
        std::memcpy(&insn, area.data() + hdr + i * sizeof(Instruction), sizeof insn);
        const auto r = capture.Observe(insn, i);
        if (r == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) return false;
        if (r == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
    }
    Shader::Ampere::native_matrix_multiply = false;   // (graphics stages: the exact expansion)
    const auto lowered = Shader::Ampere::LowerToMaxwell(native);
    if (!lowered) {
        static std::atomic<int> told{0};
        if (told++ < 40)
            logf("direct: program stage %016llx (NVN stage %u) does not lower at +%#x: %s", (unsigned long long)c.key, st,
                 lowered.error_offset, std::string(lowered.error).c_str());
        return false;
    }
    c.texture_bound = lowered.required_texture_buffer;
    c.code.assign(sizeof(Shader::ProgramHeader) / 8, 0);
    if (graphics) std::memcpy(c.code.data(), area.data(), sizeof(Shader::ProgramHeader));
    for (const uint64_t instruction : lowered.instructions) {
        if (c.code.size() * 8 % 32 == 0) c.code.push_back(0);   // (the Maxwell scheduling word)
        c.code.push_back(instruction);
    }
    c.code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(c.code, lowered.source_instruction_offsets);
    c.c1.assign(area.begin() + c1o, area.end());
    return c.ok = true;
}
const Shader::ProgramHeader& header_of(const uint8_t* sph) { return *reinterpret_cast<const Shader::ProgramHeader*>(sph); }

// ---- the bound textures' answers (vk_shader.cpp texture_answer, canonical_answer) ----
uint8_t live_answer(uint32_t handle) {
    const uint8_t a = heap_answer(handle & 0xFFFFF);
    return a == 0xFF ? (uint8_t)Shader::TextureType::Color2D : a;   // (not registered: what the frozen back end answers)
}
uint8_t canonical(uint8_t type, bool dim_fixed, uint8_t desc_type, bool depth_float) {
    const uint8_t dim = type & 0x7f;
    if (dim_fixed && dim != (uint8_t)Shader::TextureType::Color2DRect && dim != (uint8_t)Shader::TextureType::Buffer)
        type = (uint8_t)((type & 0x80) | desc_type);
    if (depth_float) return type & 0x7f;
    return type;
}

// ---- the translator's questions (vk_shader.cpp Env, over a list's state) ----
class Env final : public Shader::Environment {
public:
    Env(const DState& state, const Code& c, Shader::Stage st) : s{state}, nvn{c} {
        stage = st;
        start_address = 0;
        std::memcpy(&sph, reinterpret_cast<const uint8_t*>(nvn.code.data()), sizeof sph);
        local_memory = static_cast<uint32_t>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
        is_proprietary_driver = true;
    }
    u64 ReadInstruction(u32 address) override { return address / 8 < nvn.code.size() ? nvn.code[address / 8] : 0; }
    u32 TranslateProgramAddress(u32 address) const override {
        if (const auto t = Shader::Ampere::NativeProgramAddress(nvn.code, address)) return *t;
        throw Shader::RuntimeError("indirect branch to native address {:#x}, not in the program", address);
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        if (index == 1) {
            uint32_t v = 0;
            if ((size_t)offset + 4 <= nvn.c1.size()) std::memcpy(&v, nvn.c1.data() + offset, 4);
            return v;
        }
        if (index == 0 || index == 2) return bank_word(s, (int)index, nvn.stage, offset);
        // (M3b, step B: a game buffer's word is not read. Translation answers from the program alone where the draw cannot
        // check the answer: a handle there is the shader's to read, a texture of it answers as its instruction says)
        if (index >= 3) game_words++;
        return 0;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        questions++;
        return handle ? (Shader::TextureType)(live_answer(handle) & 0x7f) : Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return Shader::TexturePixelFormat{}; }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        questions++;
        return handle && (live_answer(handle) & 0x80);
    }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return nvn.texture_bound != ~0u ? nvn.texture_bound : 2; }
    u32 LocalMemorySize() const override { return local_memory; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return stage == Shader::Stage::VertexA || stage == Shader::Stage::VertexB; }
    bool HasNativeInstructions() const override { return true; }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override {
        // (M3b, step B: from the program alone. A word of the driver bank's storage buffer table is a storage buffer's
        // address whatever is bound there; any other is not taken for one: its accesses stay global memory, which is read by
        // address all the same (every stage by address))
        static constexpr uint32_t kSsboTable[8] = {0x110, 0x510, 0x410, 0x210, 0x310, 0, 0x670, 0x770};
        const uint32_t base = nvn.stage >= 0 && nvn.stage < 8 ? kSsboTable[nvn.stage] : 0;
        return index == 0 && base && offset >= base && offset < base + 16 * 16 && (offset - base) % 16 == 0;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        if (bank != 0 || !HasHLEMacroState()) return std::nullopt;
        switch (offset) {
        case 0x640: return Shader::ReplaceConstant::BaseVertex;
        case 0x644: return Shader::ReplaceConstant::BaseInstance;
        case 0x648: return Shader::ReplaceConstant::DrawID;
        default: return std::nullopt;
        }
    }
    void Dump(u64, u64) override {}
    int questions = 0;
    int game_words = 0;   // (words of game buffers the translator asked for: answered 0)

private:
    const DState& s;
    const Code& nvn;
    uint32_t local_memory = 0;
};

// ---- the profile (vk_shader.cpp make_profile with RX_VK_DESC_HEAP_BDA=2 on layer 0) ----
Shader::Profile make_profile() {
    {   // (the translation's per-GPU settings, as gpu/vk/vk_coopmat.cpp coopmat_init sets them on this card: in the stamp)
        using namespace Shader::Backend::SPIRV;
        frag_nan_guard = true;
        position_invariant = dev.props.vendorID != 0x10DE;
    }
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_vertex_instance_id = false;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp32_denorm_preserve = p.support_fp32_denorm_flush = true;
    p.support_fp16_signed_zero_nan_preserve = p.support_fp32_signed_zero_nan_preserve = true;
    p.support_vote = true;
    p.supported_subgroup_stages = VK_SHADER_STAGE_ALL;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = true;
    p.support_derivative_control = true;
    p.support_native_ndc = false;
    p.support_scaled_attributes = false;
    p.support_multi_viewport = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    p.warp_size_potentially_larger_than_guest = caps.subgroup_size > 32 && (caps.sg32_stages & VK_SHADER_STAGE_ALL_GRAPHICS) != VK_SHADER_STAGE_ALL_GRAPHICS;
    p.lower_left_origin_mode = false;
    p.need_declared_frag_colors = false;
    p.max_subgroup_size = p.warp_size_potentially_larger_than_guest ? caps.subgroup_max : 32;
    p.has_broken_spirv_fragment_shuffle = caps.driver_id == VK_DRIVER_ID_AMD_PROPRIETARY;
    p.min_ssbo_alignment = dev.props.limits.minStorageBufferOffsetAlignment;
    p.ssbo_bda = true;
    p.ssbo_bda_check = false;
    p.ssbo_bda_wide = true;
    p.heap_handles = true;   // (M3b, step B: the shader reads every handle and indexes the heap with it)
    return p;
}
const Shader::Profile& profile() {
    static const Shader::Profile p = make_profile();
    return p;
}
const Shader::Profile& profile_graphics(VkShaderStageFlags stage) {
    static const Shader::Profile pinned = [] {
        Shader::Profile q = profile();
        q.warp_size_potentially_larger_than_guest = false, q.max_subgroup_size = 32;
        return q;
    }();
    return (caps.sg32_stages & stage) ? pinned : profile();
}
Shader::HostTranslateInfo host_info() {
    Shader::HostTranslateInfo h{};
    h.support_float64 = true;
    h.support_float16 = true;
    h.support_int64 = true;
    h.needs_demote_reorder = false;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = true;
    h.min_ssbo_alignment = dev.props.limits.minStorageBufferOffsetAlignment;
    h.support_geometry_shader_passthrough = false;
    h.support_conditional_barrier = true;
    h.heap_handles = true;
    return h;
}

struct Translation {
    Shader::ObjectPool<Shader::IR::Inst> insts;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
    Shader::IR::Program program;
};
std::unique_ptr<Translation> translate(Env& env) {
    auto t = std::make_unique<Translation>();
    Shader::Maxwell::Flow::CFG cfg{env, t->flow_blocks, env.StartAddress() + static_cast<u32>(sizeof(Shader::ProgramHeader)), false};
    t->program = Shader::Maxwell::TranslateProgram(t->insts, t->blocks, env, cfg, host_info());
    return t;
}

Shader::AttributeType attribute_type_of(uint32_t vattr) {
    const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&vattr);
    switch (a.fmt) {
    case NVN_FORMAT_R8UI: case NVN_FORMAT_RG8UI: case NVN_FORMAT_RGB8UI: case NVN_FORMAT_RGBA8UI:
    case NVN_FORMAT_R16UI: case NVN_FORMAT_RG16UI: case NVN_FORMAT_RGB16UI: case NVN_FORMAT_RGBA16UI:
    case NVN_FORMAT_R32UI: case NVN_FORMAT_RG32UI: case NVN_FORMAT_RGB32UI: case NVN_FORMAT_RGBA32UI:
    case NVN_FORMAT_RGB10A2UI:
        return Shader::AttributeType::UnsignedInt;
    case NVN_FORMAT_R8I: case NVN_FORMAT_RG8I: case NVN_FORMAT_RGB8I: case NVN_FORMAT_RGBA8I: case NVN_FORMAT_R16I:
    case NVN_FORMAT_RG16I: case NVN_FORMAT_RGB16I: case NVN_FORMAT_RGBA16I: case NVN_FORMAT_R32I: case NVN_FORMAT_RG32I:
    case NVN_FORMAT_RGB32I: case NVN_FORMAT_RGBA32I: case NVN_FORMAT_RGB10A2I:
        return Shader::AttributeType::SignedInt;
    case NVN_FORMAT_RGB10A2_UI2F: return Shader::AttributeType::UnsignedScaled;
    case NVN_FORMAT_RGB10A2_I2F: return Shader::AttributeType::SignedScaled;
    default:
        if (a.fmt >= NVN_FORMAT_R8_UI2F && a.fmt <= NVN_FORMAT_RGBA32_I2F)
            return (a.fmt - NVN_FORMAT_R8_UI2F) & 1 ? Shader::AttributeType::SignedScaled : Shader::AttributeType::UnsignedScaled;
        return Shader::AttributeType::Float;
    }
}
/// A vertex and fragment program's key (vk_shader.cpp graphics_key)
uint64_t graphics_key(uint64_t vs_key, const uint8_t* vs_sph, uint64_t fs_key, const uint8_t* fs_sph, const uint32_t vattr[16],
                      const uint8_t color_types[8]) {
    uint64_t key = vs_key ^ (fs_sph ? fs_key * 31 : 0);
    for (int i = 0; i < 16; i++) {
        const auto used = header_of(vs_sph).vtg.InputGeneric((size_t)i);
        const bool read = used[0] || used[1] || used[2] || used[3];
        key = key * 1099511628211ull ^ (uint64_t)(read ? attribute_type_of(vattr[i]) : Shader::AttributeType::Float) << i;
    }
    uint8_t written[8];
    for (int i = 0; i < 8; i++) written[i] = fs_sph && header_of(fs_sph).ps.HasOutputComponents(i) ? color_types[i] : 0;
    return hash_bytes(written, 8, key);
}

// ---- variants ----
struct VStage {
    int nvn_stage = 0;
    VkShaderStageFlagBits vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
    Shader::Info info;
    std::vector<uint8_t> c1;
    std::vector<uint32_t> spirv;
};
struct RawAnswer {
    uint16_t stage, desc;
    uint8_t type;
    bool image = false;
};
struct Variant {
    DrawProg d;
    std::vector<VStage> stages;
    std::vector<RawAnswer> answers;
    Variant* next = nullptr;
};
struct StageScan {
    uint64_t key = 0;
    uint8_t sph[sizeof(Shader::ProgramHeader)]{};
};
/// (M3b) The variants of one pair of stage codes, shared by every program slot with that code (the game sets the same code in
/// thousands of program objects: 12,528 set in Kokiri for 149 variants drawn). Made once, never freed
struct Family {
    std::atomic<Variant*> variants{nullptr};
    std::atomic<bool> loaded{false};   // (its variants kept in the cache file are made)
    std::mutex mu;   // (pending)
    std::unordered_set<uint64_t> pending;   // (translations queued, by key and answers)
    std::unordered_set<uint64_t> failed;    // (translations that failed)
};
std::mutex families_mu;
std::unordered_map<uint64_t, Family*> families;   // (by pair_key of the stages' code keys)
std::atomic<int64_t> st_families{0};

/// A program, by its NVN serial
struct Slot {
    uint64_t serial = 0;
    nvn_prog prog{};
    std::atomic<int> state{0};   // (0 being read, 1 a vertex (+ fragment) program, 2 not one, 3 its code not readable)
    StageScan vs, fs;
    bool has_fs = false;
    const char* why = "";
    Family* fam = nullptr;   // (set before state 1)
};

constexpr uint32_t kSlots = 1u << 17;
std::unique_ptr<std::atomic<Slot*>[]> table;
std::mutex table_mu;
std::atomic<double> last_miss_s{0};
std::atomic<int64_t> st_object_ns{0}, st_disk_ns{0}, st_ubos_max{0}, st_ubos_sum{0}, st_ubos_variants{0}, st_ubos_over{0}, st_game_words{0}, st_multi_inputs{0}, st_multi_answers{0}, st_slots{0}, st_full{0}, st_translated{0}, st_failed{0}, st_from_disk{0}, st_objects_made{0},
    st_object_failed{0}, st_translate_ns{0}, st_queued{0}, st_misses{0}, st_scan_failed{0};

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

// ---- the workers ----
std::mutex work_mu;
std::condition_variable work_cv;
std::deque<std::function<void()>> work;
std::once_flag workers_once;
void push_work(std::function<void()> f) {
    std::call_once(workers_once, [] {
        for (int i = 0; i < 2; i++)
            std::thread([] {
                for (;;) {
                    std::function<void()> f;
                    {
                        std::unique_lock lk(work_mu);
                        work_cv.wait(lk, [] { return !work.empty(); });
                        f = std::move(work.front());
                        work.pop_front();
                    }
                    f();
                }
            }).detach();
    });
    {
        std::lock_guard lk(work_mu);
        work.push_back(std::move(f));
    }
    work_cv.notify_one();
}

// ---- the program cache file (its record: vk_shader.cpp's put_info / get_info, the rest this back end's) ----
constexpr uint32_t kDiskMagic = 0x50445348;   // ('HSDP')
std::mutex disk_mu;
FILE* disk_out = nullptr;
FILE* disk_in = nullptr;
bool disk_on = false;
struct DiskRecord {
    int64_t at;
    size_t size;
};
std::unordered_map<uint64_t, std::vector<DiskRecord>> disk_by_pair;   // (vertex code key ^ fragment code key * 31)

uint64_t pair_key(uint64_t vs, uint64_t fs) { return vs ^ fs * 31; }

void put_bytes(std::vector<uint8_t>& o, const void* d, size_t n) { o.insert(o.end(), (const uint8_t*)d, (const uint8_t*)d + n); }
template <class T> void put(std::vector<uint8_t>& o, const T& v) { put_bytes(o, &v, sizeof v); }
template <class V> void put_list(std::vector<uint8_t>& o, const V& v) {
    put(o, (uint64_t)v.size());
    for (const auto& e : v) put(o, e);
}
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool bad = false;
    bool has(uint64_t n) {
        if (!bad && n > (uint64_t)(end - p)) bad = true;
        return !bad;
    }
    template <class T> T get() {
        alignas(T) uint8_t b[sizeof(T)]{};
        if (has(sizeof(T))) std::memcpy(b, p, sizeof(T)), p += sizeof(T);
        return *(T*)b;
    }
    uint64_t count(size_t elem) {
        const uint64_t n = get<uint64_t>();
        if (!bad && n > (uint64_t)(end - p) / (elem ? elem : 1)) bad = true;
        return bad ? 0 : n;
    }
    template <class V> void list(V& v) {
        const uint64_t n = count(sizeof(typename V::value_type));
        v.clear();
        v.reserve((size_t)n);
        for (uint64_t i = 0; i < n && !bad; i++) v.push_back(get<typename V::value_type>());
    }
};
void put_info(std::vector<uint8_t>& o, const Shader::Info& i) {
    put_bytes(o, &i, sizeof i);
    put_list(o, i.constant_buffer_descriptors);
    put_list(o, i.storage_buffers_descriptors);
    put_list(o, i.texture_buffer_descriptors);
    put_list(o, i.image_buffer_descriptors);
    put_list(o, i.texture_descriptors);
    put_list(o, i.image_descriptors);
    put(o, (uint64_t)i.legacy_stores_mapping.size());
    for (const auto& kv : i.legacy_stores_mapping) put(o, kv.first), put(o, kv.second);
    put(o, (uint64_t)i.handle_programs.size());
    for (const auto& h : i.handle_programs) put_list(o, h);
}
void get_info(Reader& r, Shader::Info& i) {
    if (!r.has(sizeof i)) return;
    alignas(Shader::Info) static thread_local uint8_t fresh[sizeof(Shader::Info)];
    std::memcpy(fresh, (const void*)&i, sizeof i);
    std::memcpy((void*)&i, r.p, sizeof i);
    r.p += sizeof i;
#define KEEP(m) std::memcpy((void*)&i.m, fresh + ((const uint8_t*)&i.m - (const uint8_t*)&i), sizeof i.m)
    KEEP(constant_buffer_descriptors);
    KEEP(storage_buffers_descriptors);
    KEEP(texture_buffer_descriptors);
    KEEP(image_buffer_descriptors);
    KEEP(texture_descriptors);
    KEEP(image_descriptors);
    KEEP(legacy_stores_mapping);
    KEEP(handle_programs);
#undef KEEP
    r.list(i.constant_buffer_descriptors);
    r.list(i.storage_buffers_descriptors);
    r.list(i.texture_buffer_descriptors);
    r.list(i.image_buffer_descriptors);
    r.list(i.texture_descriptors);
    r.list(i.image_descriptors);
    for (uint64_t n = r.count(2 * sizeof(Shader::IR::Attribute)); n && !r.bad; n--) {
        const auto k = r.get<Shader::IR::Attribute>();
        i.legacy_stores_mapping[k] = r.get<Shader::IR::Attribute>();
    }
    i.handle_programs.resize((size_t)r.count(sizeof(uint64_t)));
    for (auto& h : i.handle_programs) r.list(h);
    auto fit = [](const auto& list) {
        for (const auto& d : list)
            if (d.count > 4096) return false;
        return true;
    };
    if (!fit(i.constant_buffer_descriptors) || !fit(i.storage_buffers_descriptors) || !fit(i.texture_descriptors) ||
        !fit(i.image_descriptors) || !fit(i.texture_buffer_descriptors) || !fit(i.image_buffer_descriptors))
        r.bad = true;
}

uint64_t disk_stamp() {
    uint64_t h = 0xCBF29CE484222325ull;
    const auto add = [&h](const void* d, size_t n) { h = hash_bytes(d, n, h); };
    const uint64_t translator = HOOKSHOT_DIRECT_SHADER_STAMP;
    add(&translator, sizeof translator);
    const uint32_t depth = host->depth_minus_w() != 0;
    add(&depth, sizeof depth);
    const uint32_t lanes[] = {profile().warp_size_potentially_larger_than_guest ? 1u : 0u, profile().max_subgroup_size,
                              profile().has_broken_spirv_fragment_shuffle ? 1u : 0u, (uint32_t)caps.sg32_stages};
    add(lanes, sizeof lanes);
    const uint32_t abi[] = {(uint32_t)sizeof(Shader::Info), (uint32_t)sizeof(std::vector<uint32_t>),
#if defined(_LIBCPP_VERSION)
                            1u, (uint32_t)_LIBCPP_VERSION
#else
                            3u, 0u
#endif
    };
    add(abi, sizeof abi);
    using namespace Shader::Backend::SPIRV;
    const uint32_t flags = (position_invariant ? 1u : 0u) | (frag_nan_guard ? 2u : 0u);
    add(&flags, sizeof flags);
    return h;
}

std::string exe_dir() {
    char b[4096];
    const ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    std::string p(b, n > 0 ? (size_t)n : 0);
    return p.substr(0, p.find_last_of('/') + 1);
}

void disk_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        const std::string path = exe_dir() + "hookshot_direct_programs.bin";
        const int fd = open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
            logf("direct: program cache %s: another Hookshot is using it: this one runs without it", path.c_str());
            return;
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
                while (good + 32 <= whole) {
                    uint64_t rec[2], pair;
                    fseeko(f, good, SEEK_SET);
                    if (fread(rec, 8, 2, f) != 2 || fread(&pair, 8, 1, f) != 1) break;
                    if (rec[1] < 8 || rec[1] > (uint64_t)(whole - good - 16)) break;
                    disk_by_pair[pair].push_back({good + 16, (size_t)rec[1]});
                    good += 16 + (int64_t)rec[1];
                    records++;
                }
            }
            fclose(f);
        }
        if (good) {
            if (good != whole && truncate(path.c_str(), good) != 0) good = 0;
        }
        if (good) {
            disk_out = fopen(path.c_str(), "ab");
        } else {   // (none, or another stamp: made again)
            disk_by_pair.clear();
            records = 0;
            disk_out = fopen(path.c_str(), "wb");
            if (disk_out) {
                const uint32_t h[2] = {kDiskMagic, 0};
                fwrite(h, 4, 2, disk_out), fwrite(&stamp, 8, 1, disk_out), fflush(disk_out);
            }
        }
        disk_in = disk_out ? fopen(path.c_str(), "rb") : nullptr;
        disk_on = disk_out && disk_in;
        logf("direct: program cache %s: %zu variants kept (%lld KB)%s", path.c_str(), records, (long long)(good >> 10),
             whole && !good ? "; another stamp's file was truncated" : "");
    });
}

void disk_save(const Variant& v, uint64_t pair) {
    disk_init();
    if (!disk_on) return;
    std::vector<uint8_t> o;
    o.reserve(128 << 10);
    put(o, pair);
    put(o, v.d.key);
    put(o, (uint64_t)v.answers.size());
    for (const RawAnswer& a : v.answers) put(o, a);
    put(o, (uint64_t)v.stages.size());
    for (const VStage& st : v.stages) {
        put(o, st.nvn_stage);
        put(o, st.vk_stage);
        put_list(o, st.spirv);
        put_list(o, st.c1);
        put_info(o, st.info);
    }
    const uint64_t head[2] = {v.d.key, o.size()};
    std::lock_guard lk(disk_mu);
    fseeko(disk_out, 0, SEEK_END);
    fwrite(head, 8, 2, disk_out);
    fwrite(o.data(), 1, o.size(), disk_out);
    fflush(disk_out);
}

bool disk_read(const DiskRecord& d, Variant& v) {
    std::vector<uint8_t> data(d.size);
    {
        std::lock_guard lk(disk_mu);
        fseeko(disk_in, d.at, SEEK_SET);
        if (fread(data.data(), 1, data.size(), disk_in) != data.size()) return false;
    }
    Reader r{data.data(), data.data() + data.size()};
    r.get<uint64_t>();   // (the pair)
    v.d.key = r.get<uint64_t>();
    const uint64_t na = r.count(sizeof(RawAnswer));
    for (uint64_t i = 0; i < na && !r.bad; i++) v.answers.push_back(r.get<RawAnswer>());
    const uint64_t n = r.count(1);
    if (r.bad || n == 0 || n > 2) return false;
    for (uint64_t k = 0; k < n && !r.bad; k++) {
        VStage& st = v.stages.emplace_back();
        st.nvn_stage = r.get<int>();
        st.vk_stage = r.get<VkShaderStageFlagBits>();
        r.list(st.spirv);
        r.list(st.c1);
        get_info(r, st.info);
        if (st.spirv.empty()) r.bad = true;
    }
    for (const RawAnswer& a : v.answers)
        if (a.stage >= v.stages.size() ||
            a.desc >= (a.image ? v.stages[a.stage].info.image_descriptors.size() : v.stages[a.stage].info.texture_descriptors.size()))
            r.bad = true;
    return !r.bad;
}

// ---- c1: the programs' own constants, in device memory made at load ----
std::mutex c1_mu;
struct C1Chunk {
    uint8_t* map = nullptr;
    VkDeviceAddress address = 0;
    uint64_t used = 0, size = 0;
};
C1Chunk c1_chunk;
VkDeviceAddress c1_put(const std::vector<uint8_t>& bytes, uint32_t used) {
    const uint64_t n = (std::max<uint64_t>({bytes.size(), used, 16}) + 255) & ~255ull;
    std::lock_guard lk(c1_mu);
    if (c1_chunk.used + n > c1_chunk.size) {
        const uint64_t size = std::max<uint64_t>(n, 4ull << 20);
        VkBuffer b;
        VkDeviceMemory m;
        C1Chunk c;
        if (!mapped_buffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, b, m, c.map, c.address)) return 0;
        c.size = size;
        c1_chunk = c;
    }
    std::memset(c1_chunk.map + c1_chunk.used, 0, n);
    std::memcpy(c1_chunk.map + c1_chunk.used, bytes.data(), bytes.size());
    const VkDeviceAddress a = c1_chunk.address + c1_chunk.used;
    c1_chunk.used += n;
    return a;
}

// ---- the heap mapping and the shader objects (vk_desc_heap.cpp make_program, vk_shader_object.cpp so_make) ----
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
/// A binding the draw cannot give: the zero tail (null descriptors), the variant marked
VkDescriptorSetAndBindingMappingEXT null_map(uint32_t b, uint32_t stride) {
    VkDescriptorSetAndBindingMappingEXT m = map_of(b, VK_SPIRV_RESOURCE_TYPE_ALL_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT);
    m.sourceData.constantOffset.heapOffset = heap_tail_offset();
    m.sourceData.constantOffset.heapArrayStride = stride;
    m.sourceData.constantOffset.samplerHeapOffset = 0;
    m.sourceData.constantOffset.samplerHeapArrayStride = kSamplerDesc;
    return m;
}

/// The variant's mappings, push layout and draw-side tables (v.d), then its shader objects
bool finish(Variant& v) {
    DrawProg& d = v.d;
    std::vector<std::vector<VkDescriptorSetAndBindingMappingEXT>> maps(v.stages.size());
    uint32_t binding = 0;
    for (size_t si = 0; si < v.stages.size(); si++) {
        const VStage& st = v.stages[si];
        const Shader::Info& info = st.info;
        auto& m = maps[si];
        const uint8_t stage = (uint8_t)st.nvn_stage;
        if (info.uses_render_area) d.uses_render_area = true;
        // (uniform buffers: each its address in the table, read through the address the push data holds: INDIRECT_ADDRESS)
        uint32_t cb = binding;
        for (const auto& c : info.constant_buffer_descriptors) {
            if (c.count != 1) {
                d.skip = d.skip ? d.skip : "a uniform buffer array";
                m.push_back(null_map(cb, 16));
                cb += c.count;
                continue;
            }
            static const bool ubopush = dbg("ubopush");   // (HOS_DBG=ubopush, a diagnosis: the first 8 addresses pushed)
            VkDescriptorSetAndBindingMappingEXT x;
            if (ubopush && d.ubos.size() < 8) {
                x = map_of(cb, VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT);
                x.sourceData.pushAddressOffset = 192 + 8 * (uint32_t)d.ubos.size();
                d.direct_ubos = (uint8_t)(d.ubos.size() + 1);
            } else {
                x = map_of(cb, VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_INDIRECT_ADDRESS_EXT);
                x.sourceData.indirectAddress.pushOffset = kTablePush;
                x.sourceData.indirectAddress.addressOffset = 8 * (uint32_t)d.ubos.size();
            }
            m.push_back(x);
            const uint32_t used = c.index < info.constant_buffer_used_sizes.size() ? info.constant_buffer_used_sizes[c.index] : 0;
            d.ubos.push_back({stage, (uint8_t)c.index, c.index == 0 || c.index == 2 ? std::clamp<uint32_t>((used + 15) & ~15u, 16, 0xA00) : used});
            cb += c.count;
        }
        binding += (uint32_t)info.constant_buffer_descriptors.size();
        // (storage buffers: by address in the shader (ssbo_bda); their bindings kept, mapped to nothing)
        if (!info.storage_buffers_descriptors.empty() && !info.ssbo_bda) d.skip = d.skip ? d.skip : "storage buffers not by address";
        for (const auto& s : info.storage_buffers_descriptors) {
            m.push_back(null_map(binding, 16));
            binding += s.count > 1 ? 1 : s.count;
        }
        for (size_t k = 0; k < info.texture_buffer_descriptors.size(); k++) m.push_back(null_map(binding++, kImageDesc)), d.skip = d.skip ? d.skip : "a texel buffer";
        for (size_t k = 0; k < info.image_buffer_descriptors.size(); k++) m.push_back(null_map(binding++, kImageDesc)), d.skip = d.skip ? d.skip : "an image buffer";
        // (textures: each descriptor a runtime array the shader indexes with the handle's image id, mapped to the heap's region
        // of its view type; the samplers two arrays, plain and depth compare, indexed by the handle's sampler id (step B))
        for (const auto& t : info.texture_descriptors) {
            const uint32_t b = binding++;
            const uint32_t region = std::min<uint32_t>((uint32_t)view_type(t.type), kHeapRegions - 1);
            VkDescriptorSetAndBindingMappingEXT x = map_of(b, VK_SPIRV_RESOURCE_TYPE_ALL_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT);
            auto& co = x.sourceData.constantOffset;
            if (handle_is_storage_pointer(info, t)) {   // (a storage buffer's address, not a handle: every index the zero tail)
                co.heapOffset = heap_tail_offset(), co.heapArrayStride = 0;
            } else {
                co.heapOffset = heap_region_offset(region), co.heapArrayStride = kImageDesc;
            }
            m.push_back(x);
            if (t.handle_program == ~0u && t.count == 1 && !t.has_secondary) d.tex_words.push_back({stage, (uint8_t)t.cbuf_index, (uint16_t)t.cbuf_offset});
        }
        if (!info.texture_descriptors.empty()) {   // (both arrays mapped: the shader declares the ones it samples with)
            for (const bool cmp : {false, true}) {
                VkDescriptorSetAndBindingMappingEXT x = map_of(cmp ? Shader::Backend::SPIRV::kHeapCompareSamplerBinding
                                                                   : Shader::Backend::SPIRV::kHeapSamplerBinding,
                                                               VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT,
                                                               VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT);
                // (a plain sampler binding takes heapOffset / heapArrayStride, into the sampler heap: the sampler fields are a
                // combined image sampler's)
                auto& co = x.sourceData.constantOffset;
                co.heapOffset = heap_sampler_offset(cmp), co.heapArrayStride = kSamplerDesc;
                co.samplerHeapOffset = 0, co.samplerHeapArrayStride = 0;
                m.push_back(x);
            }
        }
        // (storage images: the heap's storage region of the view type, indexed by the handle's image id)
        for (const auto& im : info.image_descriptors) {
            const uint32_t region = std::min<uint32_t>((uint32_t)view_type(im.type), kHeapRegions - 1);
            VkDescriptorSetAndBindingMappingEXT x = map_of(binding++, VK_SPIRV_RESOURCE_TYPE_ALL_EXT, VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT);
            x.sourceData.constantOffset.heapOffset = heap_storage_offset(region);
            x.sourceData.constantOffset.heapArrayStride = kImageDesc;
            m.push_back(x);
        }
        if (st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT)
            for (uint32_t i = 0; i < 16; i++)
                if (info.loads.Generic(i)) d.vs_loads |= 1u << i;
        if (st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT)
            for (int i = 0; i < 8; i++) {
                if (info.stores_frag_color[i]) d.frag_stores |= (uint8_t)(1u << i);
                d.frag_components[i] = info.frag_color_components[i];
            }
        if (si < 2) d.c1[si] = c1_put(st.c1, info.constant_buffer_used_sizes.size() > 1 ? info.constant_buffer_used_sizes[1] : 0);
    }
    d.push_bytes = kTablePush + 8 + 8 * (uint32_t)d.pushed.size();
    {   // (M3b: what the address table holds: uniform buffers a variant reads, against the push data a pushed address each would need)
        const int64_t n = (int64_t)d.ubos.size();
        int64_t m = st_ubos_max.load(std::memory_order_relaxed);
        while (n > m && !st_ubos_max.compare_exchange_weak(m, n)) {}
        st_ubos_sum.fetch_add(n, std::memory_order_relaxed), st_ubos_variants.fetch_add(1, std::memory_order_relaxed);
        if (kTablePush + 8 * (uint32_t)n > caps.max_push) st_ubos_over.fetch_add(1, std::memory_order_relaxed);
    }
    if (d.direct_ubos) d.push_bytes = std::max<uint32_t>(d.push_bytes, 192 + 8 * d.direct_ubos);
    if (d.push_bytes > caps.max_push) d.skip = d.skip ? d.skip : "push data over the device's limit";
    // (the answers a draw checks: those whose handle is a driver bank word; one in a game buffer stands as translated)
    for (const RawAnswer& a : v.answers) {
        if (a.image || a.stage >= v.stages.size()) continue;
        const auto& t = v.stages[a.stage].info.texture_descriptors[a.desc];
        if (t.has_secondary || t.handle_program != ~0u || !(t.cbuf_index == 0 || t.cbuf_index == 2)) continue;
        DrawProg::Answer x{};
        x.stage = (uint8_t)v.stages[a.stage].nvn_stage, x.bank = (uint8_t)t.cbuf_index, x.offset = (uint16_t)t.cbuf_offset;
        x.dim_fixed = t.type_fixed, x.desc_type = (uint8_t)t.type, x.depth_float = t.is_depth && !t.is_integer;
        x.want = canonical(a.type, x.dim_fixed, x.desc_type, x.depth_float);
        d.answers.push_back(x);
    }
    // (the shader objects, unlinked: vertex then fragment)
    static const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo size32 = [] {
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo r{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        r.requiredSubgroupSize = 32;
        return r;
    }();
    for (size_t si = 0; si < v.stages.size(); si++) {
        const VStage& st = v.stages[si];
        VkShaderDescriptorSetAndBindingMappingInfoEXT mi{VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT};
        mi.mappingCount = (uint32_t)maps[si].size(), mi.pMappings = maps[si].data();
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sz = size32;
        VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
        ci.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
        ci.stage = st.vk_stage;
        ci.nextStage = st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT ? VK_SHADER_STAGE_FRAGMENT_BIT : 0;
        ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        ci.codeSize = st.spirv.size() * 4;
        ci.pCode = st.spirv.data();
        ci.pName = "main";
        if (caps.sg32_stages & st.vk_stage) sz.pNext = &mi, ci.pNext = &sz;
        else ci.pNext = &mi;
        VkShaderEXT obj = VK_NULL_HANDLE;
        const auto t0 = clk::now();
        const VkResult r = vkCreateShadersEXT(dev.device, 1, &ci, nullptr, &obj);
        st_object_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(), std::memory_order_relaxed);
        if (r != VK_SUCCESS) {
            st_object_failed.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> told{0};
            if (told++ < 20) logf("direct: program %016llx: its %s shader object not made (%d)", (unsigned long long)d.key,
                                  st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT ? "vertex" : "fragment", (int)r);
            return false;
        }
        st_objects.fetch_add(1, std::memory_order_relaxed);
        st_objects_made.fetch_add(1, std::memory_order_relaxed);
        (st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT ? d.vs : d.fs) = obj;
    }
    if (d.skip) {
        static std::mutex mu;
        static std::unordered_set<std::string> told;
        std::lock_guard lk(mu);
        if (told.insert(d.skip).second || told.size() < 40)
            logf("direct: program %016llx is drawn as step A cannot: %s (skipped, counted)", (unsigned long long)d.key, d.skip);
    }
    return true;
}

void publish(Family& s, Variant* v, uint64_t serial) {
    Variant* head = s.variants.load(std::memory_order_acquire);
    if (head) {   // (a program's second variant or later: for another key (inputs: vertex attribute or colour types) or answers)
        bool same_key = false;
        for (const Variant* o = head; o; o = o->next) same_key |= o->d.key == v->d.key;
        (same_key ? st_multi_answers : st_multi_inputs).fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> told{0};
        if (told++ < 30) {
            int n = 1;
            for (const Variant* o = head; o; o = o->next) n++;
            logf("direct: program serial %llu has variant %d: %s (key %016llx)", (unsigned long long)serial, n,
                 same_key ? "the same inputs, other texture answers" : "other inputs (vertex attribute or colour types)",
                 (unsigned long long)v->d.key);
        }
    }
    do v->next = head;
    while (!s.variants.compare_exchange_weak(head, v, std::memory_order_release, std::memory_order_acquire));
}

// ---- translation (vk_shader.cpp translate_into's graphics half, record_answers) ----
uint32_t cbuf_word(const DState& s, int stage, uint32_t index, uint32_t offset, const std::vector<uint8_t>& c1) {
    if (index == 1) {
        uint32_t v = 0;
        if ((size_t)offset + 4 <= c1.size()) std::memcpy(&v, c1.data() + offset, 4);
        return v;
    }
    if (index == 0 || index == 2) return bank_word(s, (int)index, stage, offset);
    return 0;   // (a game buffer's word: not read, as Env::ReadCbufValue)
}
void record_answers(Variant& v, const DState& s) {
    for (size_t k = 0; k < v.stages.size(); k++) {
        const VStage& st = v.stages[k];
        const auto& list = st.info.texture_descriptors;
        for (size_t i = 0; i < list.size(); i++) {
            if (list[i].count != 1) continue;
            const auto& t = list[i];
            uint32_t h = t.handle_program != ~0u ? 0 : cbuf_word(s, st.nvn_stage, t.cbuf_index, t.cbuf_offset, st.c1);
            if (h && t.has_secondary)
                h = h << t.shift_left | cbuf_word(s, st.nvn_stage, t.secondary_cbuf_index, t.secondary_cbuf_offset, st.c1) << t.secondary_shift_left;
            const uint8_t a = h ? live_answer(h) : (uint8_t)((uint8_t)t.type | (t.is_integer ? 0x80 : 0));
            v.answers.push_back({(uint16_t)k, (uint16_t)i, canonical(a, t.type_fixed, (uint8_t)t.type, t.is_depth && !t.is_integer)});
        }
    }
}

bool translate_variant(Variant& v, const DState& s, const Code& vsc, const Code* fsc, const uint8_t color_types[8]) {
    try {
        Shader::Backend::Bindings bindings{};
        int questions = 0;
        Env vs_env{s, vsc, Shader::Stage::VertexB};
        auto vs = translate(vs_env);
        questions += vs_env.questions;
        Shader::RuntimeInfo vrt{};
        if (!vs->program.info.stores[Shader::IR::Attribute::PointSize]) vrt.fixed_state_point_size = 1.0f;
        for (int i = 0; i < 16; i++) vrt.generic_input_types[i] = attribute_type_of(s.vattr[i]);
        vrt.previous_stage_stores.mask.set();
        vrt.convert_depth_mode = host->depth_minus_w() != 0;
        VStage& vst = v.stages.emplace_back();
        vst.nvn_stage = 0, vst.vk_stage = VK_SHADER_STAGE_VERTEX_BIT;
        vst.spirv = Shader::Backend::SPIRV::EmitSPIRV(profile_graphics(VK_SHADER_STAGE_VERTEX_BIT), vrt, vs->program, bindings);
        vst.info = vs->program.info;
        vst.c1 = vsc.c1;
        if (fsc) {
            Env fs_env{s, *fsc, Shader::Stage::Fragment};
            auto fs = translate(fs_env);
            questions += fs_env.questions;
            st_game_words.fetch_add(fs_env.game_words, std::memory_order_relaxed);
            Shader::RuntimeInfo frt{};
            frt.previous_stage_stores = vs->program.info.stores;
            for (int i = 0; i < 8; i++) frt.color_output_types[i] = static_cast<Shader::AttributeType>(color_types[i]);
            uint32_t float_targets = 0;   // (the frozen back end's NaN guard of float targets, on by default)
            for (int i = 0; i < 8; i++)
                if (frt.color_output_types[i] == Shader::AttributeType::Float) float_targets |= 1u << i;
            Shader::Optimization::GuardFragmentOutputs(fs->program, float_targets);
            VStage& fst = v.stages.emplace_back();
            fst.nvn_stage = 1, fst.vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fst.spirv = Shader::Backend::SPIRV::EmitSPIRV(profile_graphics(VK_SHADER_STAGE_FRAGMENT_BIT), frt, fs->program, bindings);
            fst.info = fs->program.info;
            fst.c1 = fsc->c1;
        }
        if (questions) record_answers(v, s);
        st_game_words.fetch_add(vs_env.game_words, std::memory_order_relaxed);
        return true;
    } catch (const std::exception& e) {
        static std::atomic<int> told{0};
        if (told++ < 30) logf("direct: program %016llx not translated: %s", (unsigned long long)v.d.key, e.what());
        return false;
    }
}

/// (a worker) the program's stages read, and its kept variants made
void scan_slot(Slot* s) {
    Code codes[6];
    int vs = -1, fs = -1;
    bool other = false;
    for (uint32_t i = 0; i < s->prog.nstage && i < 6; i++) {
        if (!scan_code(s->prog.sd[i], codes[i])) {
            s->why = "its code is not readable";
            st_scan_failed.fetch_add(1, std::memory_order_relaxed);
            return s->state.store(3, std::memory_order_release);
        }
        const Code& c = codes[i];
        if (c.header_only) continue;
        if (c.stage == 0) vs = (int)i;
        else if (c.stage == 1) fs = (int)i;
        else other = true;
    }
    if (vs < 0 || other) {
        s->why = vs < 0 ? "no vertex stage (mesh, task or compute)" : "a geometry or tessellation stage";
        return s->state.store(2, std::memory_order_release);
    }
    s->vs.key = codes[vs].key, std::memcpy(s->vs.sph, codes[vs].sph, sizeof s->vs.sph);
    if (fs >= 0) s->has_fs = true, s->fs.key = codes[fs].key, std::memcpy(s->fs.sph, codes[fs].sph, sizeof s->fs.sph);
    const uint64_t pair = pair_key(s->vs.key, s->has_fs ? s->fs.key : 0);
    bool fresh = false;
    {
        std::lock_guard lk(families_mu);
        auto [it, made] = families.try_emplace(pair, nullptr);
        if (made) it->second = new Family, st_families.fetch_add(1, std::memory_order_relaxed);
        s->fam = it->second, fresh = made;
    }
    if (!fresh) {   // (the code's variants are made by the slot that saw it first; this one waits for them, on its worker)
        while (!s->fam->loaded.load(std::memory_order_acquire)) std::this_thread::yield();
        return s->state.store(1, std::memory_order_release);
    }
    disk_init();
    std::vector<DiskRecord> recs;
    {
        std::lock_guard lk(disk_mu);
        const auto it = disk_by_pair.find(pair);
        if (it != disk_by_pair.end()) recs = it->second;
    }
    for (const DiskRecord& d : recs) {
        auto v = std::make_unique<Variant>();
        const auto t0 = clk::now();
        const bool read = disk_read(d, *v);
        st_disk_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(), std::memory_order_relaxed);
        if (!read || (v->stages.size() == 2) != s->has_fs) continue;
        v->d.variant = 0;
        for (const RawAnswer& a : v->answers) v->d.variant = (v->d.variant ^ ((uint64_t)a.stage << 32 | (uint64_t)a.desc << 16 | a.type)) * 1099511628211ull;
        if (!finish(*v)) continue;
        st_from_disk.fetch_add(1, std::memory_order_relaxed);
        publish(*s->fam, v.release(), s->serial);
    }
    s->fam->loaded.store(true, std::memory_order_release);
    s->state.store(1, std::memory_order_release);
}

Slot* slot_make(const nvn_prog& p) {
    std::lock_guard lk(table_mu);
    if (!table) {
        table.reset(new std::atomic<Slot*>[kSlots]);
        for (uint32_t i = 0; i < kSlots; i++) table[i].store(nullptr, std::memory_order_relaxed);
    }
    if (Slot* s = slot_find(p.serial)) return s;
    if (st_slots.load(std::memory_order_relaxed) >= (int64_t)(kSlots * 3 / 4)) return st_full.fetch_add(1, std::memory_order_relaxed), nullptr;
    Slot* s = new Slot;
    s->serial = p.serial, s->prog = p;
    for (uint32_t k = slot_hash(p.serial);; k = (k + 1) & (kSlots - 1))
        if (!table[k].load(std::memory_order_relaxed)) {
            table[k].store(s, std::memory_order_release);
            break;
        }
    st_slots.fetch_add(1, std::memory_order_relaxed);
    push_work([s] { scan_slot(s); });
    return s;
}

/// The live answers' hash for the answers a variant of this key checks (what a new variant would be translated for)
uint64_t live_hash(const DState& s, const DrawProg& p) {
    uint64_t h = 0;
    for (const DrawProg::Answer& a : p.answers) {
        const uint32_t hd = bank_word(s, a.bank, a.stage, a.offset);
        h = (h ^ (hd ? canonical(live_answer(hd), a.dim_fixed, a.desc_type, a.depth_float) : 0xFFu)) * 1099511628211ull;
    }
    return h;
}

}  // namespace

void shader_program_set(const nvn_prog* p) {
    if (!p || !heap_on() || !p->nstage) return;
    nvn_prog copy;
    std::memcpy(&copy, p, sizeof copy);
    slot_make(copy);
}

bool shader_answers_hold(const DState& s, const DrawProg& p) {
    for (const DrawProg::Answer& a : p.answers) {
        const uint32_t h = bank_word(s, a.bank, a.stage, a.offset);
        if (h && canonical(live_answer(h), a.dim_fixed, a.desc_type, a.depth_float) != a.want) return false;
    }
    return true;
}

const DrawProg* shader_pick(const DState& s, const uint8_t color_types[8], ShaderMiss& why) {
    if (!s.has_prog) return why = SM_NO_PROGRAM, nullptr;
    Slot* slot = slot_find(s.prog.serial);
    if (!slot) slot = heap_on() ? slot_make(s.prog) : nullptr;
    if (!slot) return why = SM_NOT_PARSED, nullptr;
    const int state = slot->state.load(std::memory_order_acquire);
    if (state == 0) return why = SM_NOT_PARSED, nullptr;
    if (state != 1) return why = SM_NOT_GRAPHICS, nullptr;
    const uint64_t key = graphics_key(slot->vs.key, slot->vs.sph, slot->fs.key, slot->has_fs ? slot->fs.sph : nullptr, s.vattr, color_types);
    const Variant* same_key = nullptr;
    for (const Variant* v = slot->fam->variants.load(std::memory_order_acquire); v; v = v->next) {
        if (v->d.key != key) continue;
        same_key = v;
        if (shader_answers_hold(s, v->d)) return why = SM_NONE, &v->d;
    }
    // (none: translated on a worker, once for these inputs and answers)
    const uint64_t want = key ^ (same_key ? live_hash(s, same_key->d) * 0x9E3779B97F4A7C15ull : 0);
    st_misses.fetch_add(1, std::memory_order_relaxed);
    last_miss_s.store(seconds_since_device(), std::memory_order_relaxed);
    {
        std::lock_guard lk(slot->fam->mu);
        if (slot->fam->failed.count(want)) return why = SM_FAILED, nullptr;
        if (!slot->fam->pending.insert(want).second) return why = SM_TRANSLATING, nullptr;
    }
    struct Job {
        Slot* slot;
        uint64_t key, want;
        DState s;
        uint8_t ct[8];
    };
    auto job = std::make_shared<Job>();
    job->slot = slot, job->key = key, job->want = want, job->s = s;
    std::memcpy(job->ct, color_types, 8);
    st_queued.fetch_add(1, std::memory_order_relaxed);
    push_work([job] {
        const auto t0 = clk::now();
        Slot& sl = *job->slot;
        Code vsc, fsc;
        bool ok = false;
        auto v = std::make_unique<Variant>();
        v->d.key = job->key;
        int vs = -1, fs = -1;
        Code codes[6];
        for (uint32_t i = 0; i < sl.prog.nstage && i < 6; i++)
            if (parse_code(sl.prog.sd[i], codes[i]) && !codes[i].header_only) {
                if (codes[i].stage == 0) vs = (int)i;
                if (codes[i].stage == 1) fs = (int)i;
            }
        if (vs >= 0 && (fs >= 0) == sl.has_fs && translate_variant(*v, job->s, codes[vs], fs >= 0 ? &codes[fs] : nullptr, job->ct)) {
            for (const RawAnswer& a : v->answers) v->d.variant = (v->d.variant ^ ((uint64_t)a.stage << 32 | (uint64_t)a.desc << 16 | a.type)) * 1099511628211ull;
            disk_save(*v, pair_key(sl.vs.key, sl.has_fs ? sl.fs.key : 0));
            ok = finish(*v);
        }
        st_translate_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count(), std::memory_order_relaxed);
        if (ok) {
            st_translated.fetch_add(1, std::memory_order_relaxed);
            publish(*sl.fam, v.release(), sl.serial);
        } else {
            st_failed.fetch_add(1, std::memory_order_relaxed);
        }
        std::lock_guard lk(sl.fam->mu);
        sl.fam->pending.erase(job->want);
        if (!ok) sl.fam->failed.insert(job->want);
    });
    return why = SM_TRANSLATING, nullptr;
}

void shader_stats(char* out, size_t cap, double frames) {
    size_t queued = 0;
    {
        std::lock_guard lk(work_mu);
        queued = work.size();
    }
    std::snprintf(out, cap, "programs (since the start): %lld set (%lld distinct codes), %lld variants translated (%.1f s on the workers), %lld failed, %lld "
                  "from the cache (%.1f s read), %lld shader objects made (%.1f s, %lld failed), %lld stages unreadable, table full %lld; misses %.1f a frame, "
                  "%zu jobs waiting; variants past a program's first: %lld for other inputs, %lld for other answers; game buffer words "
                  "asked at translation %lld; uniform buffers a variant reads: %.1f on average, %lld at most, %lld variants with more "
                  "than the push data's room for their addresses; the last miss %.2f s after the device", (long long)st_slots.load(), (long long)st_families.load(), (long long)st_translated.load(), st_translate_ns.load() / 1e9,
                  (long long)st_failed.load(), (long long)st_from_disk.load(), st_disk_ns.load() / 1e9, (long long)st_objects_made.load(),
                  st_object_ns.load() / 1e9,
                  (long long)st_object_failed.load(), (long long)st_scan_failed.load(), (long long)st_full.load(),
                  st_misses.exchange(0) / frames, queued, (long long)st_multi_inputs.load(), (long long)st_multi_answers.load(),
                  (long long)st_game_words.load(), st_ubos_variants.load() ? (double)st_ubos_sum.load() / st_ubos_variants.load() : 0.0,
                  (long long)st_ubos_max.load(), (long long)st_ubos_over.load(), last_miss_s.load());
}

}  // namespace hsd
