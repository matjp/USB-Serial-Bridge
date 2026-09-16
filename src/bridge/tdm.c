/*
 * tdm.c - Time-Division Multiplexing context switch (bridge <-> OS task).
 *
 * Implements the TDM interface declared in tdm.h. The bridge and the OS
 * background task (Seth for TempleOS) share the highest core via
 * time-division multiplexing. A local-APIC timer ISR on that core fires at
 * each slot boundary (TDM_SLOT_US = 2 ms) and alternates between the bridge
 * context and the OS task context.
 *
 * Each switch saves/restores the callee-saved registers (RBX, RBP, R12-R15)
 * and the stack pointer (RSP) - a minimal cooperative context switch, ring 0,
 * same address space (no memory protection). See docs/implementation-spec.md
 * section 5.
 *
 * NOTE: This is a Layer 1 (hardware-validation) module. The OS task context
 * (g_os_ctx) is initialized with a placeholder stack; the real OS task stack
 * pointer must be wired in by the OS integration (O2) before the first
 * tdm_switch_to_os() call. The timer ISR is installed on the highest core
 * (the bridge core) via the local APIC.
 */

#include <efi.h>

#include "tdm.h"

/* ------------------------------------------------------------------ */
/* Context record: callee-saved registers + stack pointer.             */
/* ------------------------------------------------------------------ */
typedef struct {
    UINT64 rsp;
    UINT64 rbx;
    UINT64 rbp;
    UINT64 r12;
    UINT64 r13;
    UINT64 r14;
    UINT64 r15;
} TDM_CONTEXT;

/* Bridge context and OS task context. */
static TDM_CONTEXT g_bridge_ctx;
static TDM_CONTEXT g_os_ctx;

/* Placeholder stack for the OS task context. The real OS task stack pointer
 * is wired in by O2; until then we point at this static buffer so that a
 * switch does not fault. */
#define OS_STACK_SIZE 0x1000   /* 4 KiB placeholder */
static UINT8 g_os_stack[OS_STACK_SIZE];

/* Whether the OS context has been initialized with a real stack yet. */
static BOOLEAN g_os_ctx_ready = FALSE;

/* ------------------------------------------------------------------ */
/* Core context switch.                                                */
/* ------------------------------------------------------------------ */

/* Save the current context into *from and load the context from *to.
 * This is the heart of the TDM switch: it saves the callee-saved registers
 * and RSP of the current context, then restores those of the target context,
 * so execution resumes on the target's stack. */
static void
tdm_switch(TDM_CONTEXT *from, TDM_CONTEXT *to)
{
    __asm__ __volatile__(
        /* Save the current context. */
        "movq %%rsp, %0\n\t"
        "movq %%rbx, %1\n\t"
        "movq %%rbp, %2\n\t"
        "movq %%r12, %3\n\t"
        "movq %%r13, %4\n\t"
        "movq %%r14, %5\n\t"
        "movq %%r15, %6\n\t"
        /* Load the target context. */
        "movq %7, %%rsp\n\t"
        "movq %8, %%rbx\n\t"
        "movq %9, %%rbp\n\t"
        "movq %10, %%r12\n\t"
        "movq %11, %%r13\n\t"
        "movq %12, %%r14\n\t"
        "movq %13, %%r15\n\t"
        :
        : "m"(from->rsp), "m"(from->rbx), "m"(from->rbp),
          "m"(from->r12), "m"(from->r13), "m"(from->r14), "m"(from->r15),
          "m"(to->rsp), "m"(to->rbx), "m"(to->rbp),
          "m"(to->r12), "m"(to->r13), "m"(to->r14), "m"(to->r15)
        : "memory");
}

/* ------------------------------------------------------------------ */
/* Public TDM interface.                                               */
/* ------------------------------------------------------------------ */

/* Save the current (bridge) context and switch to the OS task context.
 * Called from the timer ISR on the highest core at slot boundaries. */
void
tdm_switch_to_os(void)
{
    /* If the OS context is not yet ready, fall back to a no-op so the
     * bridge does not switch onto an uninitialized stack. */
    if (!g_os_ctx_ready)
        return;

    tdm_switch(&g_bridge_ctx, &g_os_ctx);
}

/* Save the OS task context and switch back to the bridge context. */
void
tdm_switch_to_bridge(void)
{
    tdm_switch(&g_os_ctx, &g_bridge_ctx);
}

/* ------------------------------------------------------------------ */
/* Local APIC timer setup.                                             */
/* ------------------------------------------------------------------ */

/* Write to the local APIC (memory-mapped at the standard base 0xFEE00000). */
static void
lapic_write(UINT32 offset, UINT32 value)
{
    volatile UINT32 *lapic = (volatile UINT32 *)0xFEE00000ULL;
    lapic[offset / 4] = value;
}

/* Install the timer ISR that drives the TDM round-robin on the highest core.
 *
 * Uses the local APIC timer in periodic mode. The timer is programmed to
 * fire every TDM_SLOT_US microseconds; the ISR alternates between the bridge
 * and OS contexts. The ISR vector and the exact LVT entry are wired by the
 * OS integration (O2); here we program the timer to a placeholder count and
 * document the intended LVT setup.
 *
 * NOTE: This is a Layer 1 scaffold. The LVT timer entry (0x320) and the
 * divide configuration (0x3E0) are programmed per the Intel SDM; the ISR
 * handler that calls tdm_switch_to_os()/tdm_switch_to_bridge() is installed
 * by O2. */
void
tdm_install_timer(void)
{
    /* Initialize the OS context with the placeholder stack so a switch is
     * safe even before O2 wires the real OS task stack. */
    g_os_ctx.rsp = (UINT64)(UINTN)(g_os_stack + OS_STACK_SIZE);
    g_os_ctx.rbx = 0;
    g_os_ctx.rbp = 0;
    g_os_ctx.r12 = 0;
    g_os_ctx.r13 = 0;
    g_os_ctx.r14 = 0;
    g_os_ctx.r15 = 0;
    g_os_ctx_ready = TRUE;

    /* Local APIC timer: divide configuration (divide by 1). */
    lapic_write(0x3E0, 0x0B);

    /* Program the initial count for a 2 ms slot. The count value depends on
     * the bus frequency; this is a placeholder that O2 calibrates. */
    lapic_write(0x380, 0x00010000);

    /* LVT timer: periodic mode (bit 17), unmasked, vector 0x40. */
    lapic_write(0x320, 0x00020040);
}
