#include "starlet/os/crash_report.hpp"
#include <initializer_list>
#include <ucontext.h>
#include <signal.h>
#include <link.h>
#include <execinfo.h>
#include <unwind.h>
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
void (*fault_details)(const void*) = nullptr;

void report_frame(const char* label, std::uintptr_t address) { report_frames(label, &address, 1); }

void report_decimal(unsigned long value) {
    char digits[24];
    int n = 23;
    digits[n] = 0;
    do digits[--n] = char('0' + value % 10);
    while (value /= 10);
    report_text(digits + n);
}

#if defined(__aarch64__) && defined(__linux__)
// The kernel's signal frame: the handler starts with its stack pointer here and receives pointers to
// both members.
struct SignalFrame {
    siginfo_t info;
    ucontext_t context;
};

// The vDSO's __kernel_rt_sigreturn, where a signal handler returns: mov x8, #139; svc #0.
bool is_sigreturn(std::uintptr_t pc) {
    Dl_info module{};
    if (!dladdr(reinterpret_cast<void*>(pc), &module)) return false;
    const auto* code = reinterpret_cast<const std::uint32_t*>(pc);
    return code[0] == 0xd2801168u && code[1] == 0xd4000001u;
}

struct FrameSearch {
    const siginfo_t* own;
    bool past_own;
    const SignalFrame* found;
};

// A frame returning to the sigreturn trampoline is a signal handler, and its CFA is its kernel
// frame. The first is this handler's own (a check of the layout); the next is the handler that
// raised the signal again, interrupted by the original fault.
_Unwind_Reason_Code find_signal_frame(_Unwind_Context* context, void* argument) {
    auto& search = *static_cast<FrameSearch*>(argument);
    if (!is_sigreturn(_Unwind_GetIP(context))) return _URC_NO_REASON;
    const auto* frame = reinterpret_cast<const SignalFrame*>(_Unwind_GetCFA(context));
    if (!search.past_own) {
        if (&frame->info != search.own) return _URC_END_OF_STACK;
        search.past_own = true;
        return _URC_NO_REASON;
    }
    search.found = frame;
    return _URC_END_OF_STACK;
}
#endif

void fatal_signal(int signal, siginfo_t* info, void* context) {
    // One report: a thread that faults while another reports waits for the process to end.
    if (reporting.exchange(true))
        for (;;) pause();
    bool raised_again = false;
    const void* address = info->si_code > 0 ? info->si_addr : nullptr;
#if defined(__aarch64__) && defined(__linux__)
    // A library that catches the signal first (SDL restoring the console) restores this handler and
    // raises the signal again from its own handler, whose kernel frame holds the original fault.
    // The kernel keeps a thread's last fault address in every frame; the siginfo is there only for a
    // handler installed with SA_SIGINFO.
    if (info->si_code <= 0) {
        FrameSearch search{info, false, nullptr};
        _Unwind_Backtrace(find_signal_frame, &search);
        if (search.found) {
            context = const_cast<ucontext_t*>(&search.found->context);
            raised_again = true;
        }
        if (signal == SIGSEGV || signal == SIGBUS)
            address = reinterpret_cast<const void*>(static_cast<ucontext_t*>(context)->uc_mcontext.fault_address);
        if (search.found && search.found->info.si_signo == signal && search.found->info.si_code > 0)
            address = search.found->info.si_addr;
    }
#endif
    const char number[] = {char('0' + signal / 10 % 10), char('0' + signal % 10), 0};
    report_text(program_name);
    report_text(": fatal signal ");
    report_text(number[0] == '0' ? number + 1 : number);
    if (info->si_code <= 0 && !raised_again) {
        report_text(" sent by process ");
        report_decimal(static_cast<unsigned long>(info->si_pid));
        if (address) report_text(", last fault");
    }
    if (address || info->si_code > 0) {
        report_text(" at 0x");
        report_hex(reinterpret_cast<std::uintptr_t>(address));
    }
    report_text(", build ");
    report_text(build_id_text);
    report_text("\n");
    if (raised_again) report_text("  raised again by another signal handler; the original fault:\n");
#if defined(__aarch64__) && defined(__linux__)
    const auto& registers = static_cast<ucontext_t*>(context)->uc_mcontext;
    // brk #0x550b is -fsanitize-trap=return: decompiled code with no return statement.
    if (signal == SIGTRAP && *reinterpret_cast<const std::uint32_t*>(registers.pc) == 0xd42aa160u)
        report_text("  a decompiled function ended without returning its value\n");
    report_frame("  pc ", registers.pc);
    report_frame("  lr ", registers.regs[30]);
    for (int i = 0; i < 32; ++i) {
        report_text(i % 4 ? " " : "  ");
        if (i < 31) {
            report_text("x");
            report_decimal(unsigned(i));
        } else {
            report_text("sp");
        }
        report_text("=0x");
        report_hex(i < 31 ? registers.regs[i] : registers.sp);
        if (i % 4 == 3) report_text("\n");
    }
#endif
    if (fault_details) fault_details(context);
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

void set_fault_details(void (*details)(const void* context)) { fault_details = details; }

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
