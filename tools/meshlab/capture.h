// meshlab: reading a frame captured by HOS_DBG=meshcap (gpu/direct/direct_meshcap_format.h).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../gpu/direct/direct_meshcap_format.h"

namespace meshlab {

struct Stage {
    mcap::McStage h{};
    std::vector<uint32_t> spirv;
    std::vector<uint8_t> maps;   // (VkDescriptorSetAndBindingMappingEXT[h.maps])
    std::vector<uint8_t> c1;
};
struct Program {
    mcap::McProgram h{};
    std::vector<mcap::McProgUbo> ubos;
    std::vector<mcap::McPushed> pushed;
    std::vector<Stage> stages;
    const Stage* stage(uint32_t vk_stage) const {
        for (const Stage& s : stages)
            if (s.h.vk_stage == vk_stage) return &s;
        return nullptr;
    }
};
struct Blob {
    uint64_t guest = 0;
    std::vector<uint8_t> bytes;
};
struct Draw {
    bool mesh = false;
    mcap::McDraw d{};
    mcap::McOther o{};
    std::vector<mcap::McDrawUbo> ubos;
    std::vector<mcap::McDrawSsbo> ssbos;
    std::vector<uint32_t> map, payload;
    uint32_t seq() const { return mesh ? d.seq : o.seq; }
    uint64_t cb() const { return mesh ? d.cb : o.cb; }
    uint32_t cb_seq() const { return mesh ? d.cb_seq : o.cb_seq; }
    uint64_t key() const { return mesh ? d.key : o.key; }
};
struct Frame {
    mcap::McFile h{};
    std::map<uint64_t, Program> programs;
    std::vector<Blob> blobs;
    std::vector<Draw> draws;   // (in recording order)
    const Draw* by_seq(uint32_t s) const {
        for (const Draw& d : draws)
            if (d.seq() == s) return &d;
        return nullptr;
    }
};

inline bool load(const char* path, Frame& f, std::string& err) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return err = std::string("cannot open ") + path, false;
    std::fseek(fp, 0, SEEK_END);
    const long n = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> b((size_t)n);
    if (std::fread(b.data(), 1, b.size(), fp) != b.size()) return std::fclose(fp), err = "short read", false;
    std::fclose(fp);
    size_t at = 0;
    auto take = [&](void* dst, size_t k) {
        if (at + k > b.size()) return false;
        std::memcpy(dst, b.data() + at, k);
        at += k;
        return true;
    };
    if (!take(&f.h, sizeof f.h) || f.h.magic != mcap::kMagic || f.h.version != mcap::kVersion) return err = "not a meshcap file of this version", false;
    while (at < b.size()) {
        mcap::McRecord r;
        if (!take(&r, sizeof r)) return err = "truncated record", false;
        const size_t end = at + r.bytes;
        if (end > b.size()) return err = "truncated payload", false;
        if (r.type == mcap::MC_PROGRAM) {
            Program p;
            take(&p.h, sizeof p.h);
            p.ubos.resize(p.h.nubo), p.pushed.resize(p.h.npushed);
            take(p.ubos.data(), p.ubos.size() * sizeof p.ubos[0]);
            take(p.pushed.data(), p.pushed.size() * sizeof p.pushed[0]);
            for (uint32_t i = 0; i < p.h.nstages; i++) {
                Stage s;
                take(&s.h, sizeof s.h);
                s.spirv.resize(s.h.spirv_words), s.maps.resize((size_t)s.h.maps * p.h.map_size), s.c1.resize(s.h.c1_bytes);
                take(s.spirv.data(), s.spirv.size() * 4), take(s.maps.data(), s.maps.size()), take(s.c1.data(), s.c1.size());
                p.stages.push_back(std::move(s));
            }
            f.programs[p.h.key] = std::move(p);
        } else if (r.type == mcap::MC_BLOB) {
            mcap::McBlob h;
            take(&h, sizeof h);
            if (f.blobs.size() <= h.id) f.blobs.resize(h.id + 1);
            f.blobs[h.id].guest = h.guest;
            f.blobs[h.id].bytes.resize(h.bytes);
            take(f.blobs[h.id].bytes.data(), h.bytes);
        } else if (r.type == mcap::MC_DRAW) {
            Draw d;
            d.mesh = true;
            take(&d.d, sizeof d.d);
            d.ubos.resize(d.d.nubo), d.ssbos.resize(d.d.nssbo), d.map.resize(d.d.nmap), d.payload.resize(d.d.npayload);
            take(d.ubos.data(), d.ubos.size() * sizeof d.ubos[0]);
            take(d.ssbos.data(), d.ssbos.size() * sizeof d.ssbos[0]);
            take(d.map.data(), d.map.size() * 4), take(d.payload.data(), d.payload.size() * 4);
            f.draws.push_back(std::move(d));
        } else if (r.type == mcap::MC_OTHER) {
            Draw d;
            take(&d.o, sizeof d.o);
            f.draws.push_back(std::move(d));
        }
        at = end;
    }
    return true;
}

}  // namespace meshlab
