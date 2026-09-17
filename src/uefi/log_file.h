/*
 * log_file.h - UEFI debug log to the boot volume (FAT32).
 *
 * Redirects all GNU-EFI Print() output to a log file (bridge-debug.log) on
 * the same FAT32 volume the app was booted from, while still mirroring it to
 * the console. This lets a debug run on real hardware be captured to a file
 * that can be read back on a PC, instead of relying on screen-scraping a
 * scrolling console.
 *
 * DEBUG-ONLY: this module is compiled in ONLY for the debug build
 * (-DBRIDGE_DEBUG). In the normal build the functions compile to no-ops, so
 * the release image never touches the boot volume.
 *
 * OVERWRITE: each boot deletes any existing bridge-debug.log and creates a
 * fresh one, so the file always reflects exactly the current run.
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
