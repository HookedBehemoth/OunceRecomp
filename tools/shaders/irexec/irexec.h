// irexec: an interpreter for the translator's post-pass IR (an IR::Program as the SPIR-V back end gets it), for the
// Ampere decoder's differential (work/shader-plan, section 3). One subgroup of 32 lanes runs in lockstep through the
// structured control flow; its inputs (constant banks, attributes, memory, texels) are hashes of the input seed and of
// where they are read, the program's own c1 where it is given; every side effect lands in a trace whose hash two
// translations of one stage can compare. What the interpreter does where the SPIR-V leaves a choice, and where its float
// results may differ from RADV's, is listed at the top of irexec.cpp. An op it does not know is an Error.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace Shader::IR {
struct Program;
}

namespace irexec {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Inputs {
    uint64_t seed = 0;
    const std::vector<uint8_t>* c1 = nullptr;   // (bank 1's bytes; null: bank 1 is synthetic like the others)
    uint32_t lanes = 32;                        // (lanes that start; the rest are inactive from the start)
    uint64_t max_steps = 1u << 16;              // (block entries of the subgroup before the run stops as capped)
    bool log = false;                           // (keep the ordered event log as text)
};

struct Trace {
    uint64_t hash = 0;                          // (of the canonical state below)
    std::map<std::string, std::string> state;   // (the final side effects, keyed: "out", "mem", "atomic", ...)
    std::vector<std::string> log;               // (Inputs::log: every side effect in execution order)
    uint64_t steps = 0, insts = 0;
    bool capped = false;
    uint32_t undefined_reads = 0;               // (shuffles and derivatives from lanes that were not active)
};

struct Options {
    std::map<uint32_t, uint32_t> bank_alias;   // (constant banks read and keyed as another: {24, 17} compares a root table at 24 with 17)
};

class Compiled;
/// The program prepared for runs (its slots and blocks); it holds pointers into the program
std::shared_ptr<Compiled> compile(const Shader::IR::Program& program, const Options& options = {});
Trace run(const Compiled& c, const Inputs& in);

/// The IR opcodes the interpreter executes
std::vector<std::string> supported_opcodes();

}  // namespace irexec
