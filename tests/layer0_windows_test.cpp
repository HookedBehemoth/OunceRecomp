// (native-vk, layer 0) The window scheme's arithmetic (gpu/vk/layer0_windows.h): every binding up to 2 GB whole in the window of
// its start, a chunk's two places in the aliased windows, the chunk size from the wish and udmabuf's limit, the bound-chunk bits,
// and what a first use binds. tests/test_layer0_windows.py builds and runs it.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "layer0_windows.h"

using namespace mkwvk::l0;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main() {
    // window arithmetic
    CHECK(window_of(0) == 0 && window_of(kStride - 1) == 0 && window_of(kStride) == 1);
    CHECK(window_of(0x1000000000ull) == 32);
    CHECK(window_offset(0x1080000000ull) == 0 && window_offset(0x107fffffffull) == kStride - 1);
    CHECK(window_base(33) + window_offset(0x1080001000ull) == 0x1080001000ull);
    // every range up to 2 GB fits the window of its start, wherever it starts; longer ones do not
    std::mt19937_64 rng(5);
    for (int i = 0; i < 100000; i++) {
        const uint64_t g = rng() % (512ull << 30), n = 1 + rng() % kMaxRange;
        CHECK(fits_window(g, n));
        const uint64_t off = window_offset(g);
        CHECK(off + n <= kWindow);
        CHECK(window_base(window_of(g)) + off == g);
    }
    CHECK(fits_window(kStride - 1, kMaxRange));          // (from the last byte of a window's first half, 2 GB on: still inside)
    CHECK(!fits_window(0x1000000000ull, kMaxRange + 1));
    CHECK(!fits_window((uint64_t)kWindows * kStride, 16));   // (past the windows)

    // chunk size: a power of two in [2 MB, min(64 MB, the limit)]
    CHECK(chunk_bytes(64, 64ull << 20) == (64ull << 20));
    CHECK(chunk_bytes(16, 64ull << 20) == (16ull << 20));
    CHECK(chunk_bytes(48, 64ull << 20) == (32ull << 20));
    CHECK(chunk_bytes(64, 32ull << 20) == (32ull << 20));    // (udmabuf's limit lowered)
    CHECK(chunk_bytes(1, 64ull << 20) == kChunkMin);
    CHECK(chunk_bytes(4096, 1ull << 30) == kChunkMax);
    CHECK(chunk_bytes(64, 1ull << 20) == kChunkMin);         // (a limit below the minimum: the minimum, the import will say)

    for (uint64_t chunk : {kChunkMin, (uint64_t)(16ull << 20), kChunkMax}) {
        // a chunk never crosses a 2 GB line, and its two places are the same guest bytes in windows k and k - 1
        for (int i = 0; i < 20000; i++) {
            const uint64_t c = rng() % ((512ull << 30) / chunk);
            Place pl[2];
            const int n = chunk_places(c, chunk, pl);
            const uint64_t g = c * chunk;
            CHECK(window_of(g) == window_of(g + chunk - 1));
            CHECK(n == (window_of(g) ? 2 : 1));
            CHECK(pl[0].window == window_of(g) && window_base(pl[0].window) + pl[0].offset == g);
            CHECK(pl[0].offset + chunk <= kWindow && pl[0].offset % (64u << 10) == 0);
            if (n == 2) {
                CHECK(pl[1].window + 1 == pl[0].window && window_base(pl[1].window) + pl[1].offset == g);
                CHECK(pl[1].offset >= kStride && pl[1].offset + chunk <= kWindow);
            }
        }
        // a range whole in window k: every chunk of it is bound in window k (one of its two places)
        for (int i = 0; i < 20000; i++) {
            const uint64_t g = rng() % (500ull << 30), n = 1 + rng() % (64ull << 20);
            const uint32_t k = window_of(g);
            uint64_t c0, c1;
            chunk_span(g, g + n, chunk, c0, c1);
            CHECK(c0 * chunk <= g && c1 * chunk >= g + n && c1 > c0);
            for (uint64_t c = c0; c < c1; c++) {
                Place pl[2];
                const int m = chunk_places(c, chunk, pl);
                bool in_k = false;
                for (int j = 0; j < m; j++) in_k |= pl[j].window == k;
                CHECK(in_k);
            }
        }
    }
    uint64_t c0, c1;
    chunk_span(100, 100, 16ull << 20, c0, c1);
    CHECK(c0 == c1);   // (an empty range: no chunk)

    // the bits: set after binding, read by any thread
    {
        const uint64_t chunk = 16ull << 20;
        ChunkBits b(512ull << 30, chunk);
        CHECK(b.chunks == (512ull << 30) / chunk);
        CHECK(!b.all(0x1000000000ull, 0x1000000100ull) && !b.any(0x1000000000ull, 0x1000000100ull));
        b.set(0x1000000000ull / chunk);
        CHECK(b.all(0x1000000000ull, 0x1000000100ull));
        CHECK(!b.all(0x1000000000ull, 0x1000000000ull + chunk + 1));   // (the next chunk is not bound)
        CHECK(b.any(0x1000000000ull - 4096, 0x1000000000ull + 4096));
        CHECK(!b.any(0x1000000000ull - 4096, 0x1000000000ull));
        CHECK(!b.all(5, 5));                                             // (empty)
        CHECK(!b.all((512ull << 30) - 16, (512ull << 30) + 16));         // (past the window)
        b.set(b.chunks + 7);                                             // (ignored)
        CHECK(!b.has(b.chunks + 7));
        // threads setting different chunks of one word lose none
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; t++)
            ts.emplace_back([&b, t] {
                for (uint64_t c = (uint64_t)t; c < 4096; c += 8) b.set(c);
            });
        for (auto& t : ts) t.join();
        for (uint64_t c = 0; c < 4096; c++) CHECK(b.has(c));
    }

    // a first use: the whole pool when it is small, else the range; never past the guest window
    {
        uint64_t lo, hi;
        bind_span(0x1000010000ull, 0x100, 0x1000000000ull, 0x1000400000ull, 64ull << 20, 512ull << 30, lo, hi);
        CHECK(lo == 0x1000000000ull && hi == 0x1000400000ull);
        bind_span(0x1000010000ull, 0x100, 0x1000000000ull, 0x1000000000ull + (256ull << 20), 64ull << 20, 512ull << 30, lo, hi);
        CHECK(lo == 0x1000010000ull && hi == 0x1000010100ull);
        bind_span(0x1000010000ull, 0x100, 0, 0, 64ull << 20, 512ull << 30, lo, hi);   // (no pool given)
        CHECK(lo == 0x1000010000ull && hi == 0x1000010100ull);
        bind_span((512ull << 30) - 0x100, 0x200, 0, 0, 64ull << 20, 512ull << 30, lo, hi);
        CHECK(lo == (512ull << 30) - 0x100 && hi == (512ull << 30));
        bind_span(0x1000010000ull, 0x100, 0x1000000000ull, 0x1000400000ull, 0, 512ull << 30, lo, hi);   // (span 0: the range only)
        CHECK(lo == 0x1000010000ull && hi == 0x1000010100ull);
    }
    std::printf("PASS: layer 0 windows (bindings whole in a window, aliased chunk places, chunk sizes, bits, first-use spans)\n");
    return 0;
}
