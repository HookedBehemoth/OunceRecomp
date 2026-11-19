// The cache files' records and index (vk_cache_file.h).
#include "vk_cache_file.h"

#include <cstring>

#include "vk_internal.h"

namespace hsd {
namespace {

constexpr uint64_t kIndexMagic = 0x3258444950445348ull;   // ("HSDPIDX2")

uint64_t hash_bytes(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

}  // namespace

/// The index's entries when it is the file's (f, its size whole, under this header); the offset it covers, else 0. Its last
/// record's head is checked against the file's
int64_t CacheFile::index_load(os_file* f, int64_t whole) {
    os_file* x = os_file_open((path_ + ".idx").c_str(), OS_READ, nullptr);
    if (!x) return 0;
    uint64_t head[4]{};
    std::vector<Entry> e;
    const int64_t n = os_file_size(x);
    bool ok = n >= 32 && os_file_read_at(x, head, 32, 0) == 32 && head[0] == kIndexMagic && head[1] == stamp_ && (int64_t)head[2] <= whole &&
              (uint64_t)(n - 32) == head[3] * sizeof(Entry);
    if (ok) {
        e.resize(head[3]);
        ok = os_file_read_at(x, e.data(), e.size() * sizeof(Entry), 32) == (int64_t)(e.size() * sizeof(Entry));
    }
    os_file_close(x);
    if (ok && !e.empty()) {
        uint64_t rec[2];
        ok = os_file_read_at(f, rec, 16, (uint64_t)(e.back().at - 16)) == 16 && rec[0] == e.back().key && rec[1] == e.back().size &&
             e.back().at + (int64_t)e.back().size == (int64_t)head[2];
    }
    if (!ok) return 0;
    index_ = std::move(e);
    return (int64_t)head[2];
}

void CacheFile::index_save() {
    const std::string tmp = path_ + ".idx.tmp";
    os_file* f = os_file_open(tmp.c_str(), OS_WRITE | OS_CREATE | OS_TRUNC, nullptr);
    if (!f) return;
    const uint64_t head[4] = {kIndexMagic, stamp_, (uint64_t)end_, index_.size()};
    const size_t n = index_.size() * sizeof(Entry);
    const bool ok = os_file_write_at(f, head, 32, 0) == 32 && (!n || os_file_write_at(f, index_.data(), n, 32) == (int64_t)n);
    os_file_close(f);
    if (ok) os_rename(tmp.c_str(), (path_ + ".idx").c_str(), true);
}

void CacheFile::open(const std::string& path, const char* what, const void* header, uint32_t header_n) {
    std::lock_guard lk(mu_);
    path_ = path, stamp_ = hash_bytes(header, header_n);
    os_file* lock = os_file_open((path + ".lock").c_str(), OS_READ | OS_WRITE | OS_CREATE, nullptr);   // (held for the process's life)
    if (!lock || !os_file_trylock(lock)) {
        os_file_close(lock);
        return logf("direct: %s %s: another game is using it: this one runs without it", what, path.c_str());
    }
    int64_t good = 0, whole = 0;
    bool scanned = false;
    if (os_file* f = os_file_open(path.c_str(), OS_READ, nullptr)) {   // (the index, then the heads after it: no read-ahead)
        os_file_random(f);
        whole = os_file_size(f);
        std::vector<uint8_t> got(header_n);
        if (whole >= header_n && os_file_read_at(f, got.data(), header_n, 0) == header_n && !std::memcmp(got.data(), header, header_n)) {
            good = std::max<int64_t>(header_n, index_load(f, whole));
            while (good + 16 <= whole) {
                uint64_t rec[2];
                if (os_file_read_at(f, rec, 16, (uint64_t)good) != 16 || !rec[1] || rec[1] > (uint64_t)(whole - good - 16)) break;
                index_.push_back({rec[0], good + 16, rec[1]});
                good += 16 + (int64_t)rec[1];
                scanned = true;
            }
        }
        os_file_close(f);
    }
    if (good) {
        out_ = os_file_open(path.c_str(), OS_READ | OS_WRITE, nullptr);
        if (out_ && good != whole && os_file_resize(out_, (uint64_t)good) != 0) os_file_close(out_), out_ = nullptr;
    }
    if (!out_) {   // (none, or another header's: made again)
        index_.clear(), good = header_n;
        out_ = os_file_open(path.c_str(), OS_READ | OS_WRITE | OS_CREATE | OS_TRUNC, nullptr);
        if (out_ && os_file_write_at(out_, header, header_n, 0) != header_n) os_file_close(out_), out_ = nullptr;
        scanned = true;
    }
    end_ = good;
    for (const Entry& e : index_) by_key_[e.key].push_back({e.at, e.size});
    if (out_ && scanned) index_save();
    logf("direct: %s %s: %zu records kept (%lld KB)%s", what, path.c_str(), index_.size(), (long long)(end_ >> 10),
         whole && end_ == header_n && whole > header_n ? "; another stamp's file was made again" : "");
}

std::vector<CacheFile::Rec> CacheFile::find(uint64_t key) const {
    std::lock_guard lk(mu_);
    const auto it = by_key_.find(key);
    return it == by_key_.end() ? std::vector<Rec>{} : it->second;
}

bool CacheFile::has(uint64_t key) const {
    std::lock_guard lk(mu_);
    return by_key_.count(key) != 0;
}

bool CacheFile::read(const Rec& r, std::vector<uint8_t>& out) const {
    out.resize(r.size);
    return out_ && os_file_read_at(out_, out.data(), out.size(), (uint64_t)r.at) == (int64_t)out.size();
}

void CacheFile::append(uint64_t key, const void* bytes, size_t n) {
    if (!out_ || !n) return;
    const uint64_t head[2] = {key, n};
    std::lock_guard lk(mu_);
    if (os_file_write_at(out_, head, 16, (uint64_t)end_) != 16 || os_file_write_at(out_, bytes, n, (uint64_t)end_ + 16) != (int64_t)n) return;
    index_.push_back({key, end_ + 16, n});
    by_key_[key].push_back({end_ + 16, n});
    end_ += 16 + (int64_t)n;
}

void CacheFile::index_now() {
    std::lock_guard lk(mu_);
    if (out_) index_save();
}

size_t CacheFile::records() const {
    std::lock_guard lk(mu_);
    return index_.size();
}

int64_t CacheFile::bytes() const {
    std::lock_guard lk(mu_);
    return end_;
}

}  // namespace hsd
