// irexec's command: stages of a shader seed (tools/shadercensus/seed.py) through a front as the back end translates
// them (tools/shaders/seed.h, the IR as EmitSPIRV gets it), then through the interpreter (irexec.h) with N input seeds.
//
//   irexec <seed> --key <hex> [--inputs N] [--trace] [--ir]   one stage by code key: each input seed's trace hash
//   irexec <seed> --index <i> [same]                          one stage by seed index
//   irexec <seed> --all [--first F] [--count C] [--inputs N]   every stage: a line per failure, then the totals
//   irexec <seed> --opcodes [--first F] [--count C]            the post-pass opcodes of the stages: stages and uses each
//   --alias <a>=<b>: constant bank a read and keyed as bank b (irexec::Options::bank_alias)
//   --front old|new: the front that translates (seed.h; old, the lowering to Maxwell, by default)
//   --lanes N: lanes that start (32); --vertices N: a geometry stage's input primitive's vertex count (3)
//   --attr <lane|*>,<vertex>,<byte address>=<value>: an input attribute (the address as ALD's, 0x70 position x)
//   --cbuf <bank>,<offset>=<value>: a constant word; a value is hex bits (0x3f800000) or a float (1.5f)
//   irexec --supported                                         the opcodes the interpreter executes
// "@@stage <index>" goes to stderr before each stage (a crash names its stage).
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../seed.h"
#include "irexec.h"

namespace {
using namespace Shader::IR;

seed::Seed sd;
const seed::Front* front = &seed::kOld;
uint64_t key_of(const seed::Stage& s) { return s.key(); }
seed::Translated translate(int id) { return seed::translate(sd, id, *front); }

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

irexec::Options options;
irexec::Inputs base;   // (the inputs every run starts from: --lanes, --vertices, --attr, --cbuf)

uint32_t value_of(const std::string& v) {
    if (!v.empty() && v.back() == 'f') {
        const float f = std::stof(v.substr(0, v.size() - 1));
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }
    return (uint32_t)std::stoul(v, nullptr, 0);
}
/// "a,b,c=value" split into its numbers before '=' ('*' is ~0) and the value
std::vector<uint32_t> fields_of(const std::string& a, uint32_t& value) {
    const size_t eq = a.find('=');
    if (eq == std::string::npos) throw Shader::LogicError("{}: no value", a);
    value = value_of(a.substr(eq + 1));
    std::vector<uint32_t> out;
    size_t at = 0;
    while (at <= eq) {
        size_t end = a.find(',', at);
        if (end == std::string::npos || end > eq) end = eq;
        const std::string f = a.substr(at, end - at);
        out.push_back(f == "*" ? ~0u : (uint32_t)std::stoul(f, nullptr, 0));
        at = end + 1;
    }
    return out;
}

int one(int id, int inputs, bool trace, bool ir) {
    seed::Translated s = translate(id);
    if (ir) std::printf("%s\n", Shader::IR::DumpProgram(s.t->program).c_str());
    const auto c = irexec::compile(s.t->program, options);
    for (int k = 0; k < inputs; k++) {
        irexec::Inputs in = base;
        in.seed = (uint64_t)k;
        in.c1 = &s.code.c1;
        in.log = trace;
        const auto t0 = std::chrono::steady_clock::now();
        const irexec::Trace tr = irexec::run(*c, in);
        std::printf("input %d: %016" PRIx64 " steps %" PRIu64 " insts %" PRIu64 "%s undefined %u (%.2f ms)\n", k, tr.hash, tr.steps,
                    tr.insts, tr.capped ? " capped" : "", tr.undefined_reads, ms_since(t0));
        if (trace) {
            for (const auto& l : tr.log) std::printf("  %s\n", l.c_str());
            for (const auto& [k2, v] : tr.state) std::printf("  = %s %s\n", k2.c_str(), v.c_str());
        }
    }
    return 0;
}

int all(int first, int count, int inputs) {
    size_t translated = 0, failed_translate = 0, ran = 0, failed = 0, capped = 0;
    uint64_t insts = 0;
    double t_translate = 0, t_run = 0, worst = 0;
    int worst_id = -1;
    std::map<std::string, int> reasons;
    const int end = std::min<int>(first + count, (int)sd.stages.size());
    for (int id = first; id < end; id++) {
        std::fprintf(stderr, "@@stage %d\n", id);
        seed::Translated s;
        auto t0 = std::chrono::steady_clock::now();
        try {
            s = translate(id);
        } catch (const std::exception&) {
            failed_translate++;
            continue;
        }
        t_translate += ms_since(t0);
        translated++;
        t0 = std::chrono::steady_clock::now();
        try {
            const auto c = irexec::compile(s.t->program, options);
            for (int k = 0; k < inputs; k++) {
                irexec::Inputs in = base;
                in.seed = (uint64_t)k;
                in.c1 = &s.code.c1;
                const irexec::Trace tr = irexec::run(*c, in);
                insts += tr.insts;
                capped += tr.capped;
            }
            ran++;
        } catch (const std::exception& e) {
            failed++;
            reasons[e.what()]++;
            std::printf("fail %d %016" PRIx64 ": %s\n", id, key_of(sd.stages[(size_t)id]), e.what());
        }
        const double ms = ms_since(t0);
        t_run += ms;
        if (ms > worst) worst = ms, worst_id = id;
    }
    std::printf("stages %d..%d: translated %zu (not %zu), ran %zu, failed %zu, capped runs %zu, inputs %d\n", first, end, translated,
                failed_translate, ran, failed, capped, inputs);
    std::printf("time: translate %.0f ms, interpret %.0f ms (%.3f ms a run), slowest stage %d %.1f ms; %" PRIu64 " instructions\n", t_translate,
                t_run, ran ? t_run / (double)(ran * (size_t)inputs) : 0.0, worst_id, worst, insts);
    for (const auto& [r, n] : reasons) std::printf("  %6d %s\n", n, r.c_str());
    return failed ? 1 : 0;
}

int opcodes(int first, int count) {
    std::map<std::string, std::pair<uint64_t, uint64_t>> hist;   // (stages, uses)
    const int end = std::min<int>(first + count, (int)sd.stages.size());
    size_t n = 0;
    for (int id = first; id < end; id++) {
        std::fprintf(stderr, "@@stage %d\n", id);
        seed::Translated s;
        try {
            s = translate(id);
        } catch (const std::exception&) {
            continue;
        }
        n++;
        std::set<std::string> here;
        for (Block* b : s.t->program.blocks)
            for (Inst& i : b->Instructions()) {
                std::string name(NameOf(i.GetOpcode()));
                hist[name].second++;
                here.insert(name);
            }
        for (const auto& h : here) hist[h].first++;
    }
    std::printf("# %zu stages\n", n);
    for (const auto& [name, v] : hist) std::printf("%s %" PRIu64 " %" PRIu64 "\n", name.c_str(), v.first, v.second);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && !std::strcmp(argv[1], "--supported")) {
        for (const auto& o : irexec::supported_opcodes()) std::printf("%s\n", o.c_str());
        return 0;
    }
    if (argc < 3) return std::fprintf(stderr, "irexec <seed> --key <hex>|--index <i>|--all|--opcodes [options]\n"), 2;
    if (!seed::read(argv[1], sd)) return std::fprintf(stderr, "cannot read the seed %s\n", argv[1]), 1;
    int first = 0, count = 1 << 30, inputs = 3, index = -1;
    bool trace = false, ir = false, do_all = false, do_opcodes = false;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--key" && more) {
            const uint64_t key = std::strtoull(argv[++i], nullptr, 16);
            for (size_t k = 0; k < sd.stages.size() && index < 0; k++)
                if (key_of(sd.stages[k]) == key) index = (int)k;
            if (index < 0) return std::fprintf(stderr, "no stage with key %s\n", argv[i]), 1;
        } else if (a == "--index" && more) index = std::atoi(argv[++i]);
        else if (a == "--inputs" && more) inputs = std::atoi(argv[++i]);
        else if (a == "--first" && more) first = std::atoi(argv[++i]);
        else if (a == "--count" && more) count = std::atoi(argv[++i]);
        else if (a == "--alias" && more) {
            const std::string v = argv[++i];
            const size_t eq = v.find('=');
            if (eq == std::string::npos) return std::fprintf(stderr, "--alias <a>=<b>\n"), 2;
            options.bank_alias[(uint32_t)std::stoul(v.substr(0, eq))] = (uint32_t)std::stoul(v.substr(eq + 1));
        } else if (a == "--front" && more) front = &seed::front(argv[++i]);
        else if (a == "--lanes" && more) base.lanes = (uint32_t)std::atoi(argv[++i]);
        else if (a == "--vertices" && more) base.primitive_vertices = (uint32_t)std::atoi(argv[++i]);
        else if ((a == "--attr" || a == "--cbuf") && more) {
            uint32_t value;
            const std::vector<uint32_t> f = fields_of(argv[++i], value);
            if (a == "--attr" && f.size() == 3) base.attributes[{f[0], f[1], f[2] / 4}] = value;
            else if (a == "--cbuf" && f.size() == 2) base.cbuf[{f[0], f[1] & ~3u}] = value;
            else return std::fprintf(stderr, "%s: %s\n", a.c_str(), argv[i]), 2;
        }
        else if (a == "--trace") trace = true;
        else if (a == "--ir") ir = true;
        else if (a == "--all") do_all = true;
        else if (a == "--opcodes") do_opcodes = true;
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
    }
    try {
        if (do_opcodes) return opcodes(first, count);
        if (do_all) return all(first, count, inputs);
        if (index < 0 || index >= (int)sd.stages.size()) return std::fprintf(stderr, "no stage given\n"), 2;
        std::printf("stage %d key %016" PRIx64 " kind %u\n", index, key_of(sd.stages[(size_t)index]), sd.stages[(size_t)index].stage);
        return one(index, inputs, trace, ir);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
