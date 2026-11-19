// The builder's linker: lld in this process, its ELF driver (ld.lld's command line) or, on Windows, its COFF driver
// (lld-link's): argv[0] names which.

#include "lld/Common/Driver.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <cstring>
#include <string>

LLD_HAS_DRIVER(elf)
#ifdef _WIN32
LLD_HAS_DRIVER(coff)
#endif

// argv[0] is the driver's name ("ld.lld" or "lld-link"). Returns lld's exit code; *msg (malloc'd) holds what it printed.
extern "C" int hk_lld_link(const char *const *argv, int argc, char **msg) {
    std::string out;
    llvm::raw_string_ostream os(out);
#ifdef _WIN32
    lld::Result r = lld::lldMain(llvm::ArrayRef<const char *>(argv, argc), os, os, {{lld::Gnu, &lld::elf::link}, {lld::WinLink, &lld::coff::link}});
#else
    lld::Result r = lld::lldMain(llvm::ArrayRef<const char *>(argv, argc), os, os, {{lld::Gnu, &lld::elf::link}});
#endif
    os.flush();
    *msg = (char *)malloc(out.size() + 1);
    memcpy(*msg, out.data(), out.size() + 1);
    return r.retCode;
}
