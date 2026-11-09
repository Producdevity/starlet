#include "starlet/gx/gles/program_cache.hpp"

#include <GLES3/gl32.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <stdexcept>
#include <sys/stat.h>
#include <vector>

namespace starlet::gx {
namespace {
constexpr char magic[8] = {'G', 'X', 'P', 'R', 'O', 'G', '0', '1'};
constexpr std::uint64_t fnv_basis = 14695981039346656037ull;

std::uint64_t fnv(std::uint64_t h, const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
std::string gl_string(GLenum name) {
    const auto* s = glGetString(name);
    return s ? reinterpret_cast<const char*>(s) : "";
}
void pending_error(const char* operation) {
    const auto error = glGetError();
    if (error != GL_NO_ERROR) throw std::runtime_error(std::string(operation) + ": GLES error " + std::to_string(error));
}
void put32(std::vector<unsigned char>& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>(value >> (8 * i)));
}
void put(std::vector<unsigned char>& out, const std::string& text) {
    put32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}
struct Reader {
    const std::vector<unsigned char>& data;
    std::size_t at = 0;
    bool u32(std::uint32_t& value) {
        if (data.size() - at < 4) return false;
        value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(data[at + i]) << (8 * i);
        at += 4;
        return true;
    }
    bool span(std::size_t size, const unsigned char*& out) {
        if (data.size() - at < size) return false;
        out = data.data() + at;
        at += size;
        return true;
    }
    bool text(const std::string& expected) {
        std::uint32_t size = 0;
        const unsigned char* bytes = nullptr;
        return u32(size) && size == expected.size() && span(size, bytes) && !std::memcmp(bytes, expected.data(), size);
    }
};
bool read_file(const std::string& path, std::vector<unsigned char>& out) {
    auto* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    bool ok = !std::fseek(file, 0, SEEK_END);
    const long size = ok ? std::ftell(file) : -1;
    ok = size > 0 && !std::fseek(file, 0, SEEK_SET);
    if (ok) {
        out.resize(static_cast<std::size_t>(size));
        ok = std::fread(out.data(), 1, out.size(), file) == out.size();
    }
    std::fclose(file);
    return ok;
}
bool write_file(const std::string& path, const std::vector<unsigned char>& data) {
    const auto temporary = path + ".tmp";
    auto* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    if (!std::fclose(file) && written && !std::rename(temporary.c_str(), path.c_str())) return true;
    std::remove(temporary.c_str());
    return false;
}
// Entries are named by 16 lowercase hex digits and ".bin", with ".tmp" appended while written.
bool entry_name(const char* name, bool& temporary) {
    for (unsigned i = 0; i < 16; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    if (std::strncmp(name + 16, ".bin", 4)) return false;
    temporary = !std::strcmp(name + 20, ".tmp");
    return temporary || name[20] == '\0';
}
} // namespace

GlesProgramCache::GlesProgramCache(std::string directory) : directory_(std::move(directory)) {
    if (directory_.empty()) return;
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    if (formats <= 0) return;
    identity_ = gl_string(GL_VENDOR) + '\n' + gl_string(GL_RENDERER) + '\n' + gl_string(GL_VERSION) + '\n' +
                gl_string(GL_SHADING_LANGUAGE_VERSION);
    if (::mkdir(directory_.c_str(), 0755) && errno != EEXIST) return;
    auto* dir = ::opendir(directory_.c_str());
    if (!dir) return;
    std::vector<std::string> entries, temporaries;
    while (const auto* e = ::readdir(dir)) {
        bool temporary = false;
        if (entry_name(e->d_name, temporary)) (temporary ? temporaries : entries).push_back(directory_ + '/' + e->d_name);
    }
    ::closedir(dir);
    for (const auto& name : temporaries) std::remove(name.c_str());
    // Entries of older shader generators or drivers never match again; they are the
    // oldest, and anything removed by mistake is rebuilt on its next use.
    if (entries.size() > max_entries) {
        std::vector<std::pair<long long, const std::string*>> ages;
        for (const auto& name : entries) {
            struct stat s{};
            ages.emplace_back(::stat(name.c_str(), &s) ? 0 : static_cast<long long>(s.st_mtime), &name);
        }
        std::sort(ages.begin(), ages.end());
        for (std::size_t i = 0; i < entries.size() - max_entries * 3 / 4; ++i) std::remove(ages[i].second->c_str());
    }
    enabled_ = true;
}

std::string GlesProgramCache::path(const std::string& vertex, const std::string& fragment) const {
    auto h = fnv(fnv_basis, identity_.data(), identity_.size() + 1);
    h = fnv(h, vertex.data(), vertex.size() + 1);
    h = fnv(h, fragment.data(), fragment.size());
    char name[24];
    std::snprintf(name, sizeof name, "/%016llx.bin", static_cast<unsigned long long>(h));
    return directory_ + name;
}

unsigned GlesProgramCache::load(const std::string& vertex, const std::string& fragment) {
    if (!enabled_) return 0;
    const auto file = path(vertex, fragment);
    std::vector<unsigned char> data;
    if (!read_file(file, data)) {
        ++misses_;
        return 0;
    }
    // The trailing checksum covers everything before it, so a torn write is rejected
    // before the driver sees the binary.
    bool valid = data.size() > sizeof magic + 8 && !std::memcmp(data.data(), magic, sizeof magic);
    if (valid) {
        std::uint64_t sum = 0;
        for (unsigned i = 0; i < 8; ++i) sum |= std::uint64_t(data[data.size() - 8 + i]) << (8 * i);
        valid = sum == fnv(fnv_basis, data.data(), data.size() - 8);
    }
    Reader in{data, sizeof magic};
    std::uint32_t format = 0, size = 0;
    const unsigned char* binary = nullptr;
    valid = valid && in.text(identity_) && in.text(vertex) && in.text(fragment) && in.u32(format) && in.u32(size) &&
            size && in.span(size, binary) && data.size() - in.at == 8;
    if (!valid) {
        ++rejected_;
        std::remove(file.c_str());
        return 0;
    }
    pending_error("program cache load");
    const auto program = glCreateProgram();
    if (!program) throw std::runtime_error("GLES program allocation failed");
    glProgramBinary(program, format, binary, static_cast<GLsizei>(size));
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (glGetError() != GL_NO_ERROR || !linked) {
        glDeleteProgram(program);
        ++rejected_;
        std::remove(file.c_str());
        return 0;
    }
    ++hits_;
    return program;
}

void GlesProgramCache::prepare(unsigned program) const {
    if (enabled_) glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
}

void GlesProgramCache::store(unsigned program, const std::string& vertex, const std::string& fragment) {
    if (!enabled_) return;
    pending_error("program cache store");
    GLint size = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &size);
    std::vector<unsigned char> binary(size > 0 ? static_cast<std::size_t>(size) : 0);
    GLsizei written = 0;
    GLenum format = 0;
    if (size > 0) glGetProgramBinary(program, size, &written, &format, binary.data());
    if (glGetError() != GL_NO_ERROR || written <= 0) {
        ++store_failures_;
        return;
    }
    std::vector<unsigned char> data(magic, magic + sizeof magic);
    put(data, identity_);
    put(data, vertex);
    put(data, fragment);
    put32(data, format);
    put32(data, static_cast<std::uint32_t>(written));
    data.insert(data.end(), binary.begin(), binary.begin() + written);
    const auto sum = fnv(fnv_basis, data.data(), data.size());
    for (unsigned i = 0; i < 8; ++i) data.push_back(static_cast<unsigned char>(sum >> (8 * i)));
    if (write_file(path(vertex, fragment), data)) ++stored_;
    else ++store_failures_;
}

bool GlesProgramCache::contains(const std::string& vertex, const std::string& fragment) const {
    struct stat s{};
    return enabled_ && !::stat(path(vertex, fragment).c_str(), &s);
}

GlesProgramCacheStats GlesProgramCache::stats() const {
    return {hits_.load(), misses_.load(), rejected_.load(), stored_.load(), store_failures_.load()};
}

namespace {
unsigned compile(GLenum kind, const std::string& source) {
    const auto shader = glCreateShader(kind);
    if (!shader) throw std::runtime_error("GLES shader allocation failed");
    const char* text = source.c_str(); glShaderSource(shader, 1, &text, nullptr); glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]{}; glGetShaderInfoLog(shader, sizeof log, nullptr, log); glDeleteShader(shader);
        throw std::runtime_error(std::string("GLES shader: ") + log);
    }
    return shader;
}
} // namespace

unsigned link_program(const std::string& vertex_source, const std::string& fragment_source, GlesProgramCache& cache) {
    if (const auto cached = cache.load(vertex_source, fragment_source)) return cached;
    GLuint vertex = 0, fragment = 0, program = 0;
    try {
        vertex = compile(GL_VERTEX_SHADER, vertex_source);
        fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
        program = glCreateProgram();
        if (!program) throw std::runtime_error("GLES program allocation failed");
        glAttachShader(program, vertex); glAttachShader(program, fragment);
        cache.prepare(program); glLinkProgram(program);
        GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096]{}; glGetProgramInfoLog(program, sizeof log, nullptr, log);
            throw std::runtime_error(std::string("GLES link: ") + log);
        }
        glDeleteShader(vertex); glDeleteShader(fragment);
        cache.store(program, vertex_source, fragment_source);
        return program;
    } catch (...) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        if (program) glDeleteProgram(program);
        throw;
    }
}

} // namespace starlet::gx
