/*
 * exception_handler.c - UEFI CPU exception trap + stack trace (debug aid).
 *
 * Installs an EFI_CPU_ARCH_PROTOCOL interrupt handler for the x64 fault
 * vectors most likely to indicate a bug in the setup app (#PF, #GP, #UD,
 * #DF, #SS, #NP, #TS, #AC, #MC, #DE, #BP, #OF, #BR, #NM, #XM). When one
 * fires, the handler prints:
 *
 *   - the exception vector name and the faulting RIP,
 *   - the full x64 register dump (RAX..R15, RSP, RBP, CR2, RFLAGS, ...),
 *   - a raw stack trace obtained by walking the RBP frame-pointer chain.
 *
 * The stack trace entries are absolute addresses. Because the image is
 * position-independent (relocated at load), the useful value is the offset
 * from the image base; the handler prints both the absolute address and the
 * offset from the recorded image base so addr2line can resolve it:
 *
 *   addr2line -e build-debug/bridge-debug.so <image_base + offset>
 *
 * After printing, the handler halts (BS->Stall loop) so the trace is
 * captured in the serial log and the machine does not re-fault in a loop.
 *
 * ISO C99, no standard headers. Types map to <efi.h> / <efidebug.h>.
 * EFI_CPU_ARCH_PROTOCOL is not shipped by GNU-EFI, so the minimal struct and
 * GUID are declared here per the UEFI 2.x spec (CPU Architectural Protocol).
 */

#include <efi.h>
#include <efilib.h>
#include <efidebug.h>

#include "exception_handler.h"

/* ------------------------------------------------------------------ */
/* EFI_CPU_ARCH_PROTOCOL (UEFI 2.x spec) - not in GNU-EFI headers      */
/* ------------------------------------------------------------------ */

#define EFI_CPU_ARCH_PROTOCOL_GUID_VALUE \
    { 0x26baccb1, 0x6f42, 0x11d4, \
      { 0xbc, 0xe7, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

static EFI_GUID gEfiCpuArchProtocolGuid = EFI_CPU_ARCH_PROTOCOL_GUID_VALUE;

typedef
VOID
(EFIAPI *EFI_CPU_INTERRUPT_HANDLER)(
    IN EFI_EXCEPTION_TYPE InterruptType,
    IN EFI_SYSTEM_CONTEXT SystemContext);

typedef struct _EFI_CPU_ARCH_PROTOCOL EFI_CPU_ARCH_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_CPU_REGISTER_INTERRUPT_HANDLER)(
    IN EFI_CPU_ARCH_PROTOCOL *This,
    IN UINTN InterruptType,
    IN EFI_CPU_INTERRUPT_HANDLER InterruptHandler);

struct _EFI_CPU_ARCH_PROTOCOL {
    VOID *FlushDataCache;
    VOID *EnableInterrupt;
    VOID *DisableInterrupt;
    VOID *GetInterruptState;
    VOID *Init;
    EFI_CPU_REGISTER_INTERRUPT_HANDLER RegisterInterruptHandler;
    VOID *GetTimerValue;
    VOID *SetMemoryAttributes;
    UINT32 NumberOfTimers;
    UINT32 DmaBufferAlignment;
};

/* ------------------------------------------------------------------ */
/* Image base (for computing RIP offsets)                              */
/* ------------------------------------------------------------------ */

/* Recorded once at install time via EFI_LOADED_IMAGE_PROTOCOL on the image
 * handle passed to efi_main. RIP - ImageBase gives the offset that
 * addr2line expects against the .so. */
static UINTN g_image_base = 0;

/* ------------------------------------------------------------------ */
/* Exception vector names                                              */
/* ------------------------------------------------------------------ */

static const CHAR16 *
exception_name(EFI_EXCEPTION_TYPE type)
{
    switch (type) {
    case EXCEPT_X64_DIVIDE_ERROR:    return L"#DE Divide Error";
    case EXCEPT_X64_DEBUG:           return L"#DB Debug";
    case EXCEPT_X64_NMI:             return L"NMI";
    case EXCEPT_X64_BREAKPOINT:      return L"#BP Breakpoint";
    case EXCEPT_X64_OVERFLOW:        return L"#OF Overflow";
    case EXCEPT_X64_BOUND:           return L"#BR BOUND Range";
    case EXCEPT_X64_INVALID_OPCODE:  return L"#UD Invalid Opcode";
    case EXCEPT_X64_DOUBLE_FAULT:    return L"#DF Double Fault";
    case EXCEPT_X64_INVALID_TSS:     return L"#TS Invalid TSS";
    case EXCEPT_X64_SEG_NOT_PRESENT: return L"#NP Segment Not Present";
    case EXCEPT_X64_STACK_FAULT:     return L"#SS Stack Fault";
    case EXCEPT_X64_GP_FAULT:        return L"#GP General Protection";
    case EXCEPT_X64_PAGE_FAULT:      return L"#PF Page Fault";
    case EXCEPT_X64_FP_ERROR:        return L"#NM x87 FP Error";
    case EXCEPT_X64_ALIGNMENT_CHECK: return L"#AC Alignment Check";
    case EXCEPT_X64_MACHINE_CHECK:   return L"#MC Machine Check";
    case EXCEPT_X64_SIMD:            return L"#XM SIMD FP Exception";
    default:                         return L"Unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Stack trace walk (RBP frame-pointer chain)                          */
/* ------------------------------------------------------------------ */

/* Walk the x64 frame-pointer chain starting at the faulting RBP and print
 * up to max_frames return addresses. Each frame is laid out as:
 *   [RBP]     = saved RBP of the caller (next frame)
 *   [RBP + 8] = return address into the caller
 * The walk stops when RBP is null, not 16-byte aligned (a corrupted chain),
 * or outside the low UEFI memory range, or when max_frames is reached.
 * NOTE: UEFI runs with identity-mapped low memory (typically < 4 GB), so
 * frame pointers are NOT canonical x86-64 addresses - do not apply a
 * canonical-address check here. */
static void
print_stack_trace(UINT64 rbp, UINTN max_frames)
{
    UINTN frame;
    UINT64 cur = rbp;

    Print(L"BRIDGE FAULT: stack trace (RBP chain):\n");
    for (frame = 0; frame < max_frames; frame++) {
        UINT64 *fp;
        UINT64 ret;

        /* Stop on a null frame pointer. */
        if (cur == 0)
            break;
        /* Frame pointers must be 16-byte aligned; a misaligned value means
         * the chain is corrupted - stop rather than dereference garbage. */
        if ((cur & 0xF) != 0)
            break;
        /* UEFI stacks live in low memory; a frame pointer above 4 GB is
         * almost certainly garbage. */
        if (cur > 0xFFFFFFFFull)
            break;

        fp = (UINT64 *)(UINTN)cur;
        ret = fp[1];   /* return address at [RBP + 8] */

        Print(L"  #%d  ret=0x%016llX  (offset 0x%llX)\n",
              frame,
              (unsigned long long)ret,
              (unsigned long long)(ret - (UINT64)g_image_base));

        cur = fp[0];   /* saved RBP at [RBP] */
    }
}

/* ------------------------------------------------------------------ */
/* The exception handler                                               */
/* ------------------------------------------------------------------ */

/* OVMF (and all UEFI firmware) is built with EFIAPI = Microsoft x64 ABI.
 * Our app is built with EFIAPI empty (SysV ABI, since HAVE_USE_MS_ABI is
 * not defined), so a plain EFIAPI handler would receive the arguments in
 * the wrong registers. Force the MS x64 calling convention on the handler
 * so the firmware's call (InterruptType in RCX, SystemContext in RDX) is
 * decoded correctly. */
#if defined(__GNUC__) && defined(__x86_64__)
#define BRIDGE_MS_ABI __attribute__((ms_abi))
#else
#define BRIDGE_MS_ABI
#endif

static VOID BRIDGE_MS_ABI
exception_handler(EFI_EXCEPTION_TYPE type, EFI_SYSTEM_CONTEXT ctx)
{
    EFI_SYSTEM_CONTEXT_X64 *x = ctx.SystemContextX64;

    Print(L"\nBRIDGE FAULT: CPU exception %d (%s) at RIP=0x%016llX\n",
          type, exception_name(type), (unsigned long long)x->Rip);
    Print(L"BRIDGE FAULT: RIP offset from image base = 0x%llX\n",
          (unsigned long long)(x->Rip - (UINT64)g_image_base));

    Print(L"BRIDGE FAULT: RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
          (unsigned long long)x->Rax, (unsigned long long)x->Rbx,
          (unsigned long long)x->Rcx, (unsigned long long)x->Rdx);
    Print(L"BRIDGE FAULT: RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX\n",
          (unsigned long long)x->Rsi, (unsigned long long)x->Rdi,
          (unsigned long long)x->Rbp, (unsigned long long)x->Rsp);
    Print(L"BRIDGE FAULT: R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX\n",
          (unsigned long long)x->R8, (unsigned long long)x->R9,
          (unsigned long long)x->R10, (unsigned long long)x->R11);
    Print(L"BRIDGE FAULT: R12=%016llX R13=%016llX R14=%016llX R15=%016llX\n",
          (unsigned long long)x->R12, (unsigned long long)x->R13,
          (unsigned long long)x->R14, (unsigned long long)x->R15);
    Print(L"BRIDGE FAULT: CR2=%016llX RFLAGS=%016llX CS=%016llX SS=%016llX\n",
          (unsigned long long)x->Cr2, (unsigned long long)x->Rflags,
          (unsigned long long)x->Cs, (unsigned long long)x->Ss);
    Print(L"BRIDGE FAULT: ExceptionData=%016llX\n",
          (unsigned long long)x->ExceptionData);

    print_stack_trace(x->Rbp, 16);

    Print(L"BRIDGE FAULT: halting (trap).\n");

    /* Halt so the trace is captured and the machine does not re-fault. */
    for (;;)
        uefi_call_wrapper(BS->Stall, 1, 10000000);   /* 10 s per iteration */
}

/* ------------------------------------------------------------------ */
/* Install                                                             */
/* ------------------------------------------------------------------ */

EFI_STATUS
uefi_install_exception_handler(EFI_HANDLE image)
{
    EFI_STATUS status;
    EFI_CPU_ARCH_PROTOCOL *cpu = NULL;
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    UINTN i;

    /* Record the image base so the handler can print RIP offsets. */
    status = uefi_call_wrapper(
        BS->HandleProtocol, 3,
        image, &gEfiLoadedImageProtocolGuid, (VOID **)&li);
    if (EFI_ERROR(status) || li == NULL) {
        /* Not fatal - offsets will just be absolute. */
        g_image_base = 0;
    } else {
        g_image_base = (UINTN)li->ImageBase;
    }

    /* Locate the CPU architectural protocol. OVMF and most firmware expose
     * it; if not, we simply leave the firmware's default handler in place. */
    status = uefi_call_wrapper(
        BS->LocateProtocol, 3,
        &gEfiCpuArchProtocolGuid, NULL, (VOID **)&cpu);
    if (EFI_ERROR(status) || cpu == NULL) {
        Print(L"BRIDGE-DBG: exception trap unavailable "
              L"(no EFI_CPU_ARCH_PROTOCOL)\n");
        return EFI_UNSUPPORTED;
    }

    /* Register the handler for every x64 fault vector we care about. */
    {
        static const UINTN vectors[] = {
            EXCEPT_X64_DIVIDE_ERROR,    /* 0  */
            EXCEPT_X64_DEBUG,           /* 1  */
            EXCEPT_X64_BREAKPOINT,      /* 3  */
            EXCEPT_X64_OVERFLOW,        /* 4  */
            EXCEPT_X64_BOUND,           /* 5  */
            EXCEPT_X64_INVALID_OPCODE,  /* 6  */
            EXCEPT_X64_DOUBLE_FAULT,    /* 8  */
            EXCEPT_X64_INVALID_TSS,     /* 10 */
            EXCEPT_X64_SEG_NOT_PRESENT, /* 11 */
            EXCEPT_X64_STACK_FAULT,     /* 12 */
            EXCEPT_X64_GP_FAULT,        /* 13 */
            EXCEPT_X64_PAGE_FAULT,      /* 14 */
            EXCEPT_X64_FP_ERROR,        /* 16 */
            EXCEPT_X64_ALIGNMENT_CHECK, /* 17 */
            EXCEPT_X64_MACHINE_CHECK,   /* 18 */
            EXCEPT_X64_SIMD,            /* 19 */
        };

        for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
            status = uefi_call_wrapper(
                cpu->RegisterInterruptHandler, 3,
                cpu, vectors[i], exception_handler);
            /* Ignore per-vector registration failures: on many platforms the
             * vectors are already handled by the firmware, so
             * RegisterInterruptHandler returns EFI_ALREADY_STARTED. That is
             * expected and not worth logging. */
            (void)status;
        }
    }

    Print(L"BRIDGE-DBG: exception trap installed (image base 0x%llX)\n",
          (unsigned long long)g_image_base);
    return EFI_SUCCESS;
}
