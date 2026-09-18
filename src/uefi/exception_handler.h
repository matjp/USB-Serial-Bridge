/*
 * exception_handler.h - UEFI CPU exception trap + stack trace (debug aid).
 *
 * Installs an EFI_DEBUG_SUPPORT_PROTOCOL exception callback so that a CPU
 * fault (e.g. #PF, #GP) inside the setup app is caught, a register dump and
 * a raw stack trace are printed to the console, and the machine halts -
 * instead of silently dying in the firmware's default handler.
 *
 * The printed return addresses are image-relative offsets that can be mapped
 * back to source lines offline with addr2line against the .so (the debug
 * build is compiled with -g, so the DWARF symbols are present).
 *
 * See docs/architecture.md and the OVMF CI test (grep for "BRIDGE FAULT").
 */

#ifndef EXCEPTION_HANDLER_H
#define EXCEPTION_HANDLER_H

#include <efi.h>

/* Install the exception trap. Safe to call once, right after InitializeLib.
 * `image` is the EFI_HANDLE passed to efi_main (used to record the image
 * base for RIP-offset reporting). If the platform does not expose
 * EFI_CPU_ARCH_PROTOCOL this is a no-op (returns EFI_UNSUPPORTED) and the
 * firmware's default handler remains. */
EFI_STATUS uefi_install_exception_handler(EFI_HANDLE image);

#endif /* EXCEPTION_HANDLER_H */
