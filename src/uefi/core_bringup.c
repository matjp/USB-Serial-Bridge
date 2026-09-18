/*
 * core_bringup.c - U2: Highest-core bring-up via SIPI.
 *
 * Determines the core count (CPUID leaf 0xB / ACPI MADT), picks the
 * highest-numbered core as the bridge core, sets up its GDT, stack, and page
 * tables, loads the bridge code, and starts it via SIPI.
 *
 * The AP wakes in 16-bit real mode at the STARTUP vector. A small real-mode
 * trampoline (copied to a low fixed address) switches the AP to 64-bit long
 * mode - loading a flat GDT, enabling PAE, loading CR3 with an identity-map
 * page table, setting EFER.LME, and far-jumping to a 64-bit stub that loads
 * the bridge stack and calls bridge_entry() (B5).
 *
 * The page tables identity-map the low 4 GB with 2 MB pages. This covers the
 * UEFI image (where the bridge code and its static data live), the reserved
 * region (bridge stack, USB_TOPOLOGY), the XHCI MMIO, and the virtual 8042
 * port region (0x10000000). The bridge runs in place from the image; the
 * copy-to-reserved-region step is deferred to the production handoff.
 *
 * See docs/architecture.md section 7.3, module U2.
 */

#include <efi.h>
#include <efilib.h>

#include "uefi.h"
#include "../bridge/usb_topology.h"

/* ------------------------------------------------------------------ */
/* Constants (standard x86 APIC multiprocessor conventions).           */
/* ------------------------------------------------------------------ */

/* INIT IPI delivery mode (level assert) and STARTUP IPI delivery mode,
 * per the standard x86 multiprocessor startup convention. */
#define ICR_INIT_LEVEL_ASSERT  0x0000C500
#define ICR_STARTUP            0x00000600

/* Real-mode address where the AP trampoline is copied and executed. The
 * STARTUP IPI vector is the page number of this address. 0x8000 is the
 * conventional AP-startup location in the low 640 KB. */
#define TRAMPOLINE_ADDR        0x8000
#define STARTUP_VECTOR         (TRAMPOLINE_ADDR >> 12)   /* 0x08 */

/* Bridge core stack size (bytes), allocated in the reserved region. */
#define BRIDGE_STACK_SIZE      0x4000   /* 16 KiB */

/* GDT selectors (flat 64-bit segments). */
#define GDT_CODE_SEL           0x08
#define GDT_DATA_SEL           0x10

/* ------------------------------------------------------------------ */
/* Page tables: identity-map the low 4 GB with 2 MB pages.             */
/*                                                                     */
/* PML4[0] -> PDPT; PDPT[0..3] -> four 512-entry blocks of the PD;     */
/* each PD entry is a 2 MB page (PS=1) covering 0..4 GB.               */
/* ------------------------------------------------------------------ */
#define PD_ENTRIES  2048   /* 4 GB / 2 MB */
static UINT64 g_pml4[512] __attribute__((aligned(4096)));
static UINT64 g_pdpt[512] __attribute__((aligned(4096)));
static UINT64 g_pd[PD_ENTRIES] __attribute__((aligned(4096)));

/* Flat 64-bit GDT: null, code, data. */
static UINT64 g_gdt[3] __attribute__((aligned(8)));

/* Bridge stack top (set before SIPI; read by the 64-bit AP stub).
 * Global (not static) so the naked asm's RIP-relative reference resolves to a
 * normal data-section relocation rather than one against a local symbol. */
UINT64 g_bridge_stack_top;

/* 64-bit AP entry stub (defined below). Forward-declared so install_trampoline
 * can take its address to patch the trampoline's far-jump target. */
__attribute__((naked)) static void ap_entry64(void);

/* ------------------------------------------------------------------ */
/* Real-mode AP trampoline (16-bit machine code) + its data area.      */
/*                                                                     */
/* The trampoline is copied to TRAMPOLINE_ADDR and patched with the    */
/* GDT pointer, CR3 (PML4 base), and the 64-bit entry point. Layout:   */
/*                                                                     */
/*   code  : TRAMPOLINE_CODE_LEN bytes of 16-bit code                  */
/*   +0x37 : gdt_ptr  (6 bytes: limit + base)                          */
/*   +0x3D : cr3_val  (8 bytes: PML4 base, low 32 bits used)           */
/*   +0x45 : entry64  (8 bytes: 64-bit entry point, low 32 bits used)  */
/*                                                                     */
/* The code (all 16-bit, operand-size prefix 0x66 for 32-bit ops):     */
/*   cli                                                               */
/*   lgdt [gdt_ptr]                                                    */
/*   mov eax, cr4 ; or eax, 0x20 (PAE) ; mov cr4, eax                  */
/*   mov eax, [cr3_val] ; mov cr3, eax                                 */
/*   mov ecx, 0xC0000080 (EFER) ; rdmsr ; or eax, 0x100 (LME) ; wrmsr  */
/*   mov eax, cr0 ; or eax, 0x80000001 (PG|PE) ; mov cr0, eax          */
/*   jmp 0x08:entry64   (far jump into 64-bit code)                    */
/* ------------------------------------------------------------------ */
#define TRAMPOLINE_CODE_LEN  0x37
#define TRAMP_GDT_PTR_OFF    0x37
#define TRAMP_CR3_OFF        0x3D
#define TRAMP_ENTRY64_OFF    0x45
#define TRAMP_LGDT_DISP      0x04   /* disp16 of the lgdt operand */
#define TRAMP_CR3_DISP       0x11   /* disp16 of mov eax,[cr3_val] */
#define TRAMP_FARJMP_OFF     0x31   /* off32 of the far jump */

static const UINT8 g_trampoline_code[TRAMPOLINE_CODE_LEN] = {
    0xFA,                                        /* cli */
    0x0F, 0x01, 0x16, 0x00, 0x00,                /* lgdt [gdt_ptr] */
    0x0F, 0x20, 0xE0,                            /* mov eax, cr4 */
    0x83, 0xC8, 0x20,                            /* or eax, 0x20 (PAE) */
    0x0F, 0x22, 0xE0,                            /* mov cr4, eax */
    0x66, 0xA1, 0x00, 0x00,                      /* mov eax, [cr3_val] */
    0x0F, 0x22, 0xD8,                            /* mov cr3, eax */
    0xB9, 0x80, 0x00, 0x00, 0xC0,                /* mov ecx, 0xC0000080 */
    0x0F, 0x32,                                  /* rdmsr */
    0x0D, 0x00, 0x01, 0x00, 0x00,                /* or eax, 0x100 (LME) */
    0x0F, 0x30,                                  /* wrmsr */
    0x0F, 0x20, 0xC0,                            /* mov eax, cr0 */
    0x0D, 0x01, 0x00, 0x00, 0x80,                /* or eax, 0x80000001 */
    0x0F, 0x22, 0xC0,                            /* mov cr0, eax */
    0x66, 0xEA, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00  /* jmp 0x08:entry64 */
};

/* ------------------------------------------------------------------ */
/* CPUID helpers.                                                      */
/* ------------------------------------------------------------------ */

/* Return the maximum basic CPUID leaf. */
static UINT32
cpuid_max_leaf(void)
{
    UINT32 eax, ebx, ecx, edx;
    __asm__ __volatile__(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0)
        : "memory");
    return eax;
}

/* CPUID leaf 0xB (x2APIC topology): returns the number of logical
 * processors (EBX bits 15:0 of sub-leaf 0). Returns 0 if leaf 0xB is not
 * supported. */
static UINT32
cpuid_max_logical_processors(void)
{
    UINT32 eax, ebx, ecx, edx;

    if (cpuid_max_leaf() < 0x0B)
        return 0;

    __asm__ __volatile__(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x0B), "c"(0)
        : "memory");
    return ebx & 0xFFFF;
}

/* ------------------------------------------------------------------ */
/* Local APIC helpers.                                                 */
/* ------------------------------------------------------------------ */

/* Write to the local APIC ICR (interrupt command register) to send an IPI.
 * The local APIC is memory-mapped at the standard base 0xFEE00000. */
static void
lapic_write(UINT32 offset, UINT32 value)
{
    volatile UINT32 *lapic = (volatile UINT32 *)0xFEE00000ULL;
    lapic[offset / 4] = value;
}

/* Send an INIT IPI to the given APIC ID. */
static void
send_init_ipi(UINT32 apic_id)
{
    /* ICR low: delivery mode 101 (INIT), level assert, trigger level. */
    lapic_write(0x310, (apic_id << 24) | ICR_INIT_LEVEL_ASSERT);
}

/* Send a STARTUP IPI to the given APIC ID with the given vector. */
static void
send_startup_ipi(UINT32 apic_id, UINT32 vector)
{
    /* ICR low: delivery mode 110 (STARTUP), vector. */
    lapic_write(0x310, (apic_id << 24) | ICR_STARTUP | (vector & 0xFF));
}

/* ------------------------------------------------------------------ */
/* Bridge core environment setup.                                      */
/* ------------------------------------------------------------------ */

/* Build the identity-map page tables (low 4 GB, 2 MB pages). */
static void
setup_page_tables(void)
{
    UINTN i;

    for (i = 0; i < 512; i++) {
        g_pml4[i] = 0;
        g_pdpt[i] = 0;
    }
    for (i = 0; i < PD_ENTRIES; i++)
        g_pd[i] = 0;

    /* PML4[0] -> PDPT. */
    g_pml4[0] = (UINT64)(UINTN)g_pdpt | 0x3;   /* present + writable */

    /* PDPT[0..3] -> four 512-entry blocks of the PD (each covers 1 GB). */
    for (i = 0; i < 4; i++)
        g_pdpt[i] = (UINT64)(UINTN)(g_pd + i * 512) | 0x3;

    /* PD entries: 2 MB pages, present + writable + PS. */
    for (i = 0; i < PD_ENTRIES; i++)
        g_pd[i] = ((UINT64)i << 21) | 0x83;
}

/* Build the flat 64-bit GDT. */
static void
setup_gdt(void)
{
    g_gdt[0] = 0;                              /* null descriptor */
    g_gdt[1] = 0x00AF9A000000FFFFULL;          /* 64-bit code, DPL0 */
    g_gdt[2] = 0x00AF92000000FFFFULL;          /* 64-bit data, DPL0 */
}

/* Write a little-endian 16-bit value. */
static void
put_le16(UINT8 *p, UINT16 v)
{
    p[0] = (UINT8)(v & 0xFF);
    p[1] = (UINT8)((v >> 8) & 0xFF);
}

/* Write a little-endian 32-bit value. */
static void
put_le32(UINT8 *p, UINT32 v)
{
    p[0] = (UINT8)(v & 0xFF);
    p[1] = (UINT8)((v >> 8) & 0xFF);
    p[2] = (UINT8)((v >> 16) & 0xFF);
    p[3] = (UINT8)((v >> 24) & 0xFF);
}

/* Copy the trampoline to TRAMPOLINE_ADDR and patch its data area and
 * immediates with the GDT pointer, CR3 (PML4 base), and 64-bit entry. */
static void
install_trampoline(void)
{
    UINT8 *tramp = (UINT8 *)(UINTN)TRAMPOLINE_ADDR;
    UINT64 gdt_base = (UINT64)(UINTN)g_gdt;
    UINT16 gdt_limit = (UINT16)(sizeof(g_gdt) - 1);
    UINT64 cr3 = (UINT64)(UINTN)g_pml4;
    UINT64 entry = (UINT64)(UINTN)ap_entry64;
    UINTN i;

    /* Copy the code. */
    for (i = 0; i < TRAMPOLINE_CODE_LEN; i++)
        tramp[i] = g_trampoline_code[i];

    /* Patch the GDT pointer (limit + base) in the data area. */
    put_le16(tramp + TRAMP_GDT_PTR_OFF, gdt_limit);
    put_le32(tramp + TRAMP_GDT_PTR_OFF + 2, (UINT32)gdt_base);

    /* Patch the lgdt disp16 to point at the GDT pointer. */
    put_le16(tramp + TRAMP_LGDT_DISP, TRAMP_GDT_PTR_OFF);

    /* Patch the CR3 value (PML4 base, low 32 bits) in the data area. */
    put_le32(tramp + TRAMP_CR3_OFF, (UINT32)cr3);

    /* Patch the mov eax,[cr3_val] disp16 to point at the CR3 value. */
    put_le16(tramp + TRAMP_CR3_DISP, TRAMP_CR3_OFF);

    /* Patch the far-jump offset (64-bit entry point, low 32 bits). The
     * selector 0x08 is already in the code. */
    put_le32(tramp + TRAMP_FARJMP_OFF, (UINT32)entry);

    /* Ensure the writes are visible to the AP before SIPI. */
    __asm__ __volatile__("" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* 64-bit AP entry stub.                                               */
/*                                                                     */
/* Runs in long mode after the trampoline's far jump. Loads the flat   */
/* data segments and the bridge stack, then calls bridge_entry() (B5). */
/* ------------------------------------------------------------------ */
__attribute__((naked)) static void
ap_entry64(void)
{
    __asm__ __volatile__(
        "movw $0x10, %%ax\n\t"                 /* data selector */
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "movq g_bridge_stack_top(%%rip), %%rsp\n\t"
        "call bridge_entry\n\t"
        "1:\n\t"
        "hlt\n\t"
        "jmp 1b\n\t"
        ::: "memory");
}

/* ------------------------------------------------------------------ */
/* uefi_bringup_highest_core(): determine core count, set up the        */
/* highest core's GDT/stack/page tables, load the bridge code, and      */
/* start the core via SIPI.                                             */
/* ------------------------------------------------------------------ */
EFI_STATUS
uefi_bringup_highest_core(void)
{
    UINT32 core_count;
    UINT32 highest_core;
    EFI_PHYSICAL_ADDRESS stack_addr = 0;
    EFI_STATUS status;

    /* 1. Determine the core count (CPUID leaf 0xB). */
    core_count = cpuid_max_logical_processors();
    if (core_count == 0)
        core_count = 1;   /* fallback: assume at least one core */

    /* 2. Pick the highest-numbered core as the bridge core. */
    highest_core = core_count - 1;

    /* 3. Allocate a stack for the bridge core in the reserved region. */
    status = uefi_call_wrapper(
        BS->AllocatePages, 4, AllocateAnyPages, EfiReservedMemoryType,
        BRIDGE_STACK_SIZE / 4096, &stack_addr);
    if (EFI_ERROR(status))
        return status;
    g_bridge_stack_top = stack_addr + BRIDGE_STACK_SIZE;

    /* 4. Set up the bridge core's environment: page tables + GDT. */
    setup_page_tables();
    setup_gdt();

    /* 5. Install the real-mode trampoline at the STARTUP vector. */
    install_trampoline();

    /* 6. Start the highest core via SIPI.
     *
     * Per the standard x86 multiprocessor convention: send an INIT IPI
     * (level assert) then a STARTUP IPI with the trampoline's page number.
     * The AP begins executing the trampoline, which switches to long mode
     * and jumps to ap_entry64(), which loads the bridge stack and calls
     * bridge_entry() (B5). */
    send_init_ipi(highest_core);
    send_startup_ipi(highest_core, STARTUP_VECTOR);

    /* 7. Load the input adapter into the reserved region on core 0 (for
     *    O1/O2). The adapter code is also linked into the image; in a full
     *    implementation it is copied to a reserved region on core 0. This
     *    is deferred to the OS-integration phase. */

    return EFI_SUCCESS;
}
