#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace starlet::gx {

// Program words (see direct_program_words) of programs a device has needed: a
// "# starlet program list 1" line, then one line of hex words per program. Lists
// travel between devices and versions, so programs can be built before first use.
std::vector<std::vector<std::uint32_t>> read_program_list(const std::string& path);

// Appends words not yet in the list at path. Owner thread only.
class ProgramList {
public:
    explicit ProgramList(std::string path);
    ~ProgramList();
    ProgramList(const ProgramList&) = delete;
    ProgramList& operator=(const ProgramList&) = delete;
    void record(const std::vector<std::uint32_t>& words);

private:
    std::string path_;
    std::FILE* file_ = nullptr;
    bool failed_ = false;
    std::unordered_set<std::uint64_t> known_;
};

} // namespace starlet::gx
