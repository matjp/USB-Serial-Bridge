/*
 * log_file.h - UEFI debug log to the boot volume (FAT32), robust edition.
 *
 * Redirects all GNU-EFI Print() output to a log file (bridge-debug.log) on
 * the same FAT32 volume the app was booted from, while still mirroring it to
 * the console. This lets a debug run on real hardware be captured to a file
 * that can be read back on a PC, instead of relying on screen-scraping a
 * scrolling console.
 *
 * ROBUSTNESS (this revision):
 *   - Logging is enabled ONLY when the boot device is verified to be a
 *     removable USB drive AND it is the SAME drive the app was booted from
 *     (resolved via EFI_LOADED_IMAGE_PROTOCOL on our own image handle).
 *   - The console wrapper's OutputString is marked __attribute__((ms_abi))
 *     so it is correct regardless of whether it is invoked by our own
 *     SysV-ABI Print() or by firmware (MS x64 ABI). The previous revision
 *     used a plain EFIAPI (SysV) handler, which corrupted arguments when
 *     firmware called it and caused a #PF.
 *   - The wrapper validates its String pointer and guards against
 *     re-entrancy before touching the file.
 *   - On ANY failure the app continues with console-only output (never
 *     crashes, never blocks boot).
 *
 * Usage:
 *   - Call uefi_log_init(image) as the FIRST thing after InitializeLib() in
 *     efi_main, so every subsequent Print() is captured.
 *   - Call uefi_log_flush() before returning from efi_main so the file is
 *     flushed to disk.
 */

#ifndef LOG_FILE_H
#define LOG_FILE_H

#include <efi.h>

/* Open (or create) bridge-debug.log on the boot volume and install a console
 * wrapper so all Print() output is mirrored to the file. Returns EFI_SUCCESS
 * on success; on failure the app continues with console-only output. */
EFI_STATUS uefi_log_init(EFI_HANDLE image);

/* Flush the log file to disk. Safe to call any time; no-op if not open. */
void uefi_log_flush(void);

#endif /* LOG_FILE_H */
