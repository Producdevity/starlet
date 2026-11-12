#include "starlet/card/card.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace starlet::card {
namespace {

constexpr const char* identity_name = "card.txt";
constexpr std::size_t segment = 512;  // CARD_SEG_SIZE: read granularity.
constexpr std::uint32_t read_size = 512;  // CARD_READ_SIZE: icon data must start below.
constexpr std::uint32_t comment_size = 64;
constexpr std::uint8_t attr_public = 0x04;
constexpr std::uint8_t attr_mask = 0x1C;  // The bits CARDSetAttributes may change.

std::string error_text(int error) { return std::strerror(error); }

bool make_folders(const std::string& path) {
    std::string partial;
    std::size_t at = 0;
    while (at != std::string::npos) {
        at = path.find('/', at + 1);
        partial = path.substr(0, at);
        if (partial.empty()) continue;
        if (mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool read_file(const std::string& path, std::vector<std::uint8_t>& out, int& error) {
    error = 0;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = errno;
        return false;
    }
    struct stat info {};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        error = errno ? errno : EINVAL;
        close(fd);
        return false;
    }
    out.resize(std::size_t(info.st_size));
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = pread(fd, out.data() + done, out.size() - done, off_t(done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            error = n < 0 ? errno : EIO;
            break;
        }
        done += std::size_t(n);
    }
    close(fd);
    return error == 0;
}

// The bytes go to a temporary file, which is synced and renamed over the old one;
// syncing the folder makes the rename durable. A power cut leaves either file.
// Some file systems (exFAT) refuse to sync a directory; the rename stands.
int replace_file(const std::string& folder, const std::string& name, const std::vector<std::uint8_t>& bytes) {
    const std::string path = folder + "/" + name, temporary = path + ".tmp";
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return errno;
    int error = 0;
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            error = n < 0 ? errno : EIO;
            break;
        }
        done += std::size_t(n);
    }
    if (!error && fsync(fd) != 0) error = errno;
    if (close(fd) != 0 && !error) error = errno;
    if (!error && std::rename(temporary.c_str(), path.c_str()) != 0) error = errno;
    if (error) {
        unlink(temporary.c_str());
        return error;
    }
    const int dir = open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) {
        fsync(dir);
        close(dir);
    }
    return 0;
}

bool list_gci(const std::string& folder, std::vector<std::string>& names) {
    DIR* dir = opendir(folder.c_str());
    if (!dir) return false;
    while (const dirent* item = readdir(dir)) {
        const std::size_t n = std::strlen(item->d_name);
        if (n > 4 && strcasecmp(item->d_name + n - 4, ".gci") == 0) names.emplace_back(item->d_name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return true;
}

std::string stamp(const char* prefix) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char text[48];
    std::strftime(text, sizeof text, "-%Y%m%d-%H%M%S", &local);
    return prefix + std::string(text);
}

std::uint64_t now_os_time() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    // Sub-second ticks keep two formats in one second apart.
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count() % 1000000;
    return os_time(std::int64_t(now), local.tm_gmtoff) + std::uint64_t(micros) * 40500000u / 1000000u;
}

}

Region region_of(char c) {
    switch (c) {
    case 'J': case 'K': case 'W': return Region::japan;
    case 'E': return Region::usa;
    default: return Region::europe;
    }
}

std::string folder_for(const std::string& saves_dir, Region region, int slot) {
    const char* name = region == Region::usa ? "USA" : region == Region::japan ? "JAP" : "EUR";
    return saves_dir + "/" + name + "/Card " + char('A' + slot);
}

// ---- Folder ----

Folder::Folder(std::string path, int slot, Console console, int size_mbits, Log log)
    : path_(std::move(path)), slot_(slot), size_mbits_(size_mbits), console_(console), log_(std::move(log)) {
    if (!valid_size(size_mbits_) || slot_ < 0 || slot_ > 1) {
        report("invalid card size or slot for " + path_);
        return;
    }
    if (!make_folders(path_)) {
        report("no card: cannot create " + path_ + ": " + error_text(errno));
        return;
    }
    if (access(path_.c_str(), R_OK | W_OK | X_OK) != 0) {
        report("no card: cannot read and write " + path_ + ": " + error_text(errno));
        return;
    }
    ok_ = true;
}

void Folder::report(const std::string& line) const {
    if (log_) log_(line);
    else std::fprintf(stderr, "starlet: card: %s\n", line.c_str());
}

Result Folder::write_identity(const Identity& id) {
    char text[96];
    const int n = std::snprintf(text, sizeof text, "format_time %016llx\nencoding %u\n",
                                static_cast<unsigned long long>(id.format_time), unsigned(id.encoding));
    const int error = replace_file(path_, identity_name, std::vector<std::uint8_t>(text, text + n));
    if (error) {
        report("cannot write " + path_ + "/" + identity_name + ": " + error_text(error));
        return io_error;
    }
    return ready;
}

Result Folder::load_identity(Identity& id) {
    id = Identity{};
    id.flash_id = console_.flash_id[slot_];
    id.counter_bias = console_.counter_bias;
    id.language = console_.language;
    id.vi_dtv = console_.vi_dtv;
    std::vector<std::uint8_t> bytes;
    int error = 0;
    unsigned long long time = 0;
    unsigned encoding = 0;
    int end = -1;
    if (read_file(path_ + "/" + identity_name, bytes, error)) {
        const std::string text(bytes.begin(), bytes.end());
        if (std::sscanf(text.c_str(), "format_time %16llx encoding %u %n", &time, &encoding, &end) == 2 &&
            end == int(text.size()) && encoding <= 1) {
            id.format_time = time;
            id.encoding = Encoding(encoding);
            return ready;
        }
        report(path_ + "/" + identity_name + " is damaged; the card gets a new identity");
    }
    id.format_time = now_os_time();
    id.encoding = console_.encoding;
    return write_identity(id);
}

Result Folder::load_files(std::vector<Stored>& out) {
    out.clear();
    std::vector<std::string> names;
    if (!list_gci(path_, names)) {
        report("cannot read " + path_ + ": " + error_text(errno));
        return io_error;
    }
    std::size_t blocks = 0;
    const std::size_t capacity = std::size_t(blocks_for(size_mbits_) - system_blocks);
    for (const std::string& name : names) {
        const std::string full = path_ + "/" + name;
        std::vector<std::uint8_t> bytes;
        int error = 0;
        if (!read_file(full, bytes, error)) {
            report("skipping " + full + ": " + error_text(error));
            continue;
        }
        auto file = read_gci(bytes.data(), bytes.size());
        if (!file) {
            report("skipping " + full + ": not a GCI file (directory entry and its blocks)");
            continue;
        }
        const auto twin = std::find_if(out.begin(), out.end(), [&](const Stored& s) { return s.file.entry.same_file(file->entry); });
        if (twin != out.end()) {
            report("skipping " + full + ": the card already holds this file as " + twin->host_name);
            continue;
        }
        if (out.size() == std::size_t(max_files) || blocks + file->entry.length() > capacity) {
            report("skipping " + full + ": the card is full");
            continue;
        }
        blocks += file->entry.length();
        out.push_back({std::move(*file), name});
    }
    return ready;
}

Result Folder::save(Stored& s) {
    if (s.host_name.empty()) {
        const std::string base = gci_name(s.file.entry);
        std::string name = base;
        for (int n = 2; access((path_ + "/" + name).c_str(), F_OK) == 0; ++n)
            name = base.substr(0, base.size() - 4) + "-" + std::to_string(n) + ".gci";
        s.host_name = name;
    }
    const int error = replace_file(path_, s.host_name, write_gci(s.file));
    if (error) {
        report("cannot write " + path_ + "/" + s.host_name + ": " + error_text(error));
        return io_error;
    }
    return ready;
}

Result Folder::move_aside(const std::vector<std::string>& names, const char* prefix) {
    if (names.empty()) return ready;
    std::string aside = path_ + "/" + stamp(prefix);
    for (int n = 2; mkdir(aside.c_str(), 0755) != 0; ++n) {
        if (errno != EEXIST || n > 99) {
            report("cannot create " + aside + ": " + error_text(errno));
            return io_error;
        }
        aside = path_ + "/" + stamp(prefix) + "-" + std::to_string(n);
    }
    for (const std::string& name : names) {
        if (std::rename((path_ + "/" + name).c_str(), (aside + "/" + name).c_str()) != 0) {
            report("cannot move " + name + " to " + aside + ": " + error_text(errno));
            return io_error;
        }
    }
    return ready;
}

Result Folder::remove(const Stored& s) { return move_aside({s.host_name}, "removed"); }

void Folder::discard(const Stored& s) {
    if (unlink((path_ + "/" + s.host_name).c_str()) != 0) report("cannot delete " + path_ + "/" + s.host_name + ": " + error_text(errno));
}

Result Folder::format(Identity& id) {
    std::vector<std::string> names;
    if (!list_gci(path_, names)) {
        report("cannot read " + path_ + ": " + error_text(errno));
        return io_error;
    }
    if (move_aside(names, "formatted") != ready) return io_error;
    load_identity(id);
    id.format_time = now_os_time();
    id.encoding = console_.encoding;
    return write_identity(id);
}

std::vector<std::uint8_t> Folder::image() {
    Identity id;
    load_identity(id);
    std::vector<Stored> stored;
    load_files(stored);
    std::vector<File> files;
    for (auto& s : stored) files.push_back(s.file);
    auto built = build_image(size_mbits_, id, files);
    return built ? std::move(*built) : std::vector<std::uint8_t>(std::size_t(blocks_for(size_mbits_)) * block_size, 0xFF);
}

Result Folder::store_image(const std::uint8_t* image, std::size_t size) {
    Identity id;
    load_identity(id);
    // A card formatted on the device carries a new format time; same flash ID.
    const Parsed parsed = parse_image(image, size, id.flash_id, console_.encoding);
    if (parsed.result != ready && parsed.result != encoding) return parsed.result;
    std::vector<Stored> stored;
    if (load_files(stored) != ready) return io_error;
    std::vector<std::string> gone;
    for (const Stored& s : stored)
        if (std::none_of(parsed.files.begin(), parsed.files.end(), [&](const File& f) { return f.entry.same_file(s.file.entry); }))
            gone.push_back(s.host_name);
    Result result = move_aside(gone, "removed");
    for (const File& f : parsed.files) {
        auto old = std::find_if(stored.begin(), stored.end(), [&](const Stored& s) { return s.file.entry.same_file(f.entry); });
        Stored s{f, old != stored.end() ? old->host_name : std::string()};
        s.file.entry.set_first_block(old != stored.end() ? old->file.entry.first_block() : f.entry.first_block());
        if (old != stored.end() && old->file.data == s.file.data && old->file.entry.bytes == s.file.entry.bytes) continue;
        if (save(s) != ready) result = io_error;
    }
    const std::uint64_t format_time = [&] {
        std::uint64_t t = 0;
        for (int i = 0; i < 8; ++i) t = t << 8 | parsed.serial[12 + i];
        return t;
    }();
    if (format_time != id.format_time || parsed.encoding != id.encoding) {
        id.format_time = format_time;
        id.encoding = parsed.encoding;
        if (write_identity(id) != ready) result = io_error;
    }
    return result;
}

Result Folder::import_image(const std::uint8_t* image, std::size_t size) {
    if (size < 32) return broken;
    Parsed parsed = parse_image(image, size, flash_id_of(image), console_.encoding);
    if (parsed.result == encoding) parsed.result = ready;  // The files are what matter here.
    if (parsed.result != ready) return parsed.result;
    std::vector<Stored> stored;
    if (load_files(stored) != ready) return io_error;
    Result result = ready;
    for (const File& f : parsed.files) {
        auto old = std::find_if(stored.begin(), stored.end(), [&](const Stored& s) { return s.file.entry.same_file(f.entry); });
        Stored s{f, old != stored.end() ? old->host_name : std::string()};
        if (save(s) != ready) result = io_error;
    }
    return result;
}

// ---- Card ----

Card::Card(std::shared_ptr<Folder> folder, const char game[4], const char company[2]) : folder_(std::move(folder)) {
    std::memcpy(game_, game, 4);
    std::memcpy(company_, company, 2);
}

Result Card::mount() {
    unmount();
    if (!present()) return no_card;
    if (folder_->load_identity(identity_) != ready) return io_error;
    std::vector<Folder::Stored> stored;
    if (folder_->load_files(stored) != ready) return io_error;
    files_.clear();
    files_.resize(max_files);
    used_.assign(std::size_t(blocks_for(folder_->size_mbits())), false);
    for (std::size_t i = 0; i < stored.size(); ++i) {
        auto held = std::make_unique<Held>();
        held->stored = std::move(stored[i]);
        allocate(held->stored.file.entry.length(), held->blocks);
        for (const auto b : held->blocks) used_[b] = true;
        held->stored.file.entry.set_first_block(held->blocks[0]);
        files_[i] = std::move(held);
    }
    mounted_ = true;
    verdict_ = identity_.encoding == folder_->console().encoding ? ready : encoding;
    return verdict_;
}

void Card::unmount() {
    mounted_ = false;
    verdict_ = no_card;
    files_.clear();
    used_.clear();
}

Result Card::check() { return mounted_ ? verdict_ : no_card; }

Result Card::format() {
    if (!present()) return no_card;
    if (folder_->format(identity_) != ready) {
        unmount();
        return io_error;
    }
    return mount();
}

Result Card::usable() const {
    if (!mounted_) return no_card;
    return verdict_ == ready ? ready : broken;
}

bool Card::allocate(std::uint16_t length, std::vector<std::uint16_t>& blocks) const {
    blocks.clear();
    for (std::size_t b = system_blocks; b < used_.size() && blocks.size() < length; ++b)
        if (!used_[b]) blocks.push_back(std::uint16_t(b));
    return blocks.size() == length;
}

Result Card::free_blocks(std::int32_t& bytes, std::int32_t& files) const {
    if (const Result r = usable(); r != ready) return r;
    bytes = std::int32_t(block_size * std::size_t(std::count(used_.begin() + system_blocks, used_.end(), false)));
    files = std::int32_t(std::count(files_.begin(), files_.end(), nullptr));
    return ready;
}

Result Card::serial(std::uint64_t& out) const {
    if (!mounted_) return no_card;
    out = serial_number(serial_bytes(identity_));
    return ready;
}

// __CARDAccess: the boot disc's own files. The SDK's permission mask leaves the
// global and company attributes without effect.
Result Card::writable(int file_no) const {
    if (file_no < 0 || file_no >= max_files || !files_[file_no]) return no_file;
    const Entry& e = files_[file_no]->stored.file.entry;
    return std::memcmp(e.bytes.data(), game_, 4) == 0 && std::memcmp(e.bytes.data() + 4, company_, 2) == 0 ? ready : no_permission;
}

// __CARDIsReadable: public files of other games too.
Result Card::readable(int file_no) const {
    const Result r = writable(file_no);
    if (r == no_permission && (files_[file_no]->stored.file.entry.permission() & attr_public)) return ready;
    return r;
}

Result Card::open(const char* name, FileInfo& info) const {
    info.file_no = -1;
    if (const Result r = usable(); r != ready) return r;
    for (int n = 0; n < max_files; ++n) {
        if (writable(n) != ready || !files_[n]->stored.file.entry.name_is(name)) continue;
        info = {n, 0, files_[n]->blocks[0]};
        return ready;
    }
    return no_file;
}

Result Card::fast_open(int file_no, FileInfo& info) const {
    info.file_no = -1;
    if (file_no < 0 || file_no >= max_files) return fatal_error;
    if (const Result r = usable(); r != ready) return r;
    if (const Result r = readable(file_no); r != ready) return r;
    info = {file_no, 0, files_[file_no]->blocks[0]};
    return ready;
}

Result Card::create(const char* name, std::uint32_t size, FileInfo& info) {
    if (std::strlen(name) > name_size) return name_too_long;
    if (size == 0 || size % block_size != 0) return fatal_error;
    if (const Result r = usable(); r != ready) return r;
    int free_no = -1;
    for (int n = 0; n < max_files; ++n) {
        if (!files_[n]) {
            if (free_no < 0) free_no = n;
        } else if (writable(n) == ready && files_[n]->stored.file.entry.name_is(name)) {
            return exist;
        }
    }
    if (free_no < 0) return no_entry;
    auto held = std::make_unique<Held>();
    if (!allocate(std::uint16_t(size / block_size), held->blocks)) return insufficient_space;
    Entry& e = held->stored.file.entry;
    e = Entry{};
    e.set_owner(game_, company_);
    e.set_name(name);
    e.set_time(card_time_now());
    e.set_banner_format(0);
    e.set_icon_address(no_address);
    e.set_icon_format(0);
    e.set_icon_speed(1);  // CARD_STAT_SPEED_FAST for icon 0, END for the rest.
    e.set_permission(attr_public);
    e.set_copy_times(0);
    e.set_first_block(held->blocks[0]);
    e.set_length(std::uint16_t(size / block_size));
    e.set_comment_address(no_address);
    // A new file holds whatever its blocks held; here they read as zero.
    held->stored.file.data.assign(size, 0);
    if (folder_->save(held->stored) != ready) return io_error;
    for (const auto b : held->blocks) used_[b] = true;
    info = {free_no, 0, held->blocks[0]};
    files_[free_no] = std::move(held);
    return ready;
}

// __CARDSeek, then the SDK's transfer loop leaves the position on the last sector it
// touched.
Result Card::seek(FileInfo& info, std::int32_t length, std::int32_t offset) const {
    if (info.file_no < 0 || info.file_no >= max_files || offset < 0 || length < 0) return fatal_error;
    if (const Result r = usable(); r != ready) return r;
    if (!files_[info.file_no]) return no_file;
    const Held& h = *files_[info.file_no];
    const std::int64_t size = std::int64_t(h.stored.file.data.size());
    if (size <= offset || size < std::int64_t(offset) + length) return limit;
    std::int32_t last = offset;
    if (length > 0) last = std::max(offset, std::int32_t(std::uint32_t(offset + length - 1) & ~std::uint32_t(block_size - 1)));
    info.offset = last;
    info.block = h.blocks[std::size_t(last) / block_size];
    return ready;
}

Result Card::read(FileInfo& info, void* out, std::int32_t length, std::int32_t offset) const {
    if (offset % std::int32_t(segment) != 0 || length % std::int32_t(segment) != 0) return fatal_error;
    if (const Result r = seek(info, length, offset); r != ready) return r;
    if (const Result r = readable(info.file_no); r != ready) return r;
    if (length > 0) std::memcpy(out, files_[info.file_no]->stored.file.data.data() + offset, std::size_t(length));
    return ready;
}

Result Card::save_entry(int file_no, Entry entry, const std::vector<std::uint8_t>* data) {
    Held& h = *files_[file_no];
    Folder::Stored next{{entry, data ? *data : h.stored.file.data}, h.stored.host_name};
    if (folder_->save(next) != ready) return io_error;
    h.stored = std::move(next);
    return ready;
}

Result Card::write(FileInfo& info, const void* in, std::int32_t length, std::int32_t offset) {
    if (const Result r = seek(info, length, offset); r != ready) return r;
    if (offset % std::int32_t(block_size) != 0 || length % std::int32_t(block_size) != 0) return fatal_error;
    if (const Result r = writable(info.file_no); r != ready) return r;
    Held& h = *files_[info.file_no];
    std::vector<std::uint8_t> data = h.stored.file.data;
    if (length > 0) std::memcpy(data.data() + offset, in, std::size_t(length));
    Entry e = h.stored.file.entry;
    e.set_time(card_time_now());
    return save_entry(info.file_no, e, &data);
}

void Card::icon_offsets(const Entry& e, Stat& stat) {
    std::uint32_t offset = e.icon_address();
    if (offset == no_address) {
        stat.banner_format = 0;
        stat.icon_format = 0;
        stat.icon_speed = 0;
        offset = 0;
    }
    switch (e.banner_format() & 3) {
    case 1:  // C8
        stat.offset_banner = offset;
        offset += 96 * 32;
        stat.offset_banner_tlut = offset;
        offset += 2 * 256;
        break;
    case 2:  // RGB5A3
        stat.offset_banner = offset;
        offset += 2 * 96 * 32;
        stat.offset_banner_tlut = no_address;
        break;
    default:
        stat.offset_banner = stat.offset_banner_tlut = no_address;
        break;
    }
    bool palette = false;
    for (int i = 0; i < 8; ++i) {
        switch ((e.icon_format() >> (2 * i)) & 3) {
        case 1:
            stat.offset_icon[i] = offset;
            offset += 32 * 32;
            palette = true;
            break;
        case 2:
            stat.offset_icon[i] = offset;
            offset += 2 * 32 * 32;
            break;
        default:
            stat.offset_icon[i] = no_address;
            break;
        }
    }
    if (palette) {
        stat.offset_icon_tlut = offset;
        offset += 2 * 256;
    } else {
        stat.offset_icon_tlut = no_address;
    }
    stat.offset_data = offset;
}

Result Card::get_status(int file_no, Stat& stat) const {
    if (file_no < 0 || file_no >= max_files) return fatal_error;
    if (const Result r = usable(); r != ready) return r;
    if (const Result r = readable(file_no); r != ready) return r;
    const Entry& e = files_[file_no]->stored.file.entry;
    std::memcpy(stat.game, e.bytes.data(), 4);
    std::memcpy(stat.company, e.bytes.data() + 4, 2);
    stat.length = std::uint32_t(e.length()) * block_size;
    std::memcpy(stat.file_name, e.bytes.data() + 8, name_size);
    stat.time = e.time();
    stat.banner_format = e.banner_format();
    stat.icon_address = e.icon_address();
    stat.icon_format = e.icon_format();
    stat.icon_speed = e.icon_speed();
    stat.comment_address = e.comment_address();
    icon_offsets(e, stat);
    return ready;
}

Result Card::set_status(int file_no, const Stat& stat) {
    if (file_no < 0 || file_no >= max_files || (stat.icon_address != no_address && stat.icon_address >= read_size) ||
        (stat.comment_address != no_address && stat.comment_address % block_size > block_size - comment_size))
        return fatal_error;
    if (const Result r = usable(); r != ready) return r;
    if (const Result r = writable(file_no); r != ready) return r;
    Entry e = files_[file_no]->stored.file.entry;
    e.set_banner_format(stat.banner_format);
    e.set_icon_address(stat.icon_address);
    e.set_icon_format(stat.icon_format);
    e.set_icon_speed(stat.icon_speed);
    e.set_comment_address(stat.comment_address);
    if (e.icon_address() == no_address) e.set_icon_speed(std::uint16_t((e.icon_speed() & ~3u) | 1u));
    e.set_time(card_time_now());
    return save_entry(file_no, e, nullptr);
}

Result Card::get_attributes(int file_no, std::uint8_t& attributes) const {
    if (file_no < 0 || file_no >= max_files) return fatal_error;
    if (const Result r = usable(); r != ready) return r;
    if (const Result r = readable(file_no); r != ready) return r;
    attributes = files_[file_no]->stored.file.entry.permission();
    return ready;
}

Result Card::set_attributes(int file_no, std::uint8_t attributes) {
    if (file_no < 0 || file_no >= max_files || (attributes & ~attr_mask)) return fatal_error;
    if (const Result r = usable(); r != ready) return r;
    if (const Result r = writable(file_no); r != ready) return r;
    Entry e = files_[file_no]->stored.file.entry;
    e.set_permission(std::uint8_t((e.permission() & ~attr_mask) | attributes));
    return save_entry(file_no, e, nullptr);
}

Result Card::erase(int file_no) {
    if (file_no < 0 || file_no >= max_files) return fatal_error;
    if (const Result r = usable(); r != ready) return r;
    if (const Result r = writable(file_no); r != ready) return r;
    if (folder_->remove(files_[file_no]->stored) != ready) return io_error;
    for (const auto b : files_[file_no]->blocks) used_[b] = false;
    files_[file_no].reset();
    return ready;
}

Result Card::erase(const char* name) {
    FileInfo info;
    if (const Result r = open(name, info); r != ready) return r;
    return erase(info.file_no);
}

Result Card::rename(const char* old_name, const char* new_name) {
    if (std::strlen(old_name) > name_size || std::strlen(new_name) > name_size) return name_too_long;
    if (const Result r = usable(); r != ready) return r;
    int from = -1;
    for (int n = 0; n < max_files; ++n) {
        if (writable(n) != ready) continue;
        if (files_[n]->stored.file.entry.name_is(new_name)) return exist;
        if (files_[n]->stored.file.entry.name_is(old_name)) from = n;
    }
    if (from < 0) return no_file;
    Held& h = *files_[from];
    Folder::Stored renamed = h.stored;
    renamed.file.entry.set_name(new_name);
    renamed.file.entry.set_time(card_time_now());
    renamed.host_name.clear();
    if (folder_->save(renamed) != ready) return io_error;
    // The old file leaves the folder once the new one is in place.
    folder_->discard(h.stored);
    h.stored = std::move(renamed);
    return ready;
}

}
