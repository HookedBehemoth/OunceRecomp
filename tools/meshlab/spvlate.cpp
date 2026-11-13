// spvlate: the back end's late scheduling of a stage's SPIR-V (gpu/direct/direct_spv_late.h) as a command, for the harness:
//   spvlate <in.spv> <out.spv>
// `meshlab compare --repl` with the result in place of the translator's stage checks that it emits the same geometry;
// tools/meshlab/spill_scan.py --repl counts what RADV spills with it. Built by tools/meshlab/build.sh.
#include <cstdio>
#include <vector>

#include "../../gpu/direct/direct_spv_late.cpp"

int main(int argc, char** argv) {
    if (argc < 3) return std::fprintf(stderr, "spvlate <in.spv> <out.spv>\n"), 2;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return std::fprintf(stderr, "no %s\n", argv[1]), 2;
    std::vector<uint32_t> words;
    uint32_t x;
    while (std::fread(&x, 4, 1, f) == 1) words.push_back(x);
    std::fclose(f);
    const uint32_t moved = hsd::spv_schedule_late(words);
    f = std::fopen(argv[2], "wb");
    if (!f) return std::fprintf(stderr, "cannot write %s\n", argv[2]), 2;
    std::fwrite(words.data(), 4, words.size(), f);
    std::fclose(f);
    std::printf("%s: %u instructions moved\n", argv[2], moved);
    return 0;
}
