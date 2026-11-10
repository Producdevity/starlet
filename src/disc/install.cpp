#include "starlet/disc/install.hpp"
#include "starlet/disc/fst.hpp"
#include "starlet/disc/sha1.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <ftw.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace starlet::disc {
namespace {
constexpr std::size_t copy_block = 1 << 20;
constexpr std::uint32_t gamecube_magic = 0xc2339f3d, wii_magic = 0x5d1c9ea3;
constexpr std::uint64_t boot_size = 0x440, bi2_size = 0x2000, apploader_at = 0x2440, spare_space = 64u << 20;

std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}
std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}
bool ends_with(const std::string& text, const char* suffix) {
    const auto n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}
bool exists(const std::string& path) { struct stat info {}; return ::lstat(path.c_str(), &info) == 0; }
void make_directory(const std::string& path) {
    if (::mkdir(path.c_str(), 0755)) throw DiscError("cannot create " + path + ": " + std::strerror(errno));
}
void remove_tree(const std::string& path) {
    if (!exists(path)) return;
    const auto removed = ::nftw(path.c_str(), [](const char* name, const struct stat*, int, struct FTW*) {
        return ::remove(name);
    }, 16, FTW_DEPTH | FTW_PHYS);
    if (removed) throw DiscError("cannot remove " + path);
}
// Cleanup after a failure must not replace the error being reported.
void discard(const std::string& path) noexcept {
    try { remove_tree(path); } catch (const DiscError&) {}
}

class Output {
public:
    explicit Output(const std::string& path) : path_(path), fd_(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) {
        if (fd_ < 0) throw DiscError("cannot create " + path + ": " + std::strerror(errno));
    }
    ~Output() { if (fd_ >= 0) ::close(fd_); }
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    void write(const void* data, std::size_t size) {
        hash_.update(data, size);
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        while (size) {
            const auto count = ::write(fd_, bytes, size);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw DiscError("cannot write " + path_ + " (is the card full?)");
            bytes += count;
            size -= static_cast<std::size_t>(count);
        }
    }
    // The SHA-1 of everything written.
    std::string close() {
        const auto fd = fd_;
        fd_ = -1;
        if (::close(fd)) throw DiscError("cannot finish writing " + path_);
        return hash_.hex();
    }

private:
    std::string path_;
    int fd_;
    Sha1 hash_;
};

std::string describe(const std::uint8_t* header) {
    std::string id;
    for (unsigned i = 0; i < 6; ++i) id += header[i] >= 0x20 && header[i] < 0x7f ? static_cast<char>(header[i]) : '?';
    return id + (header[6] ? " disc " + std::to_string(header[6] + 1) : std::string()) + " revision " + std::to_string(header[7]);
}

std::string format_list(unsigned accepted) {
    std::vector<std::string> names;
    if (accepted & formats::rvz) names.emplace_back("RVZ");
    if (accepted & formats::wia) names.emplace_back("WIA");
    if (accepted & formats::iso) { names.emplace_back("ISO"); names.emplace_back("GCM"); }
    std::string text;
    for (std::size_t i = 0; i < names.size(); ++i)
        text += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + names[i];
    return text;
}

// One output file or directory and the volume bytes it holds.
struct Span {
    std::string path;
    std::uint64_t offset = 0, size = 0;
    bool directory = false;
};

SupportedDisc check_disc(DiscImage& image, const Game& game, std::uint64_t& dol_size) {
    std::uint8_t header[0x20];
    if (image.size() < sizeof header) throw DiscError("not a GameCube or Wii disc image");
    image.read(0, header, sizeof header);
    if (be32(header + 0x1c) != gamecube_magic && be32(header + 0x18) != wii_magic)
        throw DiscError("not a GameCube or Wii disc image");
    const auto found = describe(header);
    const auto disc = std::find_if(game.discs.begin(), game.discs.end(), [&](const SupportedDisc& d) {
        return d.id.size() == 6 && !std::memcmp(header, d.id.data(), 6) && header[6] == d.disc && header[7] == d.revision;
    });
    if (disc == game.discs.end()) throw DiscError("this is not " + game.title + ": found " + found);
    GameVolume volume(image);
    const auto dol = volume.main_dol();
    const auto digest = sha1_hex(dol.data(), dol.size());
    if (digest != disc->dol_sha1) throw DiscError("main.dol of " + found + " is not a supported release (SHA-1 " + digest + ")");
    dol_size = dol.size();
    return *disc;
}
}

Manifest parse_manifest(const std::string& text) {
    Manifest manifest;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        if (line.size() < 43 || line[40] != ' ' || line[41] != ' ' ||
            !std::all_of(line.begin(), line.begin() + 40, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            throw DiscError("damaged file manifest");
        if (!manifest.emplace(line.substr(42), line.substr(0, 40)).second) throw DiscError("duplicate file manifest entry");
    }
    return manifest;
}

std::string find_disc_image(const std::string& directory, unsigned accepted, const std::string& title) {
    if (!(accepted & formats::all)) throw DiscError("no disc image format is accepted");
    DIR* dir = ::opendir(directory.c_str());
    if (!dir) throw DiscError(directory + " is missing");
    static const char* const unsupported[] = {".nkit.iso", ".nkit.gcz", ".ciso", ".gcz", ".wbfs", ".wdf", ".tgc"};
    std::vector<std::string> supported, other, unknown;
    while (const auto* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.empty() || name[0] == '.' || name == "README.txt") continue;
        const auto path = directory + "/" + name;
        struct stat info {};
        if (::stat(path.c_str(), &info)) continue;
        const auto key = lower(name);
        const bool rvz = ends_with(key, ".rvz"), wia = ends_with(key, ".wia"), iso = ends_with(key, ".iso") || ends_with(key, ".gcm");
        if (S_ISDIR(info.st_mode)) unknown.push_back(name + "/");
        else if (!S_ISREG(info.st_mode)) continue;
        else if (std::any_of(std::begin(unsupported), std::end(unsupported), [&](const char* s) { return ends_with(key, s); }))
            other.push_back(name);
        else if ((rvz && accepted & formats::rvz) || (wia && accepted & formats::wia) || (iso && accepted & formats::iso))
            supported.push_back(path);
        else if (rvz || wia || iso) other.push_back(name);
        else unknown.push_back(name);
    }
    ::closedir(dir);
    std::sort(other.begin(), other.end());
    std::sort(unknown.begin(), unknown.end());
    const auto list = format_list(accepted);
    if (supported.size() > 1) throw DiscError("put only one disc image in " + directory);
    if (supported.empty() && !other.empty()) throw DiscError(other.front() + ": only " + list + " disc images are supported");
    if (supported.empty())
        throw DiscError("copy your " + title + " disc image (" + list + ") to " + directory +
                        (unknown.empty() ? std::string(", then launch the game again") : "; " + unknown.front() + " is not a disc image"));
    return supported.front();
}

SupportedDisc identify_disc(DiscImage& image, const Game& game) {
    std::uint64_t dol_size = 0;
    return check_disc(image, game, dol_size);
}

Extracted extract(DiscImage& image, const std::string& work, const Game& game, const SetupOptions& options) {
    Extracted result;
    std::uint64_t dol_size = 0;
    result.disc = check_disc(image, game, dol_size);
    GameVolume volume(image);
    const auto shift = volume.shift();
    const auto boot = volume.boot();
    const auto fst = volume.fst();
    const FileTable table(fst, shift);

    std::vector<Span> spans;
    const auto system = options.system_files;
    if (system & sys_files::boot) spans.push_back({"sys/boot.bin", 0, boot_size});
    if (system & sys_files::bi2) spans.push_back({"sys/bi2.bin", boot_size, bi2_size});
    if (system & sys_files::apploader) {
        std::uint8_t loader[0x20];
        volume.read(apploader_at, loader, sizeof loader);
        const std::uint64_t size = 0x20 + std::uint64_t{be32(loader + 0x14)} + be32(loader + 0x18);
        if (size > 4u << 20) throw DiscError("damaged apploader");
        spans.push_back({"sys/apploader.img", apploader_at, size});
    }
    if (system & sys_files::fst) spans.push_back({"sys/fst.bin", std::uint64_t{be32(boot.data() + 0x424)} << shift, fst.size()});
    if (system & sys_files::dol) spans.push_back({"sys/main.dol", std::uint64_t{be32(boot.data() + 0x420)} << shift, dol_size});
    for (std::uint32_t i = 1; i < table.size(); ++i) {
        const auto& entry = table.entry(i);
        spans.push_back({"files" + table.host_path(i), entry.offset, entry.length, entry.directory});
    }
    std::uint64_t total = 0;
    for (const auto& span : spans) {
        if (span.directory) continue;
        if (span.offset > volume.size() || span.size > volume.size() - span.offset)
            throw DiscError("the disc image is incomplete: " + span.path + " lies past its end");
        total += span.size;
    }
    const auto slash = work.find_last_of('/');
    struct statvfs space {};
    if (::statvfs(slash == std::string::npos ? "." : slash == 0 ? "/" : work.substr(0, slash).c_str(), &space) == 0 &&
        std::uint64_t{space.f_bavail} * space.f_frsize < total + spare_space)
        throw DiscError("not enough free space: " + std::to_string((total + spare_space) >> 20) + " MiB needed");

    make_directory(work);
    try {
        make_directory(work + "/gamedata");
        if (system & sys_files::all) make_directory(work + "/gamedata/sys");
        make_directory(work + "/gamedata/files");
        const auto* manifest = result.disc.manifest;
        std::vector<std::uint8_t> block(copy_block);
        std::uint64_t done = 0, written = 0;
        for (const auto& span : spans) {
            const auto path = work + "/gamedata/" + span.path;
            if (span.directory) {
                make_directory(path);
                continue;
            }
            Output out(path);
            for (std::uint64_t at = 0; at < span.size;) {
                const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), span.size - at));
                volume.read(span.offset + at, block.data(), take);
                out.write(block.data(), take);
                at += take;
                done += take;
                if (options.progress) options.progress(done, total);
            }
            const auto digest = out.close();
            if (manifest) {
                const auto expected = manifest->find(span.path);
                if (expected == manifest->end() || expected->second != digest)
                    throw DiscError("extracted file does not match the reference: " + span.path);
            }
            ++written;
            if (span.path.compare(0, 6, "files/") == 0) {
                ++result.files;
                result.bytes += span.size;
            }
        }
        if (manifest && written != manifest->size()) throw DiscError("disc image lacks files listed in the reference");
        if (options.hook) options.hook(image, result.disc, work);
    } catch (...) {
        discard(work);
        throw;
    }
    return result;
}

std::string prepared_marker(const Game& game, const SupportedDisc& disc) {
    return disc.id + " revision " + std::to_string(disc.revision) + ", setup " + std::to_string(game.setup) + "\n";
}

bool is_prepared(const std::string& port, const Game& game) {
    std::ifstream in(port + "/gamedata/.prepared", std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    return std::any_of(game.discs.begin(), game.discs.end(),
                       [&](const SupportedDisc& disc) { return text.str() == prepared_marker(game, disc); });
}

Extracted install(DiscImage& image, const std::string& port, const Game& game, const SetupOptions& options) {
    for (const auto& name : options.outputs)
        if (name.empty() || name == "." || name == ".." || name == "gamedata" || name.find('/') != std::string::npos)
            throw DiscError("invalid setup output " + name);
    const auto work = port + "/.setup-work";
    remove_tree(work);
    const auto result = extract(image, work, game, options);
    try {
        Output marker(work + "/gamedata/.prepared");
        const auto text = prepared_marker(game, result.disc);
        marker.write(text.data(), text.size());
        marker.close();
        ::sync();
        auto names = options.outputs;
        names.emplace_back("gamedata");
        for (const auto& name : names) {
            const auto target = port + "/" + name, old = port + "/.setup-old-" + name;
            remove_tree(old);
            const bool replacing = exists(target);
            if (replacing && ::rename(target.c_str(), old.c_str()))
                throw DiscError("cannot replace " + target + ": " + std::strerror(errno));
            if (::rename((work + "/" + name).c_str(), target.c_str())) {
                const auto error = errno;
                if (replacing) ::rename(old.c_str(), target.c_str());
                throw DiscError("cannot publish " + target + ": " + std::strerror(error));
            }
            remove_tree(old);
        }
        remove_tree(work);
        ::sync();
    } catch (...) {
        discard(work);
        throw;
    }
    return result;
}

} // namespace starlet::disc
