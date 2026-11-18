// A shader seed (tools/shadercensus/seed.py: "HKSEED01", the stages, then the programs) read and written, and its stages
// translated as the back end translates them (offline.h) by a front: the old one (the lowering to Maxwell) or the direct
// Ampere front (work/shader-plan M3). Shared by irexec's command and the differential (tools/shaders/difftest).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "offline.h"

namespace seed {

struct Stage {
    uint32_t stage = 0;   // (NVN kind: 0 vertex, 1 fragment, 5 compute, 6 task, 7 mesh)
    const uint8_t* ctl = nullptr;
    const uint8_t* code = nullptr;
    size_t ctl_n = 0, code_n = 0;
    uint64_t key() const { return offline::fnv(code, code_n, offline::fnv(ctl, offline::kControl)); }
};
struct Partners {   // (the first program that uses a stage: a mesh stage's task, a task stage's mesh)
    int task = -1, mesh = -1;
};
struct Seed {
    std::vector<uint8_t> blob;
    std::vector<Stage> stages;
    std::vector<std::vector<uint32_t>> programs;
    std::vector<Partners> partners;
};

inline bool read(const char* path, Seed& s) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    s.blob.resize((size_t)std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    const bool got = std::fread(s.blob.data(), 1, s.blob.size(), f) == s.blob.size();
    std::fclose(f);
    if (!got || s.blob.size() < 16 || std::memcmp(s.blob.data(), "HKSEED01", 8)) return false;
    uint32_t np, ns;
    std::memcpy(&np, &s.blob[8], 4), std::memcpy(&ns, &s.blob[12], 4);
    size_t at = 16;
    for (uint32_t i = 0; i < ns; i++) {
        uint32_t st, cn, bn;
        std::memcpy(&st, &s.blob[at + 8], 4), std::memcpy(&cn, &s.blob[at + 12], 4), std::memcpy(&bn, &s.blob[at + 16], 4);
        Stage x;
        x.stage = st, x.ctl = &s.blob[at + 20], x.ctl_n = cn, x.code = x.ctl + cn, x.code_n = bn;
        s.stages.push_back(x);
        at += 20 + cn + bn;
    }
    for (uint32_t i = 0; i < np; i++) {
        uint32_t n;
        std::memcpy(&n, &s.blob[at], 4);
        std::vector<uint32_t> ids(n);
        std::memcpy(ids.data(), &s.blob[at + 4], 4 * n);
        s.programs.push_back(std::move(ids));
        at += 4 + 4 * n;
    }
    s.partners.assign(s.stages.size(), {});
    std::vector<bool> seen(s.stages.size());
    for (const auto& prog : s.programs) {
        Partners p;
        for (uint32_t id : prog) {
            if (s.stages[id].stage == 6) p.task = (int)id;
            if (s.stages[id].stage == 7) p.mesh = (int)id;
        }
        for (uint32_t id : prog)
            if (!seen[id]) seen[id] = true, s.partners[id] = p;
    }
    return true;
}

/// A seed of stages that stand alone (no programs): each a kind, a control section and a code area
struct Written {
    uint32_t stage = 0;
    std::vector<uint8_t> ctl, code;
};
inline bool write(const char* path, const std::vector<Written>& stages) {
    std::vector<uint8_t> b(16);
    std::memcpy(b.data(), "HKSEED01", 8);
    const uint32_t np = 0, ns = (uint32_t)stages.size();
    std::memcpy(&b[8], &np, 4), std::memcpy(&b[12], &ns, 4);
    for (const Written& w : stages) {
        const uint64_t key = offline::fnv(w.code.data(), w.code.size(), offline::fnv(w.ctl.data(), offline::kControl));
        const uint32_t head[3] = {w.stage, (uint32_t)w.ctl.size(), (uint32_t)w.code.size()};
        const size_t at = b.size();
        b.resize(at + 20);
        std::memcpy(&b[at], &key, 8), std::memcpy(&b[at + 8], head, 12);
        b.insert(b.end(), w.ctl.begin(), w.ctl.end());
        b.insert(b.end(), w.code.begin(), w.code.end());
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    return std::fclose(f) == 0 && ok;
}

/// A front: how a stage's code is read (offline::load's work) and its IR made (offline::translate's). Both are called for
/// every stage, the partner stages of task and mesh included.
struct Front {
    const char* name;
    offline::Code (*load)(const uint8_t* ctl, const uint8_t* area, size_t n);
    std::unique_ptr<offline::Translation> (*translate)(offline::Env& env);
};
inline const Front kOld{"old", offline::load, offline::translate};

/// The direct Ampere front (work/shader-plan M3). Until it lands, choosing it fails here, before any stage is read.
inline constexpr bool kNewFrontBuilt = false;
inline offline::Code new_load(const uint8_t*, const uint8_t*, size_t) {
    throw Shader::LogicError("the new front is not built in (tools/shaders/seed.h new_load, work/shader-plan M3)");
}
inline std::unique_ptr<offline::Translation> new_translate(offline::Env&) {
    throw Shader::LogicError("the new front is not built in (tools/shaders/seed.h new_translate, work/shader-plan M3)");
}
inline const Front kNew{"new", new_load, new_translate};

inline const Front& front(const std::string& name) {
    if (name == "old") return kOld;
    if (name == "new") {
        if (!kNewFrontBuilt) throw Shader::LogicError("--front new: the direct Ampere front is not built in yet (work/shader-plan M3)");
        return kNew;
    }
    throw Shader::LogicError("no front {} (old or new)", name);
}

/// A stage translated by a front as the back end translates it: the IR EmitSPIRV gets
struct Translated {
    offline::Code code;
    std::unique_ptr<offline::Translation> t;
};
inline Translated translate(const Seed& s, int id, const Front& f) {
    const auto code_of = [&](int i) {
        const Stage& x = s.stages[(size_t)i];
        return f.load(x.ctl, x.code, x.code_n);
    };
    Translated r;
    r.code = code_of(id);
    if (r.code.header_only) throw Shader::RuntimeError("header only");
    const uint32_t st = s.stages[(size_t)id].stage;
    const Partners& pa = s.partners.empty() ? Partners{} : s.partners[(size_t)id];
    if (st == 0) {
        offline::Env env(r.code, Shader::Stage::VertexB);
        r.t = f.translate(env);
    } else if (st == 1) {
        offline::Env env(r.code, Shader::Stage::Fragment);
        r.t = f.translate(env);
        Shader::Optimization::GuardFragmentOutputs(r.t->program, 0xff);
    } else if (st == 7) {
        offline::Code tc;
        if (pa.task >= 0) tc = code_of(pa.task);
        offline::Env env(r.code, Shader::Stage::Mesh, offline::regs_of(r.code, pa.task >= 0 ? &tc : nullptr));
        r.t = f.translate(env);
    } else if (st == 6) {
        const offline::Code mc = pa.mesh >= 0 ? code_of(pa.mesh) : offline::Code{};
        offline::Env env(r.code, Shader::Stage::Task, offline::regs_of(mc, &r.code));
        r.t = f.translate(env);
    } else if (st == 5) {
        offline::Env env(r.code, Shader::Stage::Compute);
        r.t = f.translate(env);
    } else {
        throw Shader::RuntimeError("stage kind {} not handled", st);
    }
    return r;
}

}  // namespace seed
