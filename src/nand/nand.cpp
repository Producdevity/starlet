#include "starlet/nand/nand.hpp"
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <pthread.h>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

using namespace starlet::sdk;

namespace {
constexpr const char home[] = "/home";
constexpr u32 file_limit = 16 * 1024 * 1024;
struct Fd {
    int value = -1;
    ~Fd() { if (value >= 0) close(value); }
    Fd() = default;
    explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
struct Handle { NANDFileInfo* owner = nullptr; int fd = -1; s32 token = 0; u8 access = 0; };
std::mutex mutex;
int root_fd = -1, home_fd = -1, tmp_fd = -1;
enum class Operation { open, close, read, write, seek, length, create, remove, directory };
struct Request {
    NANDCommandBlock* block = nullptr;
    NANDCallback callback = nullptr;
    Operation operation{};
    NANDFileInfo* file = nullptr;
    void* buffer = nullptr;
    u32 count = 0;
    s32 offset = 0, whence = 0;
    u8 access = 0, permission = 0, attribute = 0;
    char path[NAND_MAX_PATH]{};
    std::uint64_t order = 0;
};
std::array<Request, 32> requests;
std::condition_variable changed;
pthread_t worker{};
bool stopping = false;
std::uint64_t next_order = 0;
void* run_requests(void*);
std::array<Handle, 16> handles;
s32 next_token = 1;
starlet::nand::Hooks hooks;
// A section the port's hooks bracket; it keeps the leave hook it entered with.
struct Section {
    void (*leave)() = hooks.leave;
    Section() { if (hooks.enter) hooks.enter(); }
    ~Section() { if (leave) leave(); }
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
};
struct Lock {
    Section section;
    std::lock_guard<std::mutex> lock{mutex};
};
s32 error(int value = errno) {
    switch (value) {
    case ENOENT: return NAND_RESULT_NOEXISTS;
    case EACCES: case EPERM: case ELOOP: case EROFS: return NAND_RESULT_ACCESS;
    case ENOSPC: case EDQUOT: case EFBIG: return NAND_RESULT_MAXBLOCKS;
    case EEXIST: return NAND_RESULT_EXISTS;
    case ENOTEMPTY: return NAND_RESULT_NOTEMPTY;
    case EMFILE: case ENFILE: return NAND_RESULT_MAXFD;
    case EINVAL: case EBADF: case ENAMETOOLONG: case ENOTDIR: case EISDIR: return NAND_RESULT_INVALID;
    default: return NAND_RESULT_UNKNOWN;
    }
}
struct Path {
    Fd parent;
    char storage[NAND_MAX_PATH]{};
    const char* leaf = nullptr;
    s32 parse(const char* path, bool directory = false) {
        if (!path || !*path || strnlen(path, NAND_MAX_PATH) == NAND_MAX_PATH) return NAND_RESULT_INVALID;
        int base = home_fd;
        if (path[0] == '/') {
            const auto prefix = [](const char* value, const char* root) {
                const auto count = std::strlen(root);
                return !std::strncmp(value, root, count) && (!value[count] || value[count] == '/');
            };
            if (prefix(path, home)) { base = home_fd; path += sizeof(home)-1; }
            else if (prefix(path, "/tmp")) { base = tmp_fd; path += 4; }
            else if (prefix(path, "/shared2")) { base = root_fd; ++path; }
            else return NAND_RESULT_ACCESS;
            if (*path == '/') ++path;
        }
        unsigned components = 0;
        for (const char* part = path; *part; ) {
            const char* slash = std::strchr(part, '/');
            const auto length = slash ? static_cast<std::size_t>(slash - part) : std::strlen(part);
            if (!length || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.'))
                return NAND_RESULT_INVALID;
            if (++components > 8) return NAND_RESULT_MAXDEPTH;
            if (!slash) break;
            part = slash + 1;
            if (!*part) return NAND_RESULT_INVALID;
        }
        parent.value = dup(base);
        if (parent.value < 0) return error();
        std::strcpy(storage, path);
        char* component = storage;
        if (!*component) return directory ? NAND_RESULT_OK : NAND_RESULT_INVALID;
        for (unsigned depth = 0; ; ++depth) {
            if (depth >= 8) return NAND_RESULT_MAXDEPTH;
            auto* slash = std::strchr(component, '/');
            if (slash) *slash = '\0';
            if (!*component || !std::strcmp(component, ".") || !std::strcmp(component, "..")) return NAND_RESULT_INVALID;
            if (!slash && !directory) { leaf = component; return NAND_RESULT_OK; }
            const int child = openat(parent.value, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0) return error();
            close(parent.value); parent.value = child;
            if (!slash) return NAND_RESULT_OK;
            component = slash + 1;
        }
    }
};
Handle* handle(NANDFileInfo* info) {
    if (!info) return nullptr;
    for (auto& item : handles) if (item.owner == info && item.token == info->fileDescriptor) return &item;
    return nullptr;
}
bool open_inode(const struct stat& info) {
    for (const auto& item : handles) if (item.owner) {
        struct stat other{};
        if (!fstat(item.fd, &other) && other.st_dev == info.st_dev && other.st_ino == info.st_ino) return true;
    }
    return false;
}
s32 regular(const Path& path, struct stat& info) {
    if (fstatat(path.parent.value, path.leaf, &info, AT_SYMLINK_NOFOLLOW)) return error();
    return S_ISREG(info.st_mode) ? NAND_RESULT_OK : NAND_RESULT_ACCESS;
}
s32 transfer(NANDFileInfo* info, void* bytes, u32 length, bool writing) {
    if (root_fd < 0) return NAND_RESULT_FATAL_ERROR;
    auto* item = handle(info);
    if (!item || (!bytes && length) || length > INT_MAX) return NAND_RESULT_INVALID;
    if (!(item->access & (writing ? NAND_ACCESS_WRITE : NAND_ACCESS_READ))) return NAND_RESULT_ACCESS;
    if (writing) {
        const auto position = lseek(item->fd, 0, SEEK_CUR);
        if (position < 0) return error();
        if (position > file_limit || length > file_limit - position) return NAND_RESULT_MAXBLOCKS;
    }
    u32 done = 0;
    while (done < length) {
        const auto count = writing ? write(item->fd, static_cast<const u8*>(bytes) + done, length - done)
                                   : read(item->fd, static_cast<u8*>(bytes) + done, length - done);
        if (count < 0) { if (errno == EINTR) continue; return done ? static_cast<s32>(done) : error(); }
        if (!count) return writing && !done ? NAND_RESULT_UNKNOWN : static_cast<s32>(done);
        done += static_cast<u32>(count);
    }
    return static_cast<s32>(done);
}
}
namespace starlet::nand {
void start(const char* directory, const Hooks& port_hooks) {
    Lock lock;
    if (root_fd >= 0) throw std::logic_error("NAND already started");
    if (!directory || !*directory || !port_hooks.enter != !port_hooks.leave) throw std::logic_error("invalid NAND owner");
    Fd root(open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (root.value < 0) throw std::runtime_error("cannot open the NAND save directory");
    for (const auto* name : {"data", "tmp"})
        if (mkdirat(root.value, name, 0700) && errno != EEXIST) throw std::runtime_error("cannot create a NAND save subdirectory");
    Fd data(openat(root.value, "data", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    Fd temp(openat(root.value, "tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (data.value < 0 || temp.value < 0) throw std::runtime_error("invalid NAND save subdirectories");
    hooks = port_hooks;
    stopping = false;
    if (pthread_create(&worker, nullptr, run_requests, nullptr)) throw std::runtime_error("cannot start the NAND worker");
    root_fd = root.value; root.value = -1;
    home_fd = data.value; data.value = -1;
    tmp_fd = temp.value; temp.value = -1;
}
void stop() noexcept {
    Section section;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (root_fd < 0) return;
        if (pthread_equal(worker, pthread_self())) std::abort();
        stopping = true;
        changed.notify_all();
    }
    if (pthread_join(worker, nullptr)) std::abort();
    Lock lock;
    for (auto& item : handles) { if (item.fd >= 0) close(item.fd); item = {}; }
    for (auto* fd : {&root_fd, &home_fd, &tmp_fd}) { if (*fd >= 0) close(*fd); *fd = -1; }
}
}
extern "C" {
s32 NANDInit() { Lock lock; return root_fd >= 0 ? NAND_RESULT_OK : NAND_RESULT_FATAL_ERROR; }
s32 NANDGetHomeDir(char* path) {
    Lock lock;
    if (root_fd < 0) return NAND_RESULT_FATAL_ERROR;
    if (!path) return NAND_RESULT_INVALID;
    std::strcpy(path, home); return NAND_RESULT_OK;
}
s32 NANDCreate(const char* path, u8 permission, u8 attribute) {
    Lock lock;
    if (root_fd < 0) return NAND_RESULT_FATAL_ERROR;
    if (permission > 63 || attribute) return NAND_RESULT_INVALID;
    Path resolved; const auto result = resolved.parse(path); if (result) return result;
    Fd file(openat(resolved.parent.value, resolved.leaf, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    return file.value < 0 ? error() : NAND_RESULT_OK;
}
s32 NANDOpen(const char* path, NANDFileInfo* info, u8 access) {
    Lock lock;
    if (root_fd < 0) return NAND_RESULT_FATAL_ERROR;
    if (!info || access < NAND_ACCESS_READ || access > NAND_ACCESS_RW) return NAND_RESULT_INVALID;
    Handle* slot = nullptr;
    for (auto& item : handles) { if (item.owner == info) return NAND_RESULT_OPENFD; if (!item.owner && !slot) slot = &item; }
    if (!slot || next_token == INT_MAX) return NAND_RESULT_MAXFD;
    Path resolved; const auto result = resolved.parse(path); if (result) return result;
    const int mode = access == NAND_ACCESS_READ ? O_RDONLY : access == NAND_ACCESS_WRITE ? O_WRONLY : O_RDWR;
    Fd file(openat(resolved.parent.value, resolved.leaf, mode | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.value < 0) return error();
    struct stat metadata{};
    if (fstat(file.value, &metadata)) return error();
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0 || metadata.st_size > file_limit) return NAND_RESULT_INVALID;
    std::memset(info, 0, sizeof(*info));
    info->fileDescriptor = next_token++; info->origFd = -1; info->accType = access; info->mark = 1;
    *slot = {info, file.value, info->fileDescriptor, access}; file.value = -1;
    return NAND_RESULT_OK;
}
s32 NANDClose(NANDFileInfo* info) {
    Lock lock;
    if (root_fd < 0) return NAND_RESULT_FATAL_ERROR;
    auto* item = handle(info); if (!item) return NAND_RESULT_INVALID;
    s32 result = NAND_RESULT_OK;
    if ((item->access & NAND_ACCESS_WRITE) && fsync(item->fd)) result = error();
    if (close(item->fd) && !result) result = error();
    *item = {}; info->fileDescriptor = -1; info->mark = 0;
    return result;
}
s32 NANDGetLength(NANDFileInfo* info, u32* length) {
    Lock lock;
    if (root_fd < 0) return NAND_RESULT_FATAL_ERROR;
    auto* item = handle(info); if (!item || !length) return NAND_RESULT_INVALID;
    struct stat metadata{}; if (fstat(item->fd, &metadata)) return error();
    if (metadata.st_size < 0 || metadata.st_size > file_limit) return NAND_RESULT_MAXBLOCKS;
    *length = static_cast<u32>(metadata.st_size); return NAND_RESULT_OK;
}
s32 NANDRead(NANDFileInfo* info, void* bytes, u32 length) { Lock lock; return transfer(info, bytes, length, false); }
s32 NANDWrite(NANDFileInfo* info, const void* bytes, u32 length) { Lock lock; return transfer(info, const_cast<void*>(bytes), length, true); }
s32 NANDDelete(const char* path) {
    Lock lock;
    if (root_fd < 0) return NAND_RESULT_FATAL_ERROR;
    Path resolved; auto result = resolved.parse(path); if (result) return result;
    struct stat metadata{}; result = regular(resolved, metadata); if (result) return result;
    if (open_inode(metadata)) return NAND_RESULT_OPENFD;
    return unlinkat(resolved.parent.value, resolved.leaf, 0) ? error() : NAND_RESULT_OK;
}
s32 NANDMove(const char* path, const char* directory) {
    Lock lock;
    if (root_fd < 0) return NAND_RESULT_FATAL_ERROR;
    Path source, destination;
    auto result = source.parse(path); if (result) return result;
    result = destination.parse(directory, true); if (result) return result;
    struct stat metadata{}; result = regular(source, metadata); if (result) return result;
    if (open_inode(metadata)) return NAND_RESULT_OPENFD;
    struct stat target{};
    if (!fstatat(destination.parent.value, source.leaf, &target, AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(target.st_mode)) return NAND_RESULT_ACCESS;
        if (open_inode(target)) return NAND_RESULT_OPENFD;
    } else if (errno != ENOENT) return error();
    if (renameat(source.parent.value, source.leaf, destination.parent.value, source.leaf)) return error();
    if (fsync(destination.parent.value)) return error();
    if (fsync(source.parent.value)) return error();
    return NAND_RESULT_OK;
}
s32 NANDCheck(u32 blocks, u32 inodes, u32* answer) {
    Lock lock;
    if (root_fd < 0) return NAND_RESULT_FATAL_ERROR;
    if (!answer) return NAND_RESULT_INVALID;
    struct statvfs storage{}; if (fstatvfs(root_fd, &storage)) return error();
    *answer = 0;
    if (static_cast<std::uint64_t>(blocks) * 16384 > static_cast<std::uint64_t>(storage.f_bavail) * storage.f_frsize)
        *answer |= NAND_CHECK_HOME_INSSPACE | NAND_CHECK_SYS_INSSPACE;
    if (storage.f_files && inodes > storage.f_favail) *answer |= NAND_CHECK_HOME_INSINODE | NAND_CHECK_SYS_INSINODE;
    return NAND_RESULT_OK;
}
}

namespace {
s32 make_directory(const Request& request) {
    Lock lock;
    if (root_fd < 0) return NAND_RESULT_FATAL_ERROR;
    if (request.permission > 63 || request.attribute) return NAND_RESULT_INVALID;
    Path path;
    const auto result = path.parse(request.path);
    if (result) return result;
    return mkdirat(path.parent.value, path.leaf, 0700) ? error() : NAND_RESULT_OK;
}
s32 execute(const Request& r) {
    switch (r.operation) {
    case Operation::open: return NANDOpen(r.path, r.file, r.access);
    case Operation::close: return NANDClose(r.file);
    case Operation::read: return NANDRead(r.file, r.buffer, r.count);
    case Operation::write: return NANDWrite(r.file, r.buffer, r.count);
    case Operation::seek: return NANDSeek(r.file, r.offset, r.whence);
    case Operation::length: return NANDGetLength(r.file, static_cast<u32*>(r.buffer));
    case Operation::create: return NANDCreate(r.path, r.permission, r.attribute);
    case Operation::remove: return NANDDelete(r.path);
    case Operation::directory: return make_directory(r);
    }
    std::abort();
}
void* run_requests(void*) {
    try {
        for (;;) {
            Request* next = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [] {
                    if (stopping) return true;
                    for (const auto& r : requests) if (r.block) return true;
                    return false;
                });
                for (auto& r : requests) if (r.block && (!next || r.order < next->order)) next = &r;
                if (!next) return nullptr;
            }
            const Request request = *next;
            const auto result = execute(request);
            {
                std::lock_guard<std::mutex> lock(mutex);
                *next = {};
            }
            // Completion owns no caller memory after the callback; it may resubmit its block.
            if (hooks.complete) hooks.complete(request.callback, result, request.block);
            else request.callback(result, request.block);
        }
    } catch (...) {
        std::fputs("NAND: an exception escaped the worker or a callback\n", stderr);
        std::abort();
    }
}
s32 submit(Request request, const char* path = nullptr) {
    Lock lock;
    if (root_fd < 0 || stopping) return NAND_RESULT_FATAL_ERROR;
    if (!request.block || !request.callback) return NAND_RESULT_INVALID;
    if (path) {
        if (!*path || strnlen(path, NAND_MAX_PATH) == NAND_MAX_PATH) return NAND_RESULT_INVALID;
        std::strcpy(request.path, path);
    }
    Request* slot = nullptr;
    for (auto& r : requests) {
        if (r.block == request.block) return NAND_RESULT_BUSY;
        if (!r.block && !slot) slot = &r;
    }
    if (!slot || next_order == UINT64_MAX) return NAND_RESULT_BUSY;
    request.order = ++next_order;
    *slot = request;
    changed.notify_one();
    return NAND_RESULT_OK;
}
Request request(Operation op, NANDCallback callback, NANDCommandBlock* block, NANDFileInfo* file = nullptr) {
    Request r;
    r.operation = op; r.callback = callback; r.block = block; r.file = file;
    return r;
}
}
extern "C" {
s32 NANDSeek(NANDFileInfo* info, s32 offset, s32 whence) {
    Lock lock;
    if (root_fd < 0) return NAND_RESULT_FATAL_ERROR;
    const auto* item = handle(info);
    if (!item || whence < 0 || whence > 2) return NAND_RESULT_INVALID;
    off_t base = 0;
    if (whence == 1) base = lseek(item->fd, 0, SEEK_CUR);
    else if (whence == 2) {
        struct stat metadata{};
        if (fstat(item->fd, &metadata)) return error();
        base = metadata.st_size;
    }
    const auto target = static_cast<std::int64_t>(base) + offset;
    if (base < 0) return error();
    if (target < 0 || target > file_limit) return NAND_RESULT_INVALID;
    const auto result = lseek(item->fd, static_cast<off_t>(target), SEEK_SET);
    return result < 0 ? error() : static_cast<s32>(result);
}
s32 NANDPrivateOpen(const char* path, NANDFileInfo* file, u8 access) { return NANDOpen(path, file, access); }
s32 NANDPrivateCreate(const char* path, u8 permission, u8 attribute) { return NANDCreate(path, permission, attribute); }
s32 NANDPrivateDelete(const char* path) { return NANDDelete(path); }
void NANDSetUserData(NANDCommandBlock* block, void* data) { if (block) block->userData = data; }
void* NANDGetUserData(const NANDCommandBlock* block) { return block ? block->userData : nullptr; }
s32 NANDOpenAsync(const char* path, NANDFileInfo* file, u8 access, NANDCallback cb, NANDCommandBlock* block) {
    auto r = request(Operation::open, cb, block, file); r.access = access; return submit(r, path);
}
s32 NANDPrivateOpenAsync(const char* path, NANDFileInfo* file, u8 access, NANDCallback cb, NANDCommandBlock* block) {
    return NANDOpenAsync(path, file, access, cb, block);
}
s32 NANDPrivateSafeOpenAsync(const char* path, NANDFileInfo* file, u8 access, void* buffer, u32 length, NANDCallback cb, NANDCommandBlock* block) {
    if (!buffer || !length) return NAND_RESULT_INVALID;
    // Write-safe open requires a copy/commit transaction; ordinary write handles cannot substitute for it.
    if (access != NAND_ACCESS_READ) return NAND_RESULT_ACCESS;
    return NANDOpenAsync(path, file, access, cb, block);
}
s32 NANDCloseAsync(NANDFileInfo* file, NANDCallback cb, NANDCommandBlock* block) {
    return submit(request(Operation::close, cb, block, file));
}
s32 NANDSafeCloseAsync(NANDFileInfo* file, NANDCallback cb, NANDCommandBlock* block) { return NANDCloseAsync(file, cb, block); }
s32 NANDReadAsync(NANDFileInfo* file, void* buffer, u32 length, NANDCallback cb, NANDCommandBlock* block) {
    auto r = request(Operation::read, cb, block, file); r.buffer = buffer; r.count = length; return submit(r);
}
s32 NANDWriteAsync(NANDFileInfo* file, const void* buffer, u32 length, NANDCallback cb, NANDCommandBlock* block) {
    auto r = request(Operation::write, cb, block, file); r.buffer = const_cast<void*>(buffer); r.count = length; return submit(r);
}
s32 NANDSeekAsync(NANDFileInfo* file, s32 offset, s32 whence, NANDCallback cb, NANDCommandBlock* block) {
    auto r = request(Operation::seek, cb, block, file); r.offset = offset; r.whence = whence; return submit(r);
}
s32 NANDGetLengthAsync(NANDFileInfo* file, u32* length, NANDCallback cb, NANDCommandBlock* block) {
    auto r = request(Operation::length, cb, block, file); r.buffer = length; return submit(r);
}
s32 NANDPrivateCreateAsync(const char* path, u8 permission, u8 attribute, NANDCallback cb, NANDCommandBlock* block) {
    auto r = request(Operation::create, cb, block); r.permission = permission; r.attribute = attribute; return submit(r, path);
}
s32 NANDPrivateCreateDirAsync(const char* path, u8 permission, u8 attribute, NANDCallback cb, NANDCommandBlock* block) {
    auto r = request(Operation::directory, cb, block); r.permission = permission; r.attribute = attribute; return submit(r, path);
}
s32 NANDPrivateDeleteAsync(const char* path, NANDCallback cb, NANDCommandBlock* block) { return submit(request(Operation::remove, cb, block), path); }
}

extern "C" void NANDInitBanner(NANDBanner* banner, u32 flags, const u16* title, const u16* comment) {
    if (!banner || !title || !comment) throw std::invalid_argument("invalid NAND banner arguments");
    std::memset(banner, 0, sizeof(*banner));
    banner->signature = NAND_BANNER_SIGNATURE;
    banner->flag = flags;
    const u16* text[]{title, comment};
    for (unsigned row = 0; row < 2; ++row) {
        if (!text[row][0]) banner->comment[row][0] = u' ';
        else for (unsigned i = 0; i < NAND_BANNER_COMMENT_SIZE && text[row][i]; ++i)
            banner->comment[row][i] = text[row][i];
    }
}
