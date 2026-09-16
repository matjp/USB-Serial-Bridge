/*
 * efi_shim.h - Host-side shim for GNU-EFI types used by the Layer 0 modules.
 *
 * The bridge modules (B2, B3, B4, O1) are written against GNU-EFI types
 * (UINT8, UINTN, BOOLEAN, INT8, UINT32) from <efi.h>. To unit-test them on
 * the host (no UEFI, no hardware), we provide equivalent definitions here.
 *
 * This shim is ONLY for the host test harness (tests/). It is NOT part of
 * the firmware build. The real types come from /usr/include/efi/ at build
 * time; this file just mirrors the subset the Layer 0 modules use.
 *
 * The mailbox uses an x86 mfence for producer ordering. On the host we
 * compile for x86-64, so the same inline asm is valid; we keep it identical
 * to the firmware so the test exercises the real ordering primitive.
 */

#ifndef EFI_SHIM_H
#define EFI_SHIM_H

#include <stdint.h>
#include <stddef.h>   /* NULL */

#ifndef NULL
#define NULL ((void *)0)
#endif

/* GNU-EFI base types (subset used by Layer 0 modules). */
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef uint64_t  UINTN;
typedef int8_t    INT8;
typedef int16_t   INT16;
typedef int32_t   INT32;
typedef int64_t   INT64;
typedef int64_t   INTN;

/* GNU-EFI BOOLEAN is _Bool (1 byte). */
typedef _Bool     BOOLEAN;

#ifndef TRUE
#define TRUE  ((BOOLEAN)1)
#endif
#ifndef FALSE
#define FALSE ((BOOLEAN)0)
#endif

/* EFI_STATUS is UINTN; we only need the success/error macros for tests. */
typedef UINTN EFI_STATUS;
#define EFI_SUCCESS 0
#define EFI_ERROR(s) ((s) != 0)

/* CHAR16 is 16-bit (matches -fshort-wchar in the firmware build). */
typedef uint16_t CHAR16;

/* EFIAPI is the calling convention; on x86-64 SysV it is a no-op. */
#ifndef EFIAPI
#define EFIAPI
#endif

/* The mailbox producer uses an x86 mfence. Keep it identical to the
 * firmware so the host test exercises the real ordering primitive. */
#ifndef __GNUC__
#error "efi_shim.h requires GCC/Clang for the mfence inline asm"
#endif

#endif /* EFI_SHIM_H */
