"""The write-back page table (gpu/vk/vk_memory.cpp WbPages): values per page in chunks made on first use, and max_in, which
reads a range of bytes skipping whole chunks never made. Built with small chunks so their edges are easy to reach. Two
values a page with the bytes each covers: a read of the bytes one write-back left alone does not see its value (OoT's
double-buffered slots sharing a page), a third value merges the older two.

python -I -B tests/test_wb_pages.py [--cxx clang++]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source', type=Path, default=ROOT / 'gpu/vk/vk_memory.cpp')
    args = parser.parse_args()
    table = block(args.source.read_text(encoding='utf-8'), 'struct WbPages {')
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
constexpr uint64_t kChunkPages = 16, kPages = 64;   // (four chunks of 16 pages: vk_memory.cpp has 2048 of 65536)
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)
''' + table + r''';
uint64_t range(const WbPages& t, uint64_t first, uint64_t last, bool want_ever) {
    bool ever = false;
    const uint64_t v = t.max_in(first * 4096, (last + 1) * 4096, &ever);
    CHECK(ever == want_ever);
    CHECK(t.max_in(first * 4096, (last + 1) * 4096, nullptr) == v);
    return v;
}
uint64_t bytes(const WbPages& t, uint64_t lo, uint64_t hi) { return t.max_in(lo, hi, nullptr); }
int main() {
    WbPages t;
    CHECK(t.empty() && t.get(5) == 0 && range(t, 0, 63, false) == 0);
    t.set(3, 7);   // (chunk 0)
    CHECK(!t.empty() && t.get(3) == 7 && t.get(4) == 0);
    CHECK(range(t, 0, 63, true) == 7 && range(t, 4, 63, false) == 0 && range(t, 3, 3, true) == 7);
    t.set(40, 9);   // (chunk 2; chunk 1 never made)
    CHECK(range(t, 3, 40, true) == 9 && range(t, 4, 39, false) == 0 && range(t, 16, 47, true) == 9);
    t.set(15, 11), t.set(16, 2);   // (the last page of chunk 0, the first of chunk 1)
    CHECK(range(t, 15, 15, true) == 11 && range(t, 16, 16, true) == 2 && range(t, 14, 17, true) == 11);
    CHECK(range(t, 16, 39, true) == 2);
    t.set(15, 1);   // (a later write-back's value replaces the page's, lower or not)
    CHECK(t.get(15) == 1 && range(t, 15, 15, true) == 1 && range(t, 0, 63, true) == 9);
    t.set(64, 5);   // (past the table: left out)
    CHECK(t.get(64) == 0 && t.get(1000) == 0);
    t.set(63, 4);   // (the table's last page; a range reaching past the end stops there)
    CHECK(range(t, 63, 1000, true) == 4 && range(t, 41, 62, false) == 0 && range(t, 41, 1000, true) == 4);
    {   // (bytes within a page: two slots back to back, the second starting 0x40 into the first one's last page)
        WbPages s;
        s.set(2, 10, 0, 0x40);      // (slot 0's tail, written back by 10)
        s.set(2, 20, 0x40, 4096);   // (slot 1's head, by the newer 20)
        CHECK(bytes(s, 0x1000, 0x2040) == 10 && bytes(s, 0x2040, 0x3000) == 20 && bytes(s, 0x1000, 0x3000) == 20);
        CHECK(s.get(2) == 20 && bytes(s, 0x2000, 0x2001) == 10 && bytes(s, 0x203f, 0x2041) == 20);
        s.set(2, 30, 0x40, 0x80);   // (a third value: the older two merged, the larger over both)
        CHECK(bytes(s, 0x2040, 0x2080) == 30 && bytes(s, 0x2000, 0x2040) == 20 && bytes(s, 0x2080, 0x3000) == 20);
        s.set(2, 30, 0x80, 0x100);  // (the same value again: its bytes grow)
        CHECK(bytes(s, 0x2080, 0x2100) == 30 && bytes(s, 0x2100, 0x3000) == 20);
        s.set(2, 5, 0, 4096);       // (a later write-back over every byte: replaced, lower or not)
        CHECK(bytes(s, 0x2000, 0x3000) == 5 && s.get(2) == 5);
        bool ever = false;
        CHECK(s.max_in(0x3000, 0x4000, &ever) == 0 && !ever && s.max_in(0x2fff, 0x3000, &ever) == 5 && ever);
    }
    std::puts("PASS: values per page and per byte range, ranges over chunks never made, chunk edges and the table's end");
}
'''
    with tempfile.TemporaryDirectory(prefix='wb-pages-') as tmp:
        cpp = Path(tmp) / 'fixture.cpp'
        exe = Path(tmp) / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=60)   # (a range read that stops moving hangs: a failure too)


if __name__ == '__main__':
    main()
