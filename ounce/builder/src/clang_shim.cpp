// A C file -> an x86-64 object in this process, through clang's libraries: the driver's command line (as
// nx2/build.py gives it to clang) made a compiler invocation, then EmitObjAction with the object kept in memory.
// Built only with the `clang` feature, against a static LLVM with clang (LLVM_STATIC_DIR).

#include "clang/Basic/DiagnosticOptions.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace clang;

static char *dup_str(const std::string &s) {
    char *p = (char *)malloc(s.size() + 1);
    memcpy(p, s.data(), s.size() + 1);
    return p;
}

// argv: clang's driver arguments (argv[0] a clang path, then the flags, -c <file>; -o is ignored); the object
// (malloc'd, *out_n bytes), or NULL and *err. *diag (malloc'd) holds the diagnostics either way.
extern "C" void *hk_clang_compile(const char *const *argv, int argc, size_t *out_n, char **err) {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86AsmParser();   // (inline assembly)
    std::string text;
    llvm::raw_string_ostream os(text);
    auto vfs = llvm::vfs::getRealFileSystem();
    DiagnosticOptions dopts;
    auto *printer = new TextDiagnosticPrinter(os, dopts);
    IntrusiveRefCntPtr<DiagnosticsEngine> diags = CompilerInstance::createDiagnostics(*vfs, dopts, printer, true);
    CreateInvocationOptions co;
    co.Diags = diags;
    co.VFS = vfs;
    std::shared_ptr<CompilerInvocation> inv = createInvocation(llvm::ArrayRef<const char *>(argv, argc), co);
    if (!inv) {
        *err = dup_str("the driver made no compiler invocation:\n" + text);
        return nullptr;
    }
    inv->getFrontendOpts().OutputFile = "-";
    CompilerInstance ci(inv);
    ci.createDiagnostics(new TextDiagnosticPrinter(os, ci.getDiagnosticOpts()), true);
    ci.createVirtualFileSystem(vfs);
    llvm::SmallVector<char, 0> obj;
    ci.setOutputStream(std::make_unique<llvm::raw_svector_ostream>(obj));
    EmitObjAction act;
    if (!ci.ExecuteAction(act)) {
        *err = dup_str("the compile failed:\n" + text);
        return nullptr;
    }
    void *out = malloc(obj.size() ? obj.size() : 1);
    memcpy(out, obj.data(), obj.size());
    *out_n = obj.size();
    return out;
}
