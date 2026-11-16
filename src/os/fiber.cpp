#include "fiber.hpp"

#include "starlet/os/host_allocation.hpp"

#include <cxxabi.h>

#include <cerrno>
#include <cstring>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define STARLET_FIBER_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define STARLET_FIBER_TSAN 1
#endif
#if STARLET_FIBER_TSAN
#include <sanitizer/tsan_interface.h>
#endif

// starlet_fiber_switch(void** save, void* load): pushes the callee-saved registers, stores the
// stack pointer to *save, loads load and pops the registers saved there. starlet_fiber_start is
// where a prepared context's first switch returns: it calls entry(argument) from the registers
// prepare() stored, with no return address, so unwinders and frame-pointer walks end there.
// starlet_fiber_call_entry(function, argument, frame) calls function(argument) from a frame record
// whose address it stores to *frame; its callee's return address is starlet_fiber_entry_return.
#if defined(__aarch64__)
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl starlet_fiber_switch\n"
    ".hidden starlet_fiber_switch\n"
    ".type starlet_fiber_switch, %function\n"
    "starlet_fiber_switch:\n"
    "    .cfi_startproc\n"
    "    sub sp, sp, #160\n"
    "    stp x19, x20, [sp, #0]\n"
    "    stp x21, x22, [sp, #16]\n"
    "    stp x23, x24, [sp, #32]\n"
    "    stp x25, x26, [sp, #48]\n"
    "    stp x27, x28, [sp, #64]\n"
    "    stp x29, x30, [sp, #80]\n"
    "    stp d8, d9, [sp, #96]\n"
    "    stp d10, d11, [sp, #112]\n"
    "    stp d12, d13, [sp, #128]\n"
    "    stp d14, d15, [sp, #144]\n"
    "    mov x9, sp\n"
    "    str x9, [x0]\n"
    "    mov sp, x1\n"
    "    ldp x19, x20, [sp, #0]\n"
    "    ldp x21, x22, [sp, #16]\n"
    "    ldp x23, x24, [sp, #32]\n"
    "    ldp x25, x26, [sp, #48]\n"
    "    ldp x27, x28, [sp, #64]\n"
    "    ldp x29, x30, [sp, #80]\n"
    "    ldp d8, d9, [sp, #96]\n"
    "    ldp d10, d11, [sp, #112]\n"
    "    ldp d12, d13, [sp, #128]\n"
    "    ldp d14, d15, [sp, #144]\n"
    "    add sp, sp, #160\n"
    "    ret\n"
    "    .cfi_endproc\n"
    ".size starlet_fiber_switch, .-starlet_fiber_switch\n"
    ".p2align 4\n"
    ".globl starlet_fiber_start\n"
    ".hidden starlet_fiber_start\n"
    ".type starlet_fiber_start, %function\n"
    "starlet_fiber_start:\n"
    "    .cfi_startproc\n"
    "    .cfi_undefined x30\n"
    "    mov x29, xzr\n"
    "    mov x30, xzr\n"
    "    mov x0, x20\n"
    "    blr x19\n"
    "    brk #1\n"
    "    .cfi_endproc\n"
    ".size starlet_fiber_start, .-starlet_fiber_start\n"
    ".p2align 4\n"
    ".globl starlet_fiber_call_entry\n"
    ".hidden starlet_fiber_call_entry\n"
    ".type starlet_fiber_call_entry, %function\n"
    "starlet_fiber_call_entry:\n"
    "    .cfi_startproc\n"
    "    stp x29, x30, [sp, #-16]!\n"
    "    .cfi_def_cfa_offset 16\n"
    "    .cfi_offset x29, -16\n"
    "    .cfi_offset x30, -8\n"
    "    mov x29, sp\n"
    "    .cfi_def_cfa x29, 16\n"
    "    str x29, [x2]\n"
    "    mov x9, x0\n"
    "    mov x0, x1\n"
    "    blr x9\n"
    ".globl starlet_fiber_entry_return\n"
    ".hidden starlet_fiber_entry_return\n"
    "starlet_fiber_entry_return:\n"
    "    ldp x29, x30, [sp], #16\n"
    "    .cfi_def_cfa sp, 0\n"
    "    .cfi_restore x29\n"
    "    .cfi_restore x30\n"
    "    ret\n"
    "    .cfi_endproc\n"
    ".size starlet_fiber_call_entry, .-starlet_fiber_call_entry\n");
#elif defined(__x86_64__)
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl starlet_fiber_switch\n"
    ".hidden starlet_fiber_switch\n"
    ".type starlet_fiber_switch, @function\n"
    "starlet_fiber_switch:\n"
    "    .cfi_startproc\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"
    "    .cfi_endproc\n"
    ".size starlet_fiber_switch, .-starlet_fiber_switch\n"
    ".p2align 4\n"
    ".globl starlet_fiber_start\n"
    ".hidden starlet_fiber_start\n"
    ".type starlet_fiber_start, @function\n"
    "starlet_fiber_start:\n"
    "    .cfi_startproc\n"
    "    .cfi_undefined rip\n"
    "    xorl %ebp, %ebp\n"
    "    movq %r13, %rdi\n"
    "    callq *%r12\n"
    "    ud2\n"
    "    .cfi_endproc\n"
    ".size starlet_fiber_start, .-starlet_fiber_start\n"
    ".p2align 4\n"
    ".globl starlet_fiber_call_entry\n"
    ".hidden starlet_fiber_call_entry\n"
    ".type starlet_fiber_call_entry, @function\n"
    "starlet_fiber_call_entry:\n"
    "    .cfi_startproc\n"
    "    pushq %rbp\n"
    "    .cfi_def_cfa_offset 16\n"
    "    .cfi_offset %rbp, -16\n"
    "    movq %rsp, %rbp\n"
    "    .cfi_def_cfa_register %rbp\n"
    "    movq %rbp, (%rdx)\n"
    "    movq %rdi, %rax\n"
    "    movq %rsi, %rdi\n"
    "    callq *%rax\n"
    ".globl starlet_fiber_entry_return\n"
    ".hidden starlet_fiber_entry_return\n"
    "starlet_fiber_entry_return:\n"
    "    popq %rbp\n"
    "    .cfi_def_cfa %rsp, 8\n"
    "    .cfi_restore %rbp\n"
    "    ret\n"
    "    .cfi_endproc\n"
    ".size starlet_fiber_call_entry, .-starlet_fiber_call_entry\n");
#else
#error "starlet::os fibers need a context switch for this architecture"
#endif

// Hidden: a reference to a local assembler symbol can resolve through a GOT entry that drops its
// offset in its section (GCC with ld.bfd).
extern "C" __attribute__((visibility("hidden"))) void starlet_fiber_switch(void** save, void* load);
extern "C" __attribute__((visibility("hidden"))) void starlet_fiber_start();
extern "C" __attribute__((visibility("hidden"))) std::uintptr_t starlet_fiber_call_entry(const void* function, void* argument,
                                                                                       void** frame);
extern "C" __attribute__((visibility("hidden"))) void starlet_fiber_entry_return();

namespace starlet::os::fiber {
namespace {

thread_local std::uint32_t sample_id = 0;

void* exception_globals() { return abi::__cxa_get_globals(); }

}

void adopt_current(Context& context) {
#if STARLET_FIBER_TSAN
    context.tsan = __tsan_get_current_fiber();
#endif
    context.guest_allocation = allocating_for_guest();
}

void prepare(Context& context, void* stack, std::size_t size, void (*entry)(void*), void* argument, const char* name) {
    auto top = (reinterpret_cast<std::uintptr_t>(stack) + size) & ~std::uintptr_t{15};
    auto entry_word = reinterpret_cast<std::uintptr_t>(entry);
    auto argument_word = reinterpret_cast<std::uintptr_t>(argument);
    auto start_word = reinterpret_cast<std::uintptr_t>(&starlet_fiber_start);
#if defined(__aarch64__)
    // x19..x28, x29, x30, d8..d15 as starlet_fiber_switch pops them.
    auto* frame = reinterpret_cast<std::uintptr_t*>(top - 160);
    std::memset(frame, 0, 160);
    frame[0] = entry_word;      // x19
    frame[1] = argument_word;   // x20
    frame[11] = start_word;     // x30
#else
    // r15, r14, r13, r12, rbx, rbp, then the return address.
    auto* frame = reinterpret_cast<std::uintptr_t*>(top - 56);
    std::memset(frame, 0, 56);
    frame[2] = argument_word;   // r13
    frame[3] = entry_word;      // r12
    frame[6] = start_word;
#endif
    context.sp = frame;
    context.error = 0;
    context.exceptions[0] = context.exceptions[1] = nullptr;
    context.guest_allocation = true;
#if STARLET_FIBER_TSAN
    context.tsan = __tsan_create_fiber(0);
    __tsan_set_fiber_name(context.tsan, name);
#else
    (void)name;
#endif
}

void release(Context& context) {
#if STARLET_FIBER_TSAN
    if (context.tsan) __tsan_destroy_fiber(context.tsan);
#endif
    context.tsan = nullptr;
    context.sp = nullptr;
}

void switch_to(Context& from, Context& to) {
    from.error = errno;
    errno = to.error;
    void* globals = exception_globals();
    std::memcpy(from.exceptions, globals, sizeof from.exceptions);
    std::memcpy(globals, to.exceptions, sizeof to.exceptions);
    from.guest_allocation = allocating_for_guest();
    set_guest_allocation(to.guest_allocation);
    sample_id = to.sample_id;
#if STARLET_FIBER_TSAN
    __tsan_switch_to_fiber(to.tsan, 0);
#endif
    starlet_fiber_switch(&from.sp, to.sp);
}

std::uint32_t current_sample_id() { return sample_id; }

std::uintptr_t call_entry(const void* function, void* argument, void** frame) {
    return starlet_fiber_call_entry(function, argument, frame);
}

std::uintptr_t entry_return_address() { return reinterpret_cast<std::uintptr_t>(&starlet_fiber_entry_return); }

}
