// (tests/test_eng_passes.py) the pass table (eng_pass.cpp) from two threads: a note and a read, rows by view, formats compared on
// the colour targets in use; the secondary trap keeps the first reason
#include "eng_pass.h"
#include <cstdio>
#include <cstdarg>
#include <thread>
namespace mkwvk { void logf(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); } }
using namespace mkwvk;
int main() {
    EngPassFormats f{}; f.ncolor = 1; f.color[0] = VK_FORMAT_B10G11R11_UFLOAT_PACK32; f.depth = VK_FORMAT_D32_SFLOAT; f.samples = VK_SAMPLE_COUNT_1_BIT;
    EngPassFormats o{};
    if (eng_pass_formats(0x1000, &o)) return 1;
    eng_pass_note_ex(0x1000, f, 0x80668c84, 1536, 864, 0xaa, 0xbb);
    if (!eng_pass_formats(0x1000, &o) || !eng_formats_equal(o, f)) return 2;
    EngPassFormats g = f; g.color[1] = VK_FORMAT_R8_UNORM;   // (beyond ncolor: ignored)
    if (!eng_formats_equal(f, g)) return 3;
    g.ncolor = 2; if (eng_formats_equal(f, g)) return 4;
    eng_pass_note_ex(0x1000, f, 0x80668c84, 1536, 864, 0xaa, 0xcc);   // (a new depth object: same formats)
    std::thread t([&] { for (int i = 0; i < 100000; i++) { EngPassFormats x; eng_pass_formats(0x1000, &x); } });
    for (int i = 0; i < 100000; i++) eng_pass_note_ex(0x2000 + (i & 15), f, 0, 1, 1, i, i);
    t.join();
    if (eng_sec_trap("x")) return 5;
    eng_sec_set(true);
    if (!eng_sec_trap("first") || !eng_sec_trap("second")) return 6;
    if (std::string(eng_sec_broken()) != "first") return 7;
    eng_sec_set(false);
    if (eng_sec_broken()) return 8;
    printf("PASS\n");
}
