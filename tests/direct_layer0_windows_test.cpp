// (M6b) The direct back end's copy of the window scheme (gpu/direct/layer0_windows.h), as tests/layer0_windows_test.cpp checks the
// original, with what the copy changes: its windows are a 64 KB granule short of 4 GB (RADV's maxBufferSize is 4 GB - 4), so the
// longest range whole in the window of its start is a granule short of 2 GB, the last granule of each 2 GB stretch is in its own
// window only, and a range's pieces never reach past a window's end. tests/test_direct_layer0_windows.py builds and runs it.
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
    CHECK(fits_window(kStride - 1, kMaxRange));          // (from the last byte of a window's first half, 2 GB less a granule on: inside)
    CHECK(!fits_window(kStride - 1, kMaxRange + 1));
    CHECK(!fits_window(0x1000000000ull, kMaxRange + 1));
    // (the copy's limits: a window fits RADV's maxBufferSize of 4 GB - 4, and the longest whole range is 2 GB less a granule)
    CHECK(kWindow <= (4ull << 30) - 4 && kWindow == (4ull << 30) - kGran && kMaxRange == kStride - kGran);
    CHECK(fits_window(0x1000000000ull, kMaxRange) && !fits_window(0x1000000000ull + kStride - kGran, kGran + kMaxRange));
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
                // (the copy: the window k - 1 holds the chunk up to its end only: the top chunk of a stretch ends past it, so its
                // binds go through range_places, which cuts them at the window's end; chunk_places names the windows to make)
                CHECK(pl[1].offset >= kStride && pl[1].offset < kWindow);
                CHECK(pl[1].offset + chunk <= kWindow || window_offset(g) + chunk == kStride);
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

    // (7b) word-wise bit ranges against bit by bit
    {
        ChunkBits b(1ull << 30, kGran);   // (16384 granules)
        std::mt19937_64 r2(9);
        for (int i = 0; i < 3000; i++) b.set(r2() % b.chunks);
        for (uint64_t c = 1000; c < 1300; c++) b.set(c);
        for (int i = 0; i < 20000; i++) {
            uint64_t c0 = r2() % b.chunks, c1 = c0 + r2() % 400;
            if (i % 3 == 0) c0 = 1000 + r2() % 200, c1 = c0 + r2() % 120;
            if (c1 > b.chunks) c1 = b.chunks;
            bool want = c0 < c1;
            for (uint64_t c = c0; c < c1; c++) want = want && b.has(c);
            CHECK(b.all_bits(c0, c1) == want);
        }
        b.clear(1100);
        CHECK(!b.all(1000 * kGran, 1300 * kGran) && b.all(1101 * kGran, 1300 * kGran));
    }
    // interior granules: the partial edges stay out
    {
        uint64_t g0, g1;
        interior_granules(0x1000007000ull, 0x1000007000ull + 0x300000, g0, g1);
        CHECK(g0 * kGran == 0x1000010000ull && g1 * kGran == 0x1000300000ull);
        interior_granules(0x1000000000ull, 0x1000020000ull, g0, g1);
        CHECK(g0 * kGran == 0x1000000000ull && g1 * kGran == 0x1000020000ull);
        interior_granules(0x1000001000ull, 0x100000f000ull, g0, g1);
        CHECK(g0 == g1);   // (inside one granule: none)
    }
    // a range's pieces: each in one 2 GB stretch, in window k and 2 GB further in k - 1, memory offsets in order
    {
        std::vector<Piece> ps;
        range_places(0x107ff00000ull, 0x1080100000ull, ps);   // (across the line at 0x1080000000)
        CHECK(ps.size() == 4);
        CHECK(ps[0].window == 32 && ps[0].offset == 0x7ff00000ull && ps[0].from == 0 && ps[0].size == 0x100000);
        CHECK(ps[1].window == 31 && ps[1].offset == 0x7ff00000ull + kStride && ps[1].from == 0);
        CHECK(ps[2].window == 33 && ps[2].offset == 0 && ps[2].from == 0x100000 && ps[2].size == 0x100000);
        CHECK(ps[3].window == 32 && ps[3].offset == kStride && ps[3].from == 0x100000);
        for (const Piece& p : ps) CHECK(p.offset + p.size <= kWindow);
        for (const Piece& p : ps) CHECK(window_base(p.window) + p.offset == 0x107ff00000ull + p.from);
        ps.clear();
        // (the copy: the last granule of a stretch is in its own window only; a piece in k - 1 is cut at the window's end)
        range_places(0x1080000000ull - kGran, 0x1080000000ull, ps);
        CHECK(ps.size() == 1 && ps[0].window == 32 && ps[0].offset == kStride - kGran && ps[0].size == kGran);
        ps.clear();
        range_places(0x1080000000ull - 2 * kGran, 0x1080000000ull, ps);
        CHECK(ps.size() == 2 && ps[1].window == 31 && ps[1].offset == 2 * kStride - 2 * kGran && ps[1].size == kGran);
        ps.clear();
        // random ranges: every piece inside its window, each byte of the range covered in the window of its own start
        std::mt19937_64 r3(11);
        for (int i = 0; i < 20000; i++) {
            const uint64_t lo = r3() % (400ull << 30), hi = lo + 1 + r3() % (5ull << 30);
            std::vector<Piece> v;
            range_places(lo, hi, v);
            uint64_t own = 0;
            for (const Piece& p : v) {
                CHECK(p.size > 0 && p.offset + p.size <= kWindow);
                CHECK(window_base(p.window) + p.offset == lo + p.from);
                if (p.window == window_of(lo + p.from)) own += p.size;
            }
            CHECK(own == hi - lo);
        }
        ps.clear();
        range_places(0x1000, 0x2000, ps);   // (window 0 has no lower neighbour)
        CHECK(ps.size() == 1 && ps[0].window == 0);
    }
    // slots: first fit, contiguous, freed neighbours merged, a full give-back is one range again
    {
        SlotRanges s(256ull << 20);
        uint64_t a, b, c, d;
        CHECK(s.take(64ull << 20, a) && a == 0);
        CHECK(s.take(32ull << 20, b) && b == (64ull << 20));
        CHECK(s.take(160ull << 20, c) && c == (96ull << 20));
        CHECK(!s.take(kGran, d));
        s.give(b, 32ull << 20);
        CHECK(s.take(16ull << 20, d) && d == b);   // (first fit in the hole)
        s.give(a, 64ull << 20);
        s.give(d, 16ull << 20);
        CHECK(s.free.size() == 1 && s.free.begin()->first == 0 && s.free.begin()->second == (96ull << 20));
        s.give(c, 160ull << 20);
        CHECK(s.free.size() == 1 && s.free_bytes() == (256ull << 20));
        SlotRanges e;
        CHECK(!e.take(kGran, d) && e.free_bytes() == 0);
    }
    // a pool's granules without those other pools cover
    {
        std::vector<std::pair<uint64_t, uint64_t>> out;
        granules_minus(10, 20, {{0, 11}, {14, 15}, {19, 30}}, out);
        CHECK(out.size() == 2 && out[0].first == 11 && out[0].second == 14 && out[1].first == 15 && out[1].second == 19);
        out.clear();
        granules_minus(10, 20, {}, out);
        CHECK(out.size() == 1 && out[0].first == 10 && out[0].second == 20);
        out.clear();
        granules_minus(10, 20, {{5, 25}}, out);
        CHECK(out.empty());
        out.clear();
        granules_minus(10, 20, {{12, 13}, {12, 16}}, out);   // (overlapping cuts, sorted by start)
        CHECK(out.size() == 2 && out[0].second == 12 && out[1].first == 16);
    }
    std::printf("PASS: direct layer 0 windows (a granule short of 4 GB: bindings whole in a window, aliased chunk places, chunk sizes, bits, first-use spans, granule ranges, pieces cut at the window's end, slots)\n");
    return 0;
}
