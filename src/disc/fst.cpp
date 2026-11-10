#include "starlet/disc/fst.hpp"
#include "starlet/disc/image.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

namespace starlet::disc {
namespace {
constexpr std::size_t max_bytes = 8u << 20, max_entries = 65536, max_depth = 64, max_name = 255, max_path = 4096;

std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}
}

std::string host_name(std::string_view fst_name) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (std::size_t i = 0; i < fst_name.size(); ++i) {
        const auto c = static_cast<unsigned char>(fst_name[i]);
        const bool last = i + 1 == fst_name.size();
        if (c < 0x20 || c > 0x7e || std::strchr("\"*:<>?\\|%", c) || (last && (c == '.' || c == ' '))) {
            result += '%';
            result += digits[c >> 4];
            result += digits[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    return result;
}

FileTable::FileTable(const std::uint8_t* fst, std::size_t size, unsigned shift) {
    if (size < 12 || size > max_bytes || be32(fst) != 0x01000000 || be32(fst + 4) != 0)
        throw DiscError("damaged game file table");
    const auto count = be32(fst + 8);
    if (!count || count > max_entries || std::size_t{count} * 12 > size) throw DiscError("damaged game file table");
    const std::size_t strings = std::size_t{count} * 12;
    entries_.reserve(count);
    entries_.push_back({"", 0, count, 0, 0, true});
    std::vector<std::uint32_t> parents{0};
    std::set<std::pair<std::uint32_t, std::string>> names;
    for (std::uint32_t i = 1; i < count; ++i) {
        while (!parents.empty() && entries_[parents.back()].next == i) parents.pop_back();
        if (parents.empty()) throw DiscError("damaged game file table");
        const auto* record = fst + std::size_t{i} * 12;
        const auto word = be32(record);
        if (word >> 24 > 1) throw DiscError("damaged game file table");
        FstEntry entry;
        entry.directory = word >> 24 == 1;
        entry.parent = parents.back();
        auto name = strings + (word & 0x00ffffff);
        for (; name < size && fst[name] && entry.name.size() <= max_name; ++name) entry.name += static_cast<char>(fst[name]);
        if (name >= size || fst[name] || entry.name.empty() || entry.name == "." || entry.name == ".." ||
            entry.name.find('/') != std::string::npos)
            throw DiscError("unsafe name in the game file table");
        auto key = host_name(entry.name);
        std::transform(key.begin(), key.end(), key.begin(), lower);
        if (!names.emplace(entry.parent, key).second)
            throw DiscError("game file table repeats the name " + host_name(entry.name));
        if (entry.directory) {
            entry.next = be32(record + 8);
            if (be32(record + 4) != entry.parent || entry.next <= i || entry.next > entries_[entry.parent].next)
                throw DiscError("damaged game file table");
        } else {
            entry.next = i + 1;
            entry.offset = std::uint64_t{be32(record + 4)} << shift;
            entry.length = be32(record + 8);
        }
        entries_.push_back(std::move(entry));
        if (entries_.back().directory) {
            if (parents.size() == max_depth) throw DiscError("damaged game file table");
            parents.push_back(i);
        }
    }
}

const FstEntry& FileTable::entry(std::uint32_t index) const {
    if (index >= entries_.size()) throw DiscError("game file table entry " + std::to_string(index) + " does not exist");
    return entries_[index];
}

std::string FileTable::path(std::uint32_t index) const {
    std::string result;
    for (const auto* e = &entry(index); index; index = e->parent, e = &entry(index)) {
        result = '/' + e->name + result;
        if (result.size() > max_path) throw DiscError("game file table path is too long");
    }
    return result.empty() ? "/" : result;
}

std::string FileTable::host_path(std::uint32_t index) const {
    std::string result;
    for (const auto* e = &entry(index); index; index = e->parent, e = &entry(index)) {
        result = '/' + host_name(e->name) + result;
        if (result.size() > max_path) throw DiscError("game file table path is too long");
    }
    return result.empty() ? "/" : result;
}

std::optional<std::uint32_t> FileTable::resolve(std::string_view text, std::uint32_t cwd) const {
    if (!entry(cwd).directory || text.size() > max_path || text.find('\0') != std::string_view::npos)
        throw DiscError("invalid game file lookup");
    while (!text.empty()) {
        if (text.front() == '/') {
            cwd = 0;
            text.remove_prefix(1);
            continue;
        }
        const auto end = text.find('/');
        const auto part = text.substr(0, end);
        if (part == "..") {
            cwd = entry(cwd).parent;
        } else if (part != ".") {
            std::optional<std::uint32_t> found;
            for (auto i = cwd + 1; i < entry(cwd).next; i = entry(i).next) {
                const auto& e = entry(i);
                if ((end == std::string_view::npos || e.directory) && same(part, e.name)) {
                    found = i;
                    break;
                }
            }
            if (!found) return std::nullopt;
            cwd = *found;
        }
        if (end == std::string_view::npos) return cwd;
        text.remove_prefix(end + 1);
    }
    return cwd;
}

} // namespace starlet::disc
