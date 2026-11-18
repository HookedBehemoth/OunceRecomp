// The builder's LLVM: a lifted partition's IR text -> an x86-64 ELF object, in this process, as the builder's
// `opt | clang -x ir -c` makes it (the same target machine options, pass pipeline and code generation level).
// libLLVM is linked dynamically; the C API lacks the TargetOptions clang sets (function sections, addrsig), so
// this file builds the target machine through the C++ API.

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

using namespace llvm;

static char *dup_str(const std::string &s) {
    char *p = (char *)malloc(s.size() + 1);
    memcpy(p, s.data(), s.size() + 1);
    return p;
}

static std::once_flag init_once;
static std::string init_error;

// -mllvm options: one set for the process (LLVM's options are global), given before the first compile
extern "C" int hk_llvm_init(const char *const *argv, int argc, char **err) {
    std::call_once(init_once, [&] {
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmPrinter();
        std::string msg;
        raw_string_ostream os(msg);
        if (!cl::ParseCommandLineOptions(argc, argv, "ounce", &os))
            init_error = "the LLVM options: " + msg;
    });
    if (!init_error.empty()) {
        *err = dup_str(init_error);
        return 1;
    }
    return 0;
}

extern "C" const char *hk_llvm_version(void) { return LLVM_VERSION_STRING; }

// clang -cc1's target machine for `-triple x86_64-pc-linux-gnu -target-cpu <cpu> -mrelocation-model pic -pic-level 2
// -pic-is-pie -ffp-contract=off -ffunction-sections -faddrsig` (clang/lib/CodeGen/BackendUtil.cpp initTargetOptions)
static std::unique_ptr<TargetMachine> target_machine(const Triple &triple, const char *cpu, int level, std::string &err) {
    const Target *t = TargetRegistry::lookupTarget(triple, err);
    if (!t) return nullptr;
    TargetOptions o;
    o.AllowFPOpFusion = FPOpFusion::Strict;
    o.UseInitArray = true;
    o.FunctionSections = true;
    o.DataSections = false;
    o.UniqueSectionNames = true;
    o.EmitAddrsig = true;
    o.MCOptions.X86RelaxRelocations = true;
    o.MCOptions.EmitDwarfUnwind = EmitDwarfUnwindType::Default;
    o.DebuggerTuning = DebuggerKind::GDB;
    const CodeGenOptLevel cg = level <= 0 ? CodeGenOptLevel::None
                               : level == 1 ? CodeGenOptLevel::Less
                               : level == 2 ? CodeGenOptLevel::Default
                                            : CodeGenOptLevel::Aggressive;
    return std::unique_ptr<TargetMachine>(t->createTargetMachine(triple, cpu, "", o, Reloc::PIC_, std::nullopt, cg));
}

// One module: IR text (or bitcode) -> the pass pipeline (a PassBuilder text, clang's -print-pipeline-passes; empty: none)
// -> code generation at `level` -> the object's bytes (malloc'd, *out_n). NULL and *err on a failure.
// LoopRotation.cpp's -rotation-max-header-size, read while the pass runs: the caller keeps compiles that want
// different values apart (src/compile.rs)
// an unsigned option of LLVM's (cl::opt<unsigned>, as every one the builder sets is): its value, set first when `set`
extern "C" int hk_llvm_uint_option(const char *name, int set, unsigned v, unsigned *out) {
    auto &all = cl::getRegisteredOptions();
    auto it = all.find(name);
    if (it == all.end()) return 1;
    auto *o = static_cast<cl::opt<unsigned> *>(it->second);
    if (set) o->setValue(v);
    *out = o->getValue();
    return 0;
}

static void set_rotation_threshold(int v) {
    static cl::opt<unsigned> *opt = [] {
        auto &all = cl::getRegisteredOptions();
        auto it = all.find("rotation-max-header-size");
        return it == all.end() ? nullptr : static_cast<cl::opt<unsigned> *>(it->second);
    }();
    if (opt && v >= 0 && (unsigned)v != opt->getValue()) opt->setValue((unsigned)v);
}

// slp: PipelineTuningOptions.SLPVectorization (clang: -vectorize-slp, on from -O2); rotation: the threshold above
// (-1: as it is)
// the last compile's stage seconds on this thread: parse, pipeline, code generation, teardown
static thread_local double stage_s[4];
extern "C" void hk_llvm_stage_seconds(double *out) { memcpy(out, stage_s, sizeof stage_s); }

static double since(std::chrono::steady_clock::time_point &t) {
    auto now = std::chrono::steady_clock::now();
    double s = std::chrono::duration<double>(now - t).count();
    t = now;
    return s;
}

static void *compile(LLVMContext &ctx, const char *ir, size_t n, const char *name, const char *cpu, const char *pipeline,
                     int level, int slp, int rotation, size_t *out_n, char **err, std::chrono::steady_clock::time_point &t);

extern "C" void *hk_llvm_compile(const char *ir, size_t n, const char *name, const char *cpu, const char *pipeline,
                                 int level, int slp, int rotation, size_t *out_n, char **err) {
    auto t = std::chrono::steady_clock::now();
    void *out;
    double module_s;
    {
        auto ctx = std::make_unique<LLVMContext>();   // (textual IR needs its value names: clang keeps them for IR input too)
        out = compile(*ctx, ir, n, name, cpu, pipeline, level, slp, rotation, out_n, err, t);
        module_s = since(t);   // (the module freed as compile returns)
    }
    stage_s[3] = module_s + since(t);   // (and the context)
    return out;
}

static void *compile(LLVMContext &ctx, const char *ir, size_t n, const char *name, const char *cpu, const char *pipeline,
                     int level, int slp, int rotation, size_t *out_n, char **err, std::chrono::steady_clock::time_point &t) {
    SMDiagnostic diag;
    std::unique_ptr<Module> m = parseIR(MemoryBufferRef(StringRef(ir, n), name), diag, ctx);
    stage_s[0] = since(t);
    if (!m) {
        std::string msg;
        raw_string_ostream os(msg);
        diag.print(name, os, false);
        *err = dup_str(msg);
        return nullptr;
    }
    const Triple triple(m->getTargetTriple());
    std::string e;
    auto tm = target_machine(triple, cpu, level, e);
    if (!tm) {
        *err = dup_str("the target machine: " + e);
        return nullptr;
    }
    m->setDataLayout(tm->createDataLayout());

    if (pipeline && *pipeline) {
        set_rotation_threshold(rotation);
        LoopAnalysisManager lam;
        FunctionAnalysisManager fam;
        CGSCCAnalysisManager cgam;
        ModuleAnalysisManager mam;
        PipelineTuningOptions pto;
        pto.SLPVectorization = slp != 0;
        PassBuilder pb(tm.get(), pto);
        auto tlii = std::make_unique<TargetLibraryInfoImpl>(triple);
        fam.registerPass([&] { return TargetLibraryAnalysis(*tlii); });
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        ModulePassManager mpm;
        if (Error pe = pb.parsePassPipeline(mpm, pipeline)) {
            *err = dup_str("the pass pipeline: " + toString(std::move(pe)));
            return nullptr;
        }
        mpm.run(*m, mam);
    }
    stage_s[1] = since(t);
    if (const char *dump = getenv("HK_DUMP_IR")) {   // (a diagnosis: the module after the pipeline)
        std::error_code ec;
        raw_fd_ostream f(dump, ec);
        if (!ec) m->print(f, nullptr);
    }

    SmallVector<char, 0> obj;
    raw_svector_ostream os(obj);
    legacy::PassManager cg;
    TargetLibraryInfoImpl tlii(triple);
    cg.add(new TargetLibraryInfoWrapperPass(tlii));
    if (tm->addPassesToEmitFile(cg, os, nullptr, CodeGenFileType::ObjectFile, /*DisableVerify*/ true)) {
        *err = dup_str("the target cannot emit an object file");
        return nullptr;
    }
    cg.run(*m);
    stage_s[2] = since(t);
    void *out = malloc(obj.size() ? obj.size() : 1);
    memcpy(out, obj.data(), obj.size());
    *out_n = obj.size();
    return out;
}

extern "C" void hk_free(void *p) { free(p); }
