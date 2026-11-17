// The builder's linker: lld's ELF driver in this process (ld.lld's command line).

#include "lld/Common/Driver.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <cstring>
#include <string>

LLD_HAS_DRIVER(elf)

// argv[0] is the driver's name ("ld.lld"). Returns lld's exit code; *msg (malloc'd) holds what it printed.
extern "C" int hk_lld_link(const char *const *argv, int argc, char **msg) {
    std::string out;
    llvm::raw_string_ostream os(out);
    lld::Result r = lld::lldMain(llvm::ArrayRef<const char *>(argv, argc), os, os, {{lld::Gnu, &lld::elf::link}});
    os.flush();
    *msg = (char *)malloc(out.size() + 1);
    memcpy(*msg, out.data(), out.size() + 1);
    return r.retCode;
}
