// spvlate: the back end's late scheduling of a stage's SPIR-V (renderer/direct_spv_late.h) as a command, for the gate
// (tools/shaders/late_gate.py) and tests/test_direct_spv_late.py:
//   spvlate <in.spv> <out.spv> [--budget N] [--sink-peak N] [--reps N] [--blocks]   (--blocks: a line a block looked at)
//   spvlate --dir <in dir> <out dir> [--budget N]      every *.spv of a folder
// One line a module: instructions moved, blocks looked at and changed, the most 32-bit values a block held before and after
// (the pass's own count), and the pass's time. Built by tools/shaders/build.sh.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../renderer/direct_spv_late.cpp"

static bool read_words(const std::string& path, std::vector<uint32_t>& words) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    words.clear();
    uint32_t x;
    while (std::fread(&x, 4, 1, f) == 1) words.push_back(x);
    std::fclose(f);
    return true;
}

static int one(const std::string& in, const std::string& out, const hsd::LateOptions& opt, int reps) {
    std::vector<uint32_t> src;
    if (!read_words(in, src)) return std::fprintf(stderr, "no %s\n", in.c_str()), 2;
    std::vector<uint32_t> words;
    hsd::LateResult r;
    double best = 1e9;
    for (int k = 0; k < reps; k++) {
        words = src;
        const auto t0 = std::chrono::steady_clock::now();
        hsd::spv_schedule_late(words, &r, opt);
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) return std::fprintf(stderr, "cannot write %s\n", out.c_str()), 2;
    std::fwrite(words.data(), 4, words.size(), f);
    std::fclose(f);
    std::printf("%s: %u instructions moved (%u into another block), %u of %u blocks changed, values held at most %u -> %u, %zu words, %.3f ms\n", out.c_str(), r.moved,
                r.sunk, r.blocks_changed, r.blocks, r.peak_before, r.peak_after, words.size(), best);
    return 0;
}

int main(int argc, char** argv) {
    hsd::LateOptions opt;
    std::vector<std::string> pos;
    bool dir = false;
    int reps = 1;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--budget") && i + 1 < argc) opt.budget = (uint32_t)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sink-peak") && i + 1 < argc) opt.sink_peak = (uint32_t)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--dir")) dir = true;
        else if (!std::strcmp(argv[i], "--blocks")) opt.log = [](const char* line) { std::printf("  %s\n", line); };
        else pos.push_back(argv[i]);
    }
    if (pos.size() < 2) return std::fprintf(stderr, "spvlate <in.spv> <out.spv> [--budget N] [--reps N] | --dir <in dir> <out dir>\n"), 2;
    if (!dir) return one(pos[0], pos[1], opt, reps);
    std::filesystem::create_directories(pos[1]);
    std::vector<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator(pos[0]))
        if (e.path().extension() == ".spv") names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    int rc = 0;
    for (const std::string& n : names) rc |= one(pos[0] + "/" + n, pos[1] + "/" + n, opt, reps);
    return rc;
}
