#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace starlet::gx {

struct GlesProgramCacheStats {
    std::uint64_t hits = 0, misses = 0, rejected = 0, stored = 0, store_failures = 0;
};

// Linked program binaries kept across runs in one directory, keyed by the GL
// driver's identity and the exact shader sources. Usable from any thread whose
// current context shares objects with the constructing one.
class GlesProgramCache {
public:
    // An empty directory, a missing parent directory or a driver without program
    // binary formats disables the cache: every load misses and nothing is stored.
    // Above max_entries, the least recently written entries are removed.
    explicit GlesProgramCache(std::string directory);
    static constexpr std::size_t max_entries = 4096;
    GlesProgramCache(const GlesProgramCache&) = delete;
    GlesProgramCache& operator=(const GlesProgramCache&) = delete;
    bool enabled() const { return enabled_; }
    // A linked program for these sources, or 0. Entries the driver no longer
    // accepts are deleted.
    unsigned load(const std::string& vertex, const std::string& fragment);
    // Before glLinkProgram, so the driver keeps the binary retrievable.
    void prepare(unsigned program) const;
    void store(unsigned program, const std::string& vertex, const std::string& fragment);
    // Whether an entry for these sources exists; it may still be rejected on load.
    bool contains(const std::string& vertex, const std::string& fragment) const;
    GlesProgramCacheStats stats() const;

private:
    std::string path(const std::string& vertex, const std::string& fragment) const;
    std::string directory_, identity_;
    bool enabled_ = false;
    std::atomic<std::uint64_t> hits_{0}, misses_{0}, rejected_{0}, stored_{0}, store_failures_{0};
};

// A linked program for these sources: the cached binary, or compiled, linked and
// stored. Throws on compile or link errors.
unsigned link_program(const std::string& vertex, const std::string& fragment, GlesProgramCache&);

} // namespace starlet::gx
