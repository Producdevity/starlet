#include "starlet/os/crash_report.hpp"
#include <initializer_list>
#include <ucontext.h>
#include <signal.h>
#include <link.h>
#include <execinfo.h>
#include <cstdio>
#include <csignal>

#include <dlfcn.h>
#include <fcntl.h>
#if defined(__linux__)
#include <elf.h>
#endif
#include <unistd.h>

#include <atomic>
#include <cstring>

namespace starlet::os {
namespace {

constexpr std::size_t max_frames = 64;
constexpr std::size_t name_length = 160;
constexpr std::size_t message_count = 24;
constexpr std::size_t message_length = 160;

char names[max_frames][name_length];
std::uintptr_t starts[max_frames];
char messages[message_count][message_length];
std::atomic<std::size_t> next_message{0};

bool read_at(int fd, void* out, std::size_t size, off_t offset) {
    auto* bytes = static_cast<char*>(out);
    while (size) {
        const ssize_t got = pread(fd, bytes, size, offset);
        if (got <= 0) return false;
        bytes += got;
        size -= std::size_t(got);
        offset += got;
    }
    return true;
}

// Finds the function containing each executable address in /proc/self/exe's .symtab.
void name_functions(const std::uintptr_t* addresses, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        names[i][0] = 0;
        starts[i] = 0;
    }
#if defined(__linux__)
    const int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    Elf64_Ehdr header;
    if (!read_at(fd, &header, sizeof header, 0) || std::memcmp(header.e_ident, ELFMAG, SELFMAG) ||
        header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_shentsize != sizeof(Elf64_Shdr)) {
        close(fd);
        return;
    }
    Elf64_Shdr symtab{}, strtab{};
    bool found = false;
    for (unsigned i = 0; i < header.e_shnum && !found; ++i) {
        Elf64_Shdr section;
        if (!read_at(fd, &section, sizeof section, off_t(header.e_shoff + i * sizeof section))) break;
        if (section.sh_type != SHT_SYMTAB) continue;
        symtab = section;
        found = read_at(fd, &strtab, sizeof strtab, off_t(header.e_shoff + symtab.sh_link * sizeof strtab));
    }
    std::uint32_t best[max_frames] = {};
    if (found) {
        static Elf64_Sym chunk[256];
        const std::size_t total = symtab.sh_size / sizeof(Elf64_Sym);
        for (std::size_t first = 0; first < total; first += 256) {
            const std::size_t n = total - first < 256 ? total - first : 256;
            if (!read_at(fd, chunk, n * sizeof(Elf64_Sym), off_t(symtab.sh_offset + first * sizeof(Elf64_Sym)))) break;
            for (std::size_t s = 0; s < n; ++s) {
                const Elf64_Sym& sym = chunk[s];
                if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC || !sym.st_value || !sym.st_size) continue;
                for (std::size_t i = 0; i < count; ++i) {
                    const std::uintptr_t at = addresses[i];
                    if (at < sym.st_value || at >= sym.st_value + sym.st_size) continue;
                    starts[i] = sym.st_value;
                    best[i] = sym.st_name;
                }
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (!starts[i]) continue;
            if (!read_at(fd, names[i], name_length - 1, off_t(strtab.sh_offset + best[i])))
                names[i][0] = 0;
            names[i][name_length - 1] = 0;
        }
    }
    close(fd);
#else
    (void)addresses;
#endif
}

}

void report_text(const char* text) { (void)!write(STDERR_FILENO, text, std::strlen(text)); }

void report_hex(std::uintptr_t value) {
    char digits[17];
    int n = 16;
    digits[n] = 0;
    do digits[--n] = "0123456789abcdef"[value & 15];
    while (value >>= 4);
    report_text(digits + n);
}

void report_frames(const char* label, const std::uintptr_t* addresses, std::size_t count) {
    if (count > max_frames) count = max_frames;
    name_functions(addresses, count);
    for (std::size_t i = 0; i < count; ++i) {
        report_text(label);
        Dl_info module{};
        if (dladdr(reinterpret_cast<void*>(addresses[i]), &module) && module.dli_fname) {
            const char* file = std::strrchr(module.dli_fname, '/');
            report_text(file ? file + 1 : module.dli_fname);
            report_text("+0x");
            report_hex(addresses[i] - reinterpret_cast<std::uintptr_t>(module.dli_fbase));
            const char* name = names[i][0] ? names[i] : module.dli_sname;
            if (name) {
                report_text(" ");
                report_text(name);
                if (names[i][0] && addresses[i] > starts[i]) {
                    report_text("+0x");
                    report_hex(addresses[i] - starts[i]);
                }
            }
        } else {
            report_text("0x");
            report_hex(addresses[i]);
        }
        report_text("\n");
    }
}

void keep_game_message(const char* text) {
    // Lines end at '\n'; each becomes one slot. Bytes outside printable ASCII (the
    // game's Shift-JIS) become '?'.
    while (*text) {
        char* slot = messages[next_message.fetch_add(1, std::memory_order_relaxed) % message_count];
        std::size_t n = 0;
        for (; *text && *text != '\n'; ++text) {
            const auto c = static_cast<unsigned char>(*text);
            if (n + 1 < message_length) slot[n++] = c >= 0x20 && c < 0x7f ? char(c) : '?';
        }
        slot[n] = 0;
        if (*text == '\n') ++text;
    }
}

void report_game_messages() {
    const std::size_t end = next_message.load(std::memory_order_relaxed);
    if (!end) return;
    report_text("Last game messages:\n");
    for (std::size_t i = end > message_count ? end - message_count : 0; i < end; ++i) {
        const char* line = messages[i % message_count];
        if (!line[0]) continue;
        report_text("  ");
        report_text(line);
        report_text("\n");
    }
}

namespace {

char build_id_text[41] = "unknown";
const char* program_name = "starlet";
std::atomic<bool> reporting{false};

void report_frame(const char* label, std::uintptr_t address) { report_frames(label, &address, 1); }

void fatal_signal(int signal, siginfo_t* info, void* context) {
    // One report: a thread that faults while another reports waits for the process to end.
    if (reporting.exchange(true))
        for (;;) pause();
    const char number[] = {char('0' + signal / 10 % 10), char('0' + signal % 10), 0};
    report_text(program_name);
    report_text(": fatal signal ");
    report_text(number[0] == '0' ? number + 1 : number);
    report_text(" at 0x");
    report_hex(reinterpret_cast<std::uintptr_t>(info->si_addr));
    report_text(", build ");
    report_text(build_id_text);
    report_text("\n");
#if defined(__aarch64__)
    const auto& registers = static_cast<ucontext_t*>(context)->uc_mcontext;
    // brk #0x550b is -fsanitize-trap=return: decompiled code with no return statement.
    if (signal == SIGTRAP && *reinterpret_cast<const std::uint32_t*>(registers.pc) == 0xd42aa160u)
        report_text("  a decompiled function ended without returning its value\n");
    report_frame("  pc ", registers.pc);
    report_frame("  lr ", registers.regs[30]);
#else
    (void)context;
#endif
    void* frames[48];
    const int count = backtrace(frames, 48);
    std::uintptr_t addresses[48];
    for (int i = 0; i < count; ++i) addresses[i] = reinterpret_cast<std::uintptr_t>(frames[i]);
    report_frames("  at ", addresses, std::size_t(count));
    report_game_messages();
    // The default action, so the launcher still sees the signal.
    std::signal(signal, SIG_DFL);
    raise(signal);
}

}

const char* build_id() { return build_id_text; }

void install_fatal_signal_report(const char* program) {
    program_name = program;
    dl_iterate_phdr([](dl_phdr_info* module, std::size_t, void*) {
        for (int i = 0; i < module->dlpi_phnum; ++i) {
            const auto& header = module->dlpi_phdr[i];
            if (header.p_type != PT_NOTE) continue;
            auto* note = reinterpret_cast<const unsigned char*>(module->dlpi_addr + header.p_vaddr);
            const auto* end = note + header.p_memsz;
            while (note + sizeof(ElfW(Nhdr)) <= end) {
                const auto* entry = reinterpret_cast<const ElfW(Nhdr)*>(note);
                const auto* desc = note + sizeof(*entry) + ((entry->n_namesz + 3) & ~3u);
                if (entry->n_type == NT_GNU_BUILD_ID && entry->n_namesz == 4 && !std::memcmp(note + sizeof(*entry), "GNU", 4))
                    for (unsigned j = 0; j < entry->n_descsz && j < 20; ++j)
                        std::snprintf(build_id_text + 2 * j, 3, "%02x", desc[j]);
                note = desc + ((entry->n_descsz + 3) & ~3u);
            }
        }
        return 1;  // The executable comes first.
    }, nullptr);
    void* warm[1];
    backtrace(warm, 1);  // Loads the unwinder now rather than inside the handler.
    static char alternate[65536];
    stack_t stack{};
    stack.ss_sp = alternate;
    stack.ss_size = sizeof(alternate);
    sigaltstack(&stack, nullptr);
    struct sigaction action{};
    action.sa_sigaction = fatal_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGABRT}) sigaction(signal, &action, nullptr);
}

}
