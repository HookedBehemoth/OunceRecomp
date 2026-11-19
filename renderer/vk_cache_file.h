// A cache file beside the executable: a header, then records {u64 key, u64 size, bytes} appended as they are made, and an
// index beside it (<file>.idx: every record's key, offset and size) so a start reads the index and the heads of the
// records after it, not the head of every record. The program cache, the compute cache and the driver binaries are each
// one (vk_shader.cpp, vk_compute.cpp). A file whose header is not the one asked for (another stamp or driver) is made
// again from its start; a torn last record is cut off. The file is locked for the process's life: another game running
// from the same folder runs without it.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "os.h"

namespace hsd {

class CacheFile {
public:
    struct Rec {
        int64_t at;   // (the record's bytes, after its 16-byte head)
        uint64_t size;
    };
    /// Opens path for this header (any bytes: a magic and a stamp); what says what it holds, in the log line
    void open(const std::string& path, const char* what, const void* header, uint32_t header_n);
    bool on() const { return out_ != nullptr; }
    /// The records kept under key, in file order
    std::vector<Rec> find(uint64_t key) const;
    bool has(uint64_t key) const;
    bool read(const Rec& r, std::vector<uint8_t>& out) const;
    void append(uint64_t key, const void* bytes, size_t n);
    /// The index written for what the file holds now (the precompile's end)
    void index_now();
    size_t records() const;
    int64_t bytes() const;

private:
    struct Entry {
        uint64_t key;
        int64_t at;
        uint64_t size;
    };
    int64_t index_load(os_file* f, int64_t whole);
    void index_save();
    mutable std::mutex mu_;
    std::string path_;
    uint64_t stamp_ = 0;   // (the header's hash: the index's)
    os_file* out_ = nullptr;
    std::vector<Entry> index_;   // (file order)
    std::unordered_map<uint64_t, std::vector<Rec>> by_key_;
    int64_t end_ = 0;
};

}  // namespace hsd
