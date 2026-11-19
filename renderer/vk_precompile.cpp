// Shaders ahead of the game: the shader pack beside the executable (shaders.pack, made by the builder from the romfs,
// ounce/shaders) read, and each of its graphics programs made as its first draw would make it (vk_shader.h
// shader_precompile), into the program cache and the driver binaries. Two ways in:
// - the game's --precompile (mkwvk_precompile): every program on every core, the build's last step, so the first boot is
//   warm;
// - at every start (shader_warm): when the caches' stamp (the translator, the device, the driver's binary UUID and
//   version) is not the one the last complete precompile wrote into ounce_renderer_shaders.stamp, as after a driver
//   update, the programs are made again on the background workers while the game plays.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "os.h"
#include "vk_api.h"
#include "vk_internal.h"
#include "vk_shader.h"
#include "vk_shader_parts.h"
#include "vk_work.h"

namespace hsd {
namespace {

/// The pack, mapped: its programs' stages (the seed format: "HKSEED01", u32 programs, u32 stages; each stage {u64 key,
/// u32 NVN stage, u32 control bytes, u32 code bytes, control, code}; each program {u32 count, u32 stage indices})
struct Pack {
    std::vector<PreStage> stages;
    std::vector<std::vector<uint32_t>> programs;
};

bool pack_read(const std::string& path, Pack& p) {
    os_file* f = os_file_open(path.c_str(), OS_READ, nullptr);
    if (!f) return false;
    size_t n = 0;
    const uint8_t* d = (const uint8_t*)os_file_map_read(f, &n);   // (kept mapped: the stages point into it)
    os_file_close(f);
    if (!d || n < 16 || std::memcmp(d, "HKSEED01", 8)) return false;
    uint32_t programs, stages;
    std::memcpy(&programs, d + 8, 4), std::memcpy(&stages, d + 12, 4);
    size_t at = 16;
    const auto u32 = [&](uint32_t& v) { return at + 4 <= n ? (std::memcpy(&v, d + at, 4), at += 4, true) : false; };
    for (uint32_t i = 0; i < stages; i++) {
        uint32_t kind, cn, xn;
        at += 8;   // (the key: shader_precompile computes its own)
        if (!u32(kind) || !u32(cn) || !u32(xn) || cn != 0xd8 || at + cn + xn > n) return false;
        p.stages.push_back({d + at, d + at + cn, xn});
        at += cn + xn;
    }
    for (uint32_t i = 0; i < programs; i++) {
        uint32_t c, id;
        if (!u32(c) || c > 8) return false;
        std::vector<uint32_t>& ids = p.programs.emplace_back();
        for (uint32_t k = 0; k < c; k++) {
            if (!u32(id) || id >= stages) return false;
            ids.push_back(id);
        }
    }
    return true;
}

PreResult make(const Pack& p, const std::vector<uint32_t>& ids) {
    PreStage st[8];
    for (size_t k = 0; k < ids.size(); k++) st[k] = p.stages[ids[k]];
    return shader_precompile(st, (int)ids.size());
}

std::string stamp_path() { return shader_parts::program_dir() + "ounce_renderer_shaders.stamp"; }
std::string pack_path() { return shader_parts::program_dir() + "shaders.pack"; }

/// The pack's size, without reading it (0: none)
uint64_t pack_size() {
    os_file* f = os_file_open(pack_path().c_str(), OS_READ, nullptr);
    const int64_t n = f ? os_file_size(f) : 0;
    os_file_close(f);
    return n > 0 ? (uint64_t)n : 0;
}
/// What the stamp file holds after a complete precompile of a pack of this size with these caches
std::string stamp_text(uint64_t pack_bytes) {
    char b[64];
    std::snprintf(b, sizeof b, "%016llx %llu\n", (unsigned long long)shader_cache_stamp(), (unsigned long long)pack_bytes);
    return b;
}
std::string read_text(const std::string& path) {
    std::string s;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char b[256];
        s.assign(b, std::fread(b, 1, sizeof b, f));
        std::fclose(f);
    }
    return s;
}
void write_text(const std::string& path, const std::string& s) {
    const std::string tmp = path + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
        os_rename(tmp.c_str(), path.c_str(), true);
    }
}

struct Counts {
    std::atomic<int> done{0}, made{0}, kept{0}, failed{0}, other{0};
    void add(PreResult r) {
        (r == PRE_MADE ? made : r == PRE_KEPT ? kept : r == PRE_FAILED ? failed : other).fetch_add(1, std::memory_order_relaxed);
        done.fetch_add(1, std::memory_order_relaxed);
    }
};

}  // namespace

/// (the start) the pack's programs made again on the background workers when the caches are not the ones its last
/// complete precompile filled
void shader_warm() {
    const uint64_t size = pack_size();
    if (!size) return;
    const std::string want = stamp_text(size);   // (the pack is read only when the caches are not its)
    if (read_text(stamp_path()) == want) return logf("direct: shaders: the caches hold the shader pack's programs");
    auto p = std::make_shared<Pack>();
    if (!pack_read(pack_path(), *p)) return logf("direct: shaders: the shader pack %s is not one", pack_path().c_str());
    logf("direct: shaders: the caches are not the pack's (a new driver or renderer, or no precompile): its %zu programs made "
         "again on the background workers", p->programs.size());
    auto c = std::make_shared<Counts>();
    const size_t n = p->programs.size(), chunk = 64;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t at = 0; at < n; at += chunk)
        work::run(work::P2, [p, c, at, n, chunk, want, t0] {
            for (size_t i = at; i < std::min(n, at + chunk); i++) c->add(make(*p, p->programs[i]));
            if ((size_t)c->done.load() == n) {
                shader_cache_index();
                write_text(stamp_path(), want);
                logf("direct: shaders: the pack's programs made in %.1f s: %d translated, %d kept, %d failed, %d not drawn so (compute)",
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), c->made.load(), c->kept.load(),
                     c->failed.load(), c->other.load());
            }
        });
}

}  // namespace hsd

extern "C" const MkwVkRenderer* mkwvk_init(const MkwVkHost* h);   // (vk_main.cpp)

/// The game's --precompile: the device made (no window, no game), then every program of the pack beside the executable
/// on every core; progress(done, of, translated, kept, failed) as they finish. 0 when the pack was made whole
extern "C" __attribute__((visibility("default"))) int mkwvk_precompile(const MkwVkHost* h, void (*progress)(int done, int of, int made, int kept, int failed)) {
    using namespace hsd;
    if (!mkwvk_init(h)) return logf("direct: precompile: the device was not made"), 1;
    const uint64_t size = pack_size();
    if (!size) return logf("direct: precompile: no shader pack %s", pack_path().c_str()), 1;
    const std::string want = stamp_text(size);
    if (read_text(stamp_path()) == want) {
        if (progress) progress(0, 0, 0, 0, 0);   // (nothing to do)
        return logf("direct: precompile: the caches hold the shader pack's programs"), 0;
    }
    Pack p;
    if (!pack_read(pack_path(), p)) return logf("direct: precompile: the shader pack %s is not one", pack_path().c_str()), 1;
    Counts c;
    std::atomic<size_t> next{0};
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (unsigned w = 0; w < threads; w++)
        ts.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < p.programs.size();) c.add(make(p, p.programs[i]));
        });
    for (int last = -1; c.done.load() < (int)p.programs.size();) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (progress && c.done.load() != last) progress(last = c.done.load(), (int)p.programs.size(), c.made.load(), c.kept.load(), c.failed.load());
    }
    for (auto& t : ts) t.join();
    if (progress) progress((int)p.programs.size(), (int)p.programs.size(), c.made.load(), c.kept.load(), c.failed.load());
    shader_cache_index();
    write_text(stamp_path(), want);
    logf("direct: precompile: %zu programs in %.1f s on %u threads: %d translated, %d kept, %d failed, %d not drawn so (compute)",
         p.programs.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), threads, c.made.load(),
         c.kept.load(), c.failed.load(), c.other.load());
    return 0;
}
