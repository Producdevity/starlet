#include "starlet/gx/gles/program_list.hpp"

#include <cstdlib>
#include <cstring>

namespace starlet::gx {
namespace {
constexpr char header[] = "# starlet program list 1\n";
// Lists recorded before the format took Starlet's name; identical otherwise.
constexpr char earlier_header[] = "# galaxy program list 1\n";

std::uint64_t key(const std::vector<std::uint32_t>& words) {
    std::uint64_t h = 14695981039346656037ull;
    for (auto w : words) { h ^= w; h *= 1099511628211ull; h ^= h >> 29; }
    return h;
}
} // namespace

std::vector<std::vector<std::uint32_t>> read_program_list(const std::string& path) {
    std::vector<std::vector<std::uint32_t>> lists;
    auto* file = std::fopen(path.c_str(), "r");
    if (!file) return lists;
    std::string line;
    char buffer[1024];
    bool known_format = false;
    while (std::fgets(buffer, sizeof buffer, file)) {
        line += buffer;
        if (line.empty() || line.back() != '\n') continue;
        if (line == header || line == earlier_header) known_format = true;
        else if (known_format && line[0] != '#') {
            std::vector<std::uint32_t> words;
            const char* at = line.c_str();
            char* end = nullptr;
            for (unsigned long v; (v = std::strtoul(at, &end, 16)), end != at; at = end) words.push_back(static_cast<std::uint32_t>(v));
            if (!words.empty()) lists.push_back(std::move(words));
        }
        line.clear();
    }
    std::fclose(file);
    return lists;
}

ProgramList::ProgramList(std::string path) : path_(std::move(path)) {
    for (const auto& words : read_program_list(path_)) known_.insert(key(words));
}

ProgramList::~ProgramList() {
    if (file_) std::fclose(file_);
}

void ProgramList::record(const std::vector<std::uint32_t>& words) {
    if (failed_ || !known_.insert(key(words)).second) return;
    if (!file_) {
        file_ = std::fopen(path_.c_str(), "a+");
        if (!file_) { failed_ = true; return; }
        std::fseek(file_, 0, SEEK_END);
        if (std::ftell(file_) == 0) std::fputs(header, file_);
        else {
            // A line cut short by an earlier exit stays unparsable instead of joining this one.
            std::fseek(file_, -1, SEEK_END);
            const bool complete = std::fgetc(file_) == '\n';
            std::fseek(file_, 0, SEEK_END);
            if (!complete) std::fputc('\n', file_);
        }
    }
    for (std::size_t i = 0; i < words.size(); ++i) std::fprintf(file_, i ? " %x" : "%x", words[i]);
    std::fputc('\n', file_);
    std::fflush(file_);
}

} // namespace starlet::gx
