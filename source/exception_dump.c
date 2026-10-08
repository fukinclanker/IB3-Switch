/* Minimal libnx user-exception handler for the IB3 runtime.
 *
 * Two jobs:
 *  1. Service permission faults on executable guest mappings (the runtime's
 *     code cache is mapped RWX; Horizon only allows RW or RX, so
 *     ib3_handle_exec_fault() flips the mapping and the access is retried),
 *     sparse VA commits, trapped counter reads and synthesised signals.
 *  2. Log anything else and panic, using only the already-open debug log.
 *
 * Resuming: svcReturnFromException is only legal while the kernel is still
 * "in" the exception (inside the entry point below). After that the handler
 * resumes by itself: it rebuilds every general register from the saved
 * exception dump and branches back through a register that already holds the
 * target (or x17 for a retry of the same pc). A faulting store is
 * re-executed from a small stub (ib3_store_stub) placed within branch range of
 * the store.
 *
 * Per-thread state (the important part)
 * -------------------------------------
 * libnx's own __libnx_exception_entry writes every fault into ONE global
 * ThreadExceptionDump and runs every handler on ONE shared exception stack.
 * That is fine for code that returns through svcReturnFromException at once,
 * but this handler resumes manually, takes locks, maps memory and writes the
 * log, so a second thread faulting meanwhile overwrote the first thread's
 * registers and stack frames: random crashes and corrupted state. The SIMD
 * save area was also picked from only 4 bits of the TLS address, so threads
 * shared save slots and resumed with another thread's vector registers
 * (corrupted vector maths, i.e. graphical glitches and crashes).
 *
 * This file therefore overrides the (weak) __libnx_exception_entry. Every
 * thread gets its own slot, looked up from its TLS address (tpidrro_el0):
 *   +0x0000  SIMD save area, depth 0      +0x0400  SIMD save area, depth 1
 *   +0x0800  ThreadExceptionDump, depth 0  +0x0C00  ThreadExceptionDump, depth 1
 *   +0x1000 .. +0x6000   handler stack for a fault raised inside the handler
 *   +0x6000 .. +0x10000  handler stack for a fault raised by guest code
 * Depth 1 exists because the guest's own signal handlers run on our stack and
 * may fault themselves (sparse commit, exec flip).
 *
 * SIMD/FP state: the C code that runs here freely uses v0-v31, so
 * __libnx_exception_handler saves v0-v31/fpcr/fpsr into the slot before any C
 * runs and every resume path restores them just before returning.
 *
 * Red zone: the guest is iOS arm64 code, whose ABI lets leaf functions keep
 * data in the 128 bytes below sp. The resume frame is therefore placed below
 * that red zone (sp - 0x1a0) instead of directly under sp. */

#include <stddef.h>
#include <stdint.h>

#include <switch.h>

#include "util.h"

#define IB3_EXC_SLOTS 64
#define IB3_EXC_SLOT_SIZE 0x10000
#define IB3_EXC_SIMD1_OFF 0x400
#define IB3_EXC_DUMP0_OFF 0x800
#define IB3_EXC_DUMP1_OFF 0xC00

_Static_assert(sizeof(ThreadExceptionDump) <= 0x400, "dump slot too small");
_Static_assert(offsetof(ThreadExceptionDump, fpu_gprs) == 0x120, "dump layout");
_Static_assert(offsetof(ThreadExceptionDump, far) == 0x330, "dump layout");

/* Kept for libnx compatibility (its own entry is overridden below). */
u8 __nx_exception_stack[0x1000] __attribute__((aligned(16)));
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

/* Referenced from the assembly below, hence not static. */
uint64_t ib3_exc_keys[IB3_EXC_SLOTS] __attribute__((aligned(64)));
uint8_t ib3_exc_slots[IB3_EXC_SLOTS * IB3_EXC_SLOT_SIZE]
    __attribute__((aligned(4096)));

/* ib3_shim.c: services RW<->RX toggles and sparse VA commits.
 * Returns 0 not handled, 1 exec flip, 2 sparse commit (retry same pc). */
int ib3_handle_exec_fault(int is_instruction_fetch, uint64_t address);
/* ib3_shim.c: stub that re-executes the store at pc and continues at pc+4. */
uint64_t ib3_store_stub(uint64_t pc);
/* ib3_shim.c: rewrite `mrs xN, cntvct_el0` in a code-cache pool to cntpct_el0. */
int ib3_patch_counter_read(uint64_t pc);
/* ib3_shim.c: synthesise POSIX signal to a registered handler.
 * regs is x0..x30, sp, pc, pstate and receives the handler's edits. */
int ib3_deliver_signal(int signum, uint64_t fault_addr, uint64_t pc,
                       uint64_t *regs, uint64_t *out_pc);

void ib3_resume_0(const uint64_t *frame);
void ib3_resume_1(const uint64_t *frame);
void ib3_resume_2(const uint64_t *frame);
void ib3_resume_3(const uint64_t *frame);
void ib3_resume_4(const uint64_t *frame);
void ib3_resume_5(const uint64_t *frame);
void ib3_resume_6(const uint64_t *frame);
void ib3_resume_7(const uint64_t *frame);
void ib3_resume_8(const uint64_t *frame);
void ib3_resume_9(const uint64_t *frame);
void ib3_resume_10(const uint64_t *frame);
void ib3_resume_11(const uint64_t *frame);
void ib3_resume_12(const uint64_t *frame);
void ib3_resume_13(const uint64_t *frame);
void ib3_resume_14(const uint64_t *frame);
void ib3_resume_15(const uint64_t *frame);
void ib3_resume_16(const uint64_t *frame);
void ib3_resume_17(const uint64_t *frame);
void ib3_resume_18(const uint64_t *frame);
void ib3_resume_19(const uint64_t *frame);
void ib3_resume_20(const uint64_t *frame);
void ib3_resume_21(const uint64_t *frame);
void ib3_resume_22(const uint64_t *frame);
void ib3_resume_23(const uint64_t *frame);
void ib3_resume_24(const uint64_t *frame);
void ib3_resume_25(const uint64_t *frame);
void ib3_resume_26(const uint64_t *frame);
void ib3_resume_27(const uint64_t *frame);
void ib3_resume_28(const uint64_t *frame);
void ib3_resume_29(const uint64_t *frame);
void ib3_resume_30(const uint64_t *frame);

/* Find (or claim) this thread's slot. Key = tpidrro_el0 (unique per live
 * thread). Result: slot base in T. Uses registers K,T,I,A,V and W (a w-reg);
 * touches no memory except the key table and no SIMD registers. If the table
 * is full, falls back to a hashed slot. */
#define IB3_SLOT_LOOKUP(K, T, I, A, V, W) \
"  mrs " K ", tpidrro_el0\n" \
"  adrp " T ", ib3_exc_keys\n" \
"  add " T ", " T ", :lo12:ib3_exc_keys\n" \
"  mov " I ", #0\n" \
"91:\n" \
"  cmp " I ", #64\n" \
"  b.hs 94f\n" \
"  add " A ", " T ", " I ", lsl #3\n" \
"  ldar " V ", [" A "]\n" \
"  cmp " V ", " K "\n" \
"  b.eq 93f\n" \
"  cbnz " V ", 92f\n" \
"95:\n" \
"  ldaxr " V ", [" A "]\n" \
"  cbnz " V ", 96f\n" \
"  stlxr " W ", " K ", [" A "]\n" \
"  cbnz " W ", 95b\n" \
"  b 93f\n" \
"96:\n" \
"  clrex\n" \
"  cmp " V ", " K "\n" \
"  b.eq 93f\n" \
"92:\n" \
"  add " I ", " I ", #1\n" \
"  b 91b\n" \
"94:\n" \
"  ubfx " I ", " K ", #9, #6\n" \
"93:\n" \
"  adrp " T ", ib3_exc_slots\n" \
"  add " T ", " T ", :lo12:ib3_exc_slots\n" \
"  add " T ", " T ", " I ", lsl #16\n"

/* Save v0-v31/fpcr/fpsr to the area at x9. Clobbers x10-x11. */
#define IB3_SIMD_STORE_X9 \
"  stp q0, q1, [x9, #0]\n" \
"  stp q2, q3, [x9, #32]\n" \
"  stp q4, q5, [x9, #64]\n" \
"  stp q6, q7, [x9, #96]\n" \
"  stp q8, q9, [x9, #128]\n" \
"  stp q10, q11, [x9, #160]\n" \
"  stp q12, q13, [x9, #192]\n" \
"  stp q14, q15, [x9, #224]\n" \
"  stp q16, q17, [x9, #256]\n" \
"  stp q18, q19, [x9, #288]\n" \
"  stp q20, q21, [x9, #320]\n" \
"  stp q22, q23, [x9, #352]\n" \
"  stp q24, q25, [x9, #384]\n" \
"  stp q26, q27, [x9, #416]\n" \
"  stp q28, q29, [x9, #448]\n" \
"  stp q30, q31, [x9, #480]\n" \
"  mrs x10, fpcr\n" \
"  mrs x11, fpsr\n" \
"  str x10, [x9, #512]\n" \
"  str x11, [x9, #520]\n"

/* The handler is entered with x0 = this thread's dump (slot + 0x800 at depth
 * 0, slot + 0xC00 at depth 1), so the SIMD area is simply x0 - 0x800. */
#define IB3_SIMD_SAVE \
"  sub x9, x0, #0x800\n" \
IB3_SIMD_STORE_X9

/* Resume side: x0 = resume frame (frame[31] = target sp). Returning into
 * handler code (target sp inside our slot) restores the depth-1 area,
 * returning into the guest restores the depth-0 area. Clobbers x9-x14. */
#define IB3_SIMD_RESTORE \
IB3_SLOT_LOOKUP("x9", "x10", "x11", "x12", "x13", "w14") \
"  ldr x11, [x0, #0xf8]\n" \
"  sub x12, x11, x10\n" \
"  cmp x12, #0x10, lsl #12\n" \
"  b.hs 97f\n" \
"  add x10, x10, #0x400\n" \
"97:\n" \
"  mov x9, x10\n" \
"  ldp q0, q1, [x9, #0]\n" \
"  ldp q2, q3, [x9, #32]\n" \
"  ldp q4, q5, [x9, #64]\n" \
"  ldp q6, q7, [x9, #96]\n" \
"  ldp q8, q9, [x9, #128]\n" \
"  ldp q10, q11, [x9, #160]\n" \
"  ldp q12, q13, [x9, #192]\n" \
"  ldp q14, q15, [x9, #224]\n" \
"  ldp q16, q17, [x9, #256]\n" \
"  ldp q18, q19, [x9, #288]\n" \
"  ldp q20, q21, [x9, #320]\n" \
"  ldp q22, q23, [x9, #352]\n" \
"  ldp q24, q25, [x9, #384]\n" \
"  ldp q26, q27, [x9, #416]\n" \
"  ldp q28, q29, [x9, #448]\n" \
"  ldp q30, q31, [x9, #480]\n" \
"  ldr x10, [x9, #512]\n" \
"  ldr x11, [x9, #520]\n" \
"  msr fpcr, x10\n" \
"  msr fpsr, x11\n"

/* The real handler (below). */
void ib3_exception_handler_c(ThreadExceptionDump *context);

/* Exception entry, replacing libnx's weak __libnx_exception_entry.
 * crt0 jumps here with x0 = exception type, x1 = kernel exception context
 * (x0..x8, lr, sp, elr, pstate, afsr0, afsr1, esr, far). x9..x29 still hold
 * the faulting thread's values. While we are here the kernel serialises
 * exceptions, so this part must be short and must not touch the faulting
 * thread's stack: it fills this thread's own dump, points the saved sp at this
 * thread's own handler stack and elr at ib3_exception_returnentry, and
 * returns through svcReturnFromException. */
__asm__(
".pushsection .text.__libnx_exception_entry,\"ax\",%progbits\n"
".balign 4\n"
".global __libnx_exception_entry\n"
".type __libnx_exception_entry, %function\n"
"__libnx_exception_entry:\n"
"  cbz x1, 99f\n"
IB3_SLOT_LOOKUP("x2", "x3", "x4", "x5", "x6", "w7")
/* x3 = slot. x4 = faulting sp. Nested if it lies inside this slot's stacks. */
"  ldr x4, [x1, #0x50]\n"
"  add x5, x3, #0x1000\n"
"  add x6, x3, #0x10, lsl #12\n"
"  cmp x4, x5\n"
"  b.lo 81f\n"
"  cmp x4, x6\n"
"  b.hs 81f\n"
/* depth 1: dump at +0xC00, stack at min(+0x6000, (sp - 0x100) & ~15) */
"  add x2, x3, #0xC00\n"
"  add x5, x3, #0x6, lsl #12\n"
"  sub x6, x4, #0x100\n"
"  and x6, x6, #0xfffffffffffffff0\n"
"  cmp x6, x5\n"
"  csel x6, x6, x5, lo\n"
"  b 82f\n"
"81:\n"
/* depth 0: dump at +0x800, stack at slot top */
"  add x2, x3, #0x800\n"
"  add x6, x3, #0x10, lsl #12\n"
"82:\n"
"  mov x5, x2\n"
"  str w0, [x2], #4\n"
"  str wzr, [x2], #4\n"
"  str wzr, [x2], #4\n"
"  str wzr, [x2], #4\n"
/* x0..x8 from the kernel context; the saved x0 becomes the dump pointer */
"  ldp x3, x4, [x1]\n"
"  str x5, [x1], #16\n"
"  stp x3, x4, [x2], #16\n"
"  ldp x3, x4, [x1], #16\n"
"  stp x3, x4, [x2], #16\n"
"  ldp x3, x4, [x1], #16\n"
"  stp x3, x4, [x2], #16\n"
"  ldp x3, x4, [x1], #16\n"
"  stp x3, x4, [x2], #16\n"
"  ldr x3, [x1], #8\n"
"  str x3, [x2], #8\n"
/* x9..x28 are live */
"  str x9, [x2], #8\n"
"  stp x10, x11, [x2], #16\n"
"  stp x12, x13, [x2], #16\n"
"  stp x14, x15, [x2], #16\n"
"  stp x16, x17, [x2], #16\n"
"  stp x18, x19, [x2], #16\n"
"  stp x20, x21, [x2], #16\n"
"  stp x22, x23, [x2], #16\n"
"  stp x24, x25, [x2], #16\n"
"  stp x26, x27, [x2], #16\n"
"  str x28, [x2], #8\n"
"  str x29, [x2], #8\n"
/* lr */
"  ldr x3, [x1], #8\n"
"  str x3, [x2], #8\n"
/* sp: report the faulting sp, resume on this thread's handler stack */
"  ldr x3, [x1]\n"
"  str x6, [x1], #8\n"
"  str x3, [x2], #8\n"
/* pc: report the faulting pc, resume at ib3_exception_returnentry */
"  adrp x4, ib3_exception_returnentry\n"
"  add x4, x4, :lo12:ib3_exception_returnentry\n"
"  ldr x3, [x1]\n"
"  str x4, [x1], #8\n"
"  str x3, [x2], #8\n"
"  str xzr, [x2], #8\n"
"  stp q0, q1, [x2], #32\n"
"  stp q2, q3, [x2], #32\n"
"  stp q4, q5, [x2], #32\n"
"  stp q6, q7, [x2], #32\n"
"  stp q8, q9, [x2], #32\n"
"  stp q10, q11, [x2], #32\n"
"  stp q12, q13, [x2], #32\n"
"  stp q14, q15, [x2], #32\n"
"  stp q16, q17, [x2], #32\n"
"  stp q18, q19, [x2], #32\n"
"  stp q20, q21, [x2], #32\n"
"  stp q22, q23, [x2], #32\n"
"  stp q24, q25, [x2], #32\n"
"  stp q26, q27, [x2], #32\n"
"  stp q28, q29, [x2], #32\n"
"  stp q30, q31, [x2], #32\n"
/* pstate, afsr0, afsr1, esr, far */
"  ldr w3, [x1], #4\n"
"  str w3, [x2], #4\n"
"  ldr w3, [x1], #4\n"
"  str w3, [x2], #4\n"
"  ldr w3, [x1], #4\n"
"  str w3, [x2], #4\n"
"  ldr w3, [x1], #4\n"
"  str w3, [x2], #4\n"
"  ldr x3, [x1], #8\n"
"  str x3, [x2], #8\n"
"  mov w0, wzr\n"
"  bl svcReturnFromException\n"
"  b .\n"
"99:\n"
"  mov w0, #0xf801\n"
"  bl svcReturnFromException\n"
"  b .\n"
".popsection\n"
);

/* Reached through svcReturnFromException with x0 = this thread's dump and sp
 * = this thread's handler stack. */
__asm__(
".pushsection .text,\"ax\",%progbits\n"
".balign 4\n"
".global ib3_exception_returnentry\n"
".type ib3_exception_returnentry, %function\n"
"ib3_exception_returnentry:\n"
"  bl __libnx_exception_handler\n"
"  mov w0, wzr\n"
"  mov x1, #0\n"
"  mov x2, #0\n"
"  bl svcBreak\n"
"  b .\n"
".global __libnx_exception_handler\n"
".type __libnx_exception_handler, %function\n"
"__libnx_exception_handler:\n"
IB3_SIMD_SAVE
"  b ib3_exception_handler_c\n"
".popsection\n"
);

__asm__(
".pushsection .text,\"ax\",%progbits\n"
".balign 4\n"
".global ib3_resume_0\n"
".type ib3_resume_0, %function\n"
"ib3_resume_0:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"10:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 10b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x0, [sp, #-136]\n"
"  br x0\n"
".global ib3_resume_1\n"
".type ib3_resume_1, %function\n"
"ib3_resume_1:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"11:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 11b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x1, [sp, #-136]\n"
"  br x1\n"
".global ib3_resume_2\n"
".type ib3_resume_2, %function\n"
"ib3_resume_2:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"12:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 12b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x2, [sp, #-136]\n"
"  br x2\n"
".global ib3_resume_3\n"
".type ib3_resume_3, %function\n"
"ib3_resume_3:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"13:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 13b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x3, [sp, #-136]\n"
"  br x3\n"
".global ib3_resume_4\n"
".type ib3_resume_4, %function\n"
"ib3_resume_4:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"14:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 14b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x4, [sp, #-136]\n"
"  br x4\n"
".global ib3_resume_5\n"
".type ib3_resume_5, %function\n"
"ib3_resume_5:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"15:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 15b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x5, [sp, #-136]\n"
"  br x5\n"
".global ib3_resume_6\n"
".type ib3_resume_6, %function\n"
"ib3_resume_6:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"16:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 16b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x6, [sp, #-136]\n"
"  br x6\n"
".global ib3_resume_7\n"
".type ib3_resume_7, %function\n"
"ib3_resume_7:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"17:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 17b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x7, [sp, #-136]\n"
"  br x7\n"
".global ib3_resume_8\n"
".type ib3_resume_8, %function\n"
"ib3_resume_8:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"18:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 18b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x8, [sp, #-136]\n"
"  br x8\n"
".global ib3_resume_9\n"
".type ib3_resume_9, %function\n"
"ib3_resume_9:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"19:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 19b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x9, [sp, #-136]\n"
"  br x9\n"
".global ib3_resume_10\n"
".type ib3_resume_10, %function\n"
"ib3_resume_10:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"110:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 110b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x10, [sp, #-136]\n"
"  br x10\n"
".global ib3_resume_11\n"
".type ib3_resume_11, %function\n"
"ib3_resume_11:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"111:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 111b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x11, [sp, #-136]\n"
"  br x11\n"
".global ib3_resume_12\n"
".type ib3_resume_12, %function\n"
"ib3_resume_12:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"112:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 112b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x12, [sp, #-136]\n"
"  br x12\n"
".global ib3_resume_13\n"
".type ib3_resume_13, %function\n"
"ib3_resume_13:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"113:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 113b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x13, [sp, #-136]\n"
"  br x13\n"
".global ib3_resume_14\n"
".type ib3_resume_14, %function\n"
"ib3_resume_14:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"114:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 114b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x14, [sp, #-136]\n"
"  br x14\n"
".global ib3_resume_15\n"
".type ib3_resume_15, %function\n"
"ib3_resume_15:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"115:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 115b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x15, [sp, #-136]\n"
"  br x15\n"
".global ib3_resume_16\n"
".type ib3_resume_16, %function\n"
"ib3_resume_16:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"116:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 116b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x16, [sp, #-136]\n"
"  br x16\n"
".global ib3_resume_17\n"
".type ib3_resume_17, %function\n"
"ib3_resume_17:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"117:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 117b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x16, [sp, #0xf8]\n"
"  mov sp, x16\n"
"  ldur x16, [sp, #-144]\n"
"  ldur x17, [sp, #-136]\n"
"  br x17\n"
".global ib3_resume_18\n"
".type ib3_resume_18, %function\n"
"ib3_resume_18:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"118:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 118b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x18, [sp, #-136]\n"
"  br x18\n"
".global ib3_resume_19\n"
".type ib3_resume_19, %function\n"
"ib3_resume_19:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"119:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 119b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x19, [sp, #-136]\n"
"  br x19\n"
".global ib3_resume_20\n"
".type ib3_resume_20, %function\n"
"ib3_resume_20:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"120:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 120b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x20, [sp, #-136]\n"
"  br x20\n"
".global ib3_resume_21\n"
".type ib3_resume_21, %function\n"
"ib3_resume_21:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"121:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 121b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x21, [sp, #-136]\n"
"  br x21\n"
".global ib3_resume_22\n"
".type ib3_resume_22, %function\n"
"ib3_resume_22:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"122:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 122b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x22, [sp, #-136]\n"
"  br x22\n"
".global ib3_resume_23\n"
".type ib3_resume_23, %function\n"
"ib3_resume_23:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"123:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 123b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x23, [sp, #-136]\n"
"  br x23\n"
".global ib3_resume_24\n"
".type ib3_resume_24, %function\n"
"ib3_resume_24:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"124:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 124b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x24, [sp, #-136]\n"
"  br x24\n"
".global ib3_resume_25\n"
".type ib3_resume_25, %function\n"
"ib3_resume_25:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"125:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 125b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x25, [sp, #-136]\n"
"  br x25\n"
".global ib3_resume_26\n"
".type ib3_resume_26, %function\n"
"ib3_resume_26:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"126:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 126b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x26, [sp, #-136]\n"
"  br x26\n"
".global ib3_resume_27\n"
".type ib3_resume_27, %function\n"
"ib3_resume_27:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"127:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 127b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x27, [sp, #-136]\n"
"  br x27\n"
".global ib3_resume_28\n"
".type ib3_resume_28, %function\n"
"ib3_resume_28:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"128:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 128b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x28, [sp, #-136]\n"
"  br x28\n"
".global ib3_resume_29\n"
".type ib3_resume_29, %function\n"
"ib3_resume_29:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"129:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 129b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x30, [sp, #240]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x29, [sp, #-136]\n"
"  br x29\n"
".global ib3_resume_30\n"
".type ib3_resume_30, %function\n"
"ib3_resume_30:\n"
IB3_SIMD_RESTORE
"  ldr x9, [x0, #0xf8]\n"
"  sub x9, x9, #0x1a0\n"
"  mov x10, x0\n"
"  mov x11, x9\n"
"  mov x12, #18\n"
"130:\n"
"  ldp x13, x14, [x10], #16\n"
"  stp x13, x14, [x11], #16\n"
"  subs x12, x12, #1\n"
"  b.ne 130b\n"
"  mov sp, x9\n"
"  ldr x10, [sp, #0x100]\n"
"  msr nzcv, x10\n"
"  ldr x0, [sp, #0]\n"
"  ldr x1, [sp, #8]\n"
"  ldr x2, [sp, #16]\n"
"  ldr x3, [sp, #24]\n"
"  ldr x4, [sp, #32]\n"
"  ldr x5, [sp, #40]\n"
"  ldr x6, [sp, #48]\n"
"  ldr x7, [sp, #56]\n"
"  ldr x8, [sp, #64]\n"
"  ldr x9, [sp, #72]\n"
"  ldr x10, [sp, #80]\n"
"  ldr x11, [sp, #88]\n"
"  ldr x12, [sp, #96]\n"
"  ldr x13, [sp, #104]\n"
"  ldr x14, [sp, #112]\n"
"  ldr x15, [sp, #120]\n"
"  ldr x16, [sp, #128]\n"
"  ldr x18, [sp, #144]\n"
"  ldr x19, [sp, #152]\n"
"  ldr x20, [sp, #160]\n"
"  ldr x21, [sp, #168]\n"
"  ldr x22, [sp, #176]\n"
"  ldr x23, [sp, #184]\n"
"  ldr x24, [sp, #192]\n"
"  ldr x25, [sp, #200]\n"
"  ldr x26, [sp, #208]\n"
"  ldr x27, [sp, #216]\n"
"  ldr x28, [sp, #224]\n"
"  ldr x29, [sp, #232]\n"
"  ldr x17, [sp, #0xf8]\n"
"  mov sp, x17\n"
"  ldur x17, [sp, #-144]\n"
"  ldur x30, [sp, #-136]\n"
"  br x30\n"
".popsection\n"
);

typedef void (*ResumeFn)(const uint64_t *frame);
static const ResumeFn g_resume_fns[31] = {
  ib3_resume_0,
  ib3_resume_1,
  ib3_resume_2,
  ib3_resume_3,
  ib3_resume_4,
  ib3_resume_5,
  ib3_resume_6,
  ib3_resume_7,
  ib3_resume_8,
  ib3_resume_9,
  ib3_resume_10,
  ib3_resume_11,
  ib3_resume_12,
  ib3_resume_13,
  ib3_resume_14,
  ib3_resume_15,
  ib3_resume_16,
  ib3_resume_17,
  ib3_resume_18,
  ib3_resume_19,
  ib3_resume_20,
  ib3_resume_21,
  ib3_resume_22,
  ib3_resume_23,
  ib3_resume_24,
  ib3_resume_25,
  ib3_resume_26,
  ib3_resume_27,
  ib3_resume_28,
  ib3_resume_29,
  ib3_resume_30
};

/* Hot-path log lines (every resume) are rate limited: each one used to be a
 * synchronous SD-card write inside the fault path, which stalled the game,
 * starved audio and widened every race window. */
static int log_budget(volatile unsigned *counter, unsigned limit) {
  return __atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED) < limit;
}
static volatile unsigned g_resume_logs;

static char *append_text(char *output, const char *text) {
  while (*text)
    *output++ = *text++;
  return output;
}

static char *append_hex(char *output, uint64_t value, int digits) {
  static const char hex[] = "0123456789abcdef";
  for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
    *output++ = hex[(value >> shift) & 0xf];
  return output;
}

static void log_line(const char *a, uint64_t va, const char *b, uint64_t vb) {
  char line[160];
  char *c = append_text(line, a);
  c = append_hex(c, va, 16);
  if (b) {
    c = append_text(c, b);
    c = append_hex(c, vb, 16);
  }
  *c++ = '\n';
  debugEmergencyWrite(line, (size_t)(c - line));
}

static int context_is_readable(const ThreadExceptionDump *context) {
  if (!context)
    return 0;
  MemoryInfo info;
  u32 page_info;
  if (R_FAILED(svcQueryMemory(&info, &page_info, (uint64_t)context)))
    return 0;
  const uintptr_t start = (uintptr_t)context;
  const uintptr_t end = start + sizeof(*context);
  return info.type != MemType_Unmapped && (info.perm & Perm_R) &&
         end >= start && end <= (uintptr_t)info.addr + info.size;
}

static uint64_t gpr(const ThreadExceptionDump *d, int i) {
  if (i < 29)
    return d->cpu_gprs[i].x;
  return i == 29 ? d->fp.x : d->lr.x;
}

static char *append_dec(char *o, unsigned v) {
  if (v >= 10)
    *o++ = (char)('0' + v / 10);
  *o++ = (char)('0' + v % 10);
  return o;
}

static int range_is_readable(uint64_t addr, size_t len) {
  MemoryInfo info;
  u32 page_info;
  if (R_FAILED(svcQueryMemory(&info, &page_info, addr)))
    return 0;
  return info.type != MemType_Unmapped && (info.perm & Perm_R) &&
         addr + len >= addr && addr + len <= (uint64_t)info.addr + info.size;
}

/* 32-bit words around `addr`, `|` marks addr itself. Only reads mapped,
 * readable memory. */
static void dump_words(const char *tag, uint64_t addr, int before, int after) {
  const uint64_t start = addr - (uint64_t)before * 4;
  const size_t bytes = (size_t)(before + after) * 4;
  char line[256];
  char *c = append_text(line, tag);
  c = append_hex(c, addr, 16);
  c = append_text(c, ":");
  if (!range_is_readable(start, bytes)) {
    c = append_text(c, " unreadable");
  } else {
    for (int i = 0; i < before + after; i++) {
      *c++ = ' ';
      if (i == before)
        *c++ = '|';
      c = append_hex(c, *(const volatile uint32_t *)(uintptr_t)(start + 4u * (unsigned)i), 8);
    }
  }
  *c++ = '\n';
  debugEmergencyWrite(line, (size_t)(c - line));
}

static void dump_regs(const ThreadExceptionDump *d) {
  for (int base = 0; base < 31; base += 3) {
    char line[160];
    char *c = append_text(line, "regs");
    for (int i = base; i < base + 3 && i < 31; i++) {
      c = append_text(c, " x");
      c = append_dec(c, (unsigned)i);
      c = append_text(c, "=");
      c = append_hex(c, gpr(d, i), 16);
    }
    *c++ = '\n';
    debugEmergencyWrite(line, (size_t)(c - line));
  }
}


/* Frame layout (dwords) shared with the assembly: 0..30 x0..x30, 31 sp,
 * 32 nzcv, 33 saved x17 (lands at [sp-152]), 34 saved xT (lands at
 * [sp-144]), 35 branch target (lands at [sp-136]). The frame is written at
 * sp-0x1a0, below the guest's 128-byte red zone. */
/* Retry the faulting instruction at the SAME pc (sparse VA commit: pages
 * were unmapped so the access never completed). Uses resume_17: restores
 * every GPR except x17, which is loaded with `pc` and used to branch.
 * x17 is IP1 (linker scratch) and is almost always free across a fault. */
static void manual_resume_retry_same_pc(const ThreadExceptionDump *d) {
  const uint64_t pc = d->pc.x;
  const uint64_t sp = d->sp.x;
  if (sp & 0xf) {
    log_line("manual-resume: guest sp misaligned: 0x", sp, 0, 0);
    return;
  }
  uint64_t frame[36];
  for (int i = 0; i < 31; i++)
    frame[i] = gpr(d, i);
  frame[31] = sp;
  frame[32] = (uint64_t)(d->pstate & 0xf0000000u);
  frame[33] = 0;
  frame[34] = gpr(d, 16); /* resume_17 restores x16 from here */
  frame[35] = pc;         /* resume_17 loads x17=pc and br x17 */
  if (log_budget(&g_resume_logs, 32))
    log_line("manual-resume: retry-same-pc=0x", pc, 0, 0);
  g_resume_fns[17](frame);
  __builtin_unreachable();
}

static void manual_resume_store(const ThreadExceptionDump *d) {
  const uint64_t pc = d->pc.x;
  const uint64_t sp = d->sp.x;
  /* Loads and stores alike: re-running the instruction from a stub restores
   * every register, including x17, which the plain retry path has to
   * clobber. (Data-abort instructions are register-addressed, so they are
   * position independent.) */
  if (sp & 0xf) {
    log_line("manual-resume: guest sp misaligned: 0x", sp, 0, 0);
    return;
  }
  const uint64_t stub = ib3_store_stub(pc);
  if (!stub) {
    /* Store outside libib3.so (e.g. memset into guest VA after sparse commit).
     * Pages are now mapped RW — retry the same instruction. x17 clobbered. */
    if (log_budget(&g_resume_logs, 32))
      log_line("manual-resume: store-retry-same-pc at 0x", pc, " instr=0x",
               *(const volatile uint32_t *)(uintptr_t)pc);
    manual_resume_retry_same_pc(d);
    return;
  }
  uint64_t frame[36];
  for (int i = 0; i < 31; i++)
    frame[i] = gpr(d, i);
  frame[31] = sp;
  frame[32] = (uint64_t)(d->pstate & 0xf0000000u);
  frame[33] = gpr(d, 17);
  frame[34] = gpr(d, 16);
  frame[35] = stub;
  g_resume_fns[17](frame); /* K=17, T=16: br x17 lands on the stub */
  __builtin_unreachable();
}

/* fault_kind: 0=fetch/exec-flip, 1=store exec-flip, 2=sparse-commit retry */
static void manual_resume(const ThreadExceptionDump *d, int is_fetch, int fault_kind) {
  const uint64_t pc = d->pc.x;
  const uint64_t sp = d->sp.x;
  if (fault_kind == 2) {
    manual_resume_retry_same_pc(d);
    return;
  }
  if (!is_fetch) {
    manual_resume_store(d);
    return;
  }
  if (sp & 0xf) {
    log_line("manual-resume: guest sp misaligned: 0x", sp, 0, 0);
    return;
  }
  int k = -1;
  for (int i = 0; i < 31; i++) {
    if (gpr(d, i) == pc) {
      k = i;
      break;
    }
  }
  if (k < 0) {
    /* No register holds pc (straight-line fetch into newly-RX chunk).
     * Retry same pc via x17 clobber path. */
    if (log_budget(&g_resume_logs, 32))
      log_line("manual-resume: fetch-retry-same-pc=0x", pc, 0, 0);
    manual_resume_retry_same_pc(d);
    return;
  }
  const int t = (k == 17) ? 16 : 17;

  /* frame layout (dwords): 0..30 x0..x30, 31 sp, 32 nzcv, 33 pad,
   * 34 original xT, 35 pc */
  uint64_t frame[36];
  for (int i = 0; i < 31; i++)
    frame[i] = gpr(d, i);
  frame[31] = sp;
  frame[32] = (uint64_t)(d->pstate & 0xf0000000u);
  frame[33] = 0;
  frame[34] = gpr(d, t);
  frame[35] = pc;
  if (log_budget(&g_resume_logs, 32))
    log_line("manual-resume: pc=0x", pc, " via x", (uint64_t)k);
  g_resume_fns[k](frame);
  __builtin_unreachable();
}

/* Run a registered guest signal handler for this fault and apply whatever it
 * changed (registers, sp, pc, flags) to the context that will be resumed.
 * Handlers that emulate the faulting access write their result into a
 * register and advance pc; ignoring those edits resumed with stale values. */
static int deliver_signal_to(ThreadExceptionDump *context, int signum,
                             uint64_t fault_address) {
  uint64_t regs[34];
  for (int i = 0; i < 31; i++)
    regs[i] = gpr(context, i);
  regs[31] = context->sp.x;
  regs[32] = context->pc.x;
  regs[33] = context->pstate;
  uint64_t resume_pc = context->pc.x;
  if (!ib3_deliver_signal(signum, fault_address, context->pc.x, regs,
                          &resume_pc))
    return 0;
  for (int i = 0; i < 29; i++)
    context->cpu_gprs[i].x = regs[i];
  context->fp.x = regs[29];
  context->lr.x = regs[30];
  if (regs[31] && !(regs[31] & 0xf))
    context->sp.x = regs[31];
  context->pc.x = resume_pc;
  context->pstate = (context->pstate & ~0xf0000000u) |
                    ((uint32_t)regs[33] & 0xf0000000u);
  return 1;
}

void ib3_exception_handler_c(ThreadExceptionDump *context) {
  static volatile int handling_exception;

  if (context_is_readable(context)) {
    const uint32_t ec = (context->esr >> 26) & 0x3f;
    const int is_fetch = ec == 0x20 || ec == 0x21;
    const int is_data = ec == 0x24 || ec == 0x25;

    if (is_fetch || is_data) {
      const uint64_t address = is_fetch ? context->pc.x : context->far.x;
      const int kind = ib3_handle_exec_fault(is_fetch, address);
      if (kind) {
        /* Data aborts (exec flip or sparse commit) re-run the access through
         * the stub path when possible; fetches resume through a register. */
        manual_resume(context, is_fetch, is_data ? 1 : kind);
      } else if (is_data) {
        /* Unhandled data abort -> synthesised SIGSEGV (11) if registered. */
        if (deliver_signal_to(context, 11, address))
          manual_resume(context, 0, 2);
      }
    }

    /* EC=0x18: trapped MSR/MRS. Horizon lets EL0 read CNTPCT_EL0 and CNTFRQ_EL0
     * but traps CNTVCT_EL0, which Android code (mach_absolute_time in libib3's
     * generated code) reads. Patch the instruction in place when it lives in
     * one of our code pools; otherwise emulate the read and skip it. */
    if (ec == 0x18) {
      const uint32_t iss = context->esr & 0x1ffffffu;
      const int is_read = iss & 1;
      const uint32_t crm = (iss >> 1) & 0xf, rt = (iss >> 5) & 0x1f;
      const uint32_t crn = (iss >> 10) & 0xf, op1 = (iss >> 14) & 7;
      const uint32_t op2 = (iss >> 17) & 7, op0 = (iss >> 20) & 3;
      if (is_read && op0 == 3 && op1 == 3 && crn == 14 && crm == 0 && op2 <= 2) {
        if (!(op2 == 2 && ib3_patch_counter_read(context->pc.x))) {
          const uint64_t value = op2 == 0 ? 19200000ull : armGetSystemTick();
          if (rt < 29)
            context->cpu_gprs[rt].x = value;
          else if (rt == 29)
            context->fp.x = value;
          else if (rt == 30)
            context->lr.x = value;
          context->pc.x += 4;
        }
        /* instruction rewritten (retry same pc) or emulated (pc advanced) */
        manual_resume(context, 1, 2);
      }
    }

    /* EC=0: undefined instruction. Guest JIT uses zero/UDF words as traps
     * that a SIGILL handler is expected to patch and resume. */
    if (ec == 0) {
      if (deliver_signal_to(context, 4 /* SIGILL */, context->pc.x))
        manual_resume(context, 1, 2);
    }
  }

  /* crash: */
  if (__atomic_exchange_n(&handling_exception, 1, __ATOMIC_SEQ_CST)) {
    svcBreak(BreakReason_Panic, (uint64_t)(uintptr_t)context, 0);
    for (;;)
      svcSleepThread(1000000000ULL);
  }

  char line[512];
  char *cursor = append_text(line, "exception-raw: context=0x");
  cursor = append_hex(cursor, (uintptr_t)context, 16);
  if (context_is_readable(context)) {
    cursor = append_text(cursor, " desc=0x");
    cursor = append_hex(cursor, context->error_desc, 8);
    cursor = append_text(cursor, " esr=0x");
    cursor = append_hex(cursor, context->esr, 8);
    cursor = append_text(cursor, " pc=0x");
    cursor = append_hex(cursor, context->pc.x, 16);
    cursor = append_text(cursor, " lr=0x");
    cursor = append_hex(cursor, context->lr.x, 16);
    cursor = append_text(cursor, " far=0x");
    cursor = append_hex(cursor, context->far.x, 16);
    cursor = append_text(cursor, " sp=0x");
    cursor = append_hex(cursor, context->sp.x, 16);
  } else {
    cursor = append_text(cursor, " UNREADABLE");
  }
  *cursor++ = '\n';
  debugEmergencyWrite(line, (size_t)(cursor - line));

  if (context_is_readable(context)) {
    /* ESR EC=0 means the CPU could not decode the word at pc (undefined
     * instruction). Show what is really there, and around the caller. */
    log_line("crash: ec=0x", (context->esr >> 26) & 0x3f, " pstate=0x", context->pstate);
    dump_words("words pc=0x", context->pc.x, 4, 8);
    dump_words("words lr=0x", context->lr.x, 4, 4);
    dump_regs(context);
  }

  /* Preserve Atmosphere's normal crash reporting after our log is flushed. */
  const uint64_t pc = context_is_readable(context) ? context->pc.x : 0;
  const uint64_t far = context_is_readable(context) ? context->far.x : 0;
  svcBreak(BreakReason_Panic, pc, far);
  for (;;)
    svcSleepThread(1000000000ULL);
}
