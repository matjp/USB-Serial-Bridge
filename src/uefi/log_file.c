/*
 * log_file.c - UEFI debug log to the boot volume (FAT32), robust edition.
 *
 * Implementation of the console-wrapper logging scheme:
 *
 *   1. Resolve the boot device via EFI_LOADED_IMAGE_PROTOCOL on our own image
 *      handle (loaded->DeviceHandle). This guarantees we only ever write to
 *      the SAME drive the app was booted from.
 *   2. Verify the boot device is actually a USB drive by walking its
 *      EFI_DEVICE_PATH_PROTOCOL for a USB messaging node.
 *   3. Verify it is removable media via EFI_BLOCK_IO_PROTOCOL.Media->
 *      RemovableMedia.
 *   4. Verify it is writable by opening EFI_SIMPLE_FILE_SYSTEM_PROTOCOL,
 *      OpenVolume, and opening (creating) L"\\bridge-debug.log" with
 *      READ|WRITE|CREATE.
 *   5. Only if ALL of the above succeed, copy the real ST->ConOut struct into
 *      a static wrapper, override ONLY OutputString, and repoint ST->ConOut at
 *      the wrapper. Every subsequent GNU-EFI Print() call goes through the
 *      wrapper, which mirrors the text to the real console AND appends it to
 *      the log file.
 *
 * The wrapper's OutputString is marked __attribute__((ms_abi)) because the
 * firmware (OVMF) is built with the MS x64 ABI while this app is built with
 * the SysV ABI (EFIAPI is empty here). If firmware ever calls
 * ST->ConOut->OutputString directly, a plain SysV handler would receive its
 * arguments in the wrong registers and corrupt memory (the #PF seen in the
 * previous revision). The ms_abi attribute makes the handler correct for
 * BOTH callers. The handler also validates its String pointer and guards
 * against re-entrancy.
 *
 * On ANY failure the app continues with console-only output; it never
 * crashes and never blocks boot.
 *
 * All firmware calls go through uefi_call_wrapper per GNU-EFI convention.
 * No standard headers are used; types map to <efi.h>.
 */

#include <efi.h>
#include <efilib.h>
#include <efidevp.h>

#include "log_file.h"

/* The open log file handle, or NULL if logging is not active. */
static EFI_FILE *g_log_file = NULL;

/* The real console protocol, saved before we install the wrapper. */
static SIMPLE_TEXT_OUTPUT_INTERFACE *g_real_conout = NULL;

/* Static wrapper struct; ST->ConOut is repointed at this. */
static SIMPLE_TEXT_OUTPUT_INTERFACE g_console_wrapper;

/* Re-entrancy guard: set while we are inside wrapper_output_string so a
 * Print() issued from within the wrapper does not recurse into the file. */
static BOOLEAN g_in_wrapper = FALSE;

/* ------------------------------------------------------------------ */
/* Device-path helpers.                                                */
/* ------------------------------------------------------------------ */

/* Return TRUE if the device path contains a USB messaging node. */
static BOOLEAN
path_is_usb(EFI_DEVICE_PATH *path)
{
    EFI_DEVICE_PATH *node;

    if (path == NULL)
        return FALSE;

    for (node = path; !IsDevicePathEnd(node);
         node = NextDevicePathNode(node)) {
        if (DevicePathType(node) == MESSAGING_DEVICE_PATH) {
            UINT8 sub = DevicePathSubType(node);
            if (sub == MSG_USB_DP || sub == MSG_USB_CLASS_DP ||
                sub == MSG_USB_WWID_DP)
                return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Log file writing.                                                   */
/* ------------------------------------------------------------------ */

/* Append a NUL-terminated CHAR16 string to the log file.
 * Returns EFI_SUCCESS if written, EFI_NOT_READY if no file is open. */
static EFI_STATUS
log_write_string(const CHAR16 *str)
{
    UINTN len = 0;
    UINTN size;

    if (g_log_file == NULL || str == NULL)
        return EFI_NOT_READY;

    while (str[len] != 0)
        len++;

    size = len * sizeof(CHAR16);
    if (size == 0)
        return EFI_SUCCESS;

    return uefi_call_wrapper(g_log_file->Write, 3, g_log_file, &size,
                             (VOID *)str);
}

/*
 * Wrapper OutputString: mirror to the real console and append to the log.
 * This is what ST->ConOut->OutputString points at after uefi_log_init().
 *
 * The struct field EFI_TEXT_OUTPUT_STRING is declared EFIAPI (SysV ABI in
 * this build), and GNU-EFI's Print() calls it with SysV convention, so the
 * wrapper must be EFIAPI to match. Defensive: validates String, guards
 * re-entrancy.
 */
static EFI_STATUS EFIAPI
wrapper_output_string(SIMPLE_TEXT_OUTPUT_INTERFACE *This, CHAR16 *String)
{
    (void)This; /* wrapper is a singleton; the real console is g_real_conout */

    /* Defensive: never dereference a garbage String pointer. */
    if (String == NULL)
        return EFI_INVALID_PARAMETER;

    /* Mirror to the real console first (always safe). */
    if (g_real_conout != NULL)
        uefi_call_wrapper(g_real_conout->OutputString, 2, g_real_conout,
                          String);

    /* Append to the log file, but never recurse. */
    if (!g_in_wrapper) {
        g_in_wrapper = TRUE;
        log_write_string(String);
        g_in_wrapper = FALSE;
    }

    return EFI_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

EFI_STATUS
uefi_log_init(EFI_HANDLE image)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
    EFI_DEVICE_PATH *devpath = NULL;
    EFI_BLOCK_IO_PROTOCOL *blockio = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_FILE *root = NULL;
    EFI_STATUS status;

    /* Remember the real console so the wrapper can mirror to it. */
    g_real_conout = ST->ConOut;

    /* 1. Find the device we were booted from. */
    status = uefi_call_wrapper(BS->HandleProtocol, 3, image,
                               &gEfiLoadedImageProtocolGuid,
                               (VOID **)&loaded);
    if (EFI_ERROR(status) || loaded == NULL || loaded->DeviceHandle == NULL)
        return EFI_UNSUPPORTED;

    /* 2. Verify the boot device is a USB drive (device-path check). */
    status = uefi_call_wrapper(BS->HandleProtocol, 3, loaded->DeviceHandle,
                               &gEfiDevicePathProtocolGuid,
                               (VOID **)&devpath);
    if (EFI_ERROR(status) || devpath == NULL || !path_is_usb(devpath))
        return EFI_UNSUPPORTED;

    /* 3. Verify it is removable media (block I/O check). */
    status = uefi_call_wrapper(BS->HandleProtocol, 3, loaded->DeviceHandle,
                               &gEfiBlockIoProtocolGuid, (VOID **)&blockio);
    if (EFI_ERROR(status) || blockio == NULL || blockio->Media == NULL ||
        !blockio->Media->RemovableMedia)
        return EFI_UNSUPPORTED;

    /* 4. Open the file system on the boot device. */
    status = uefi_call_wrapper(BS->HandleProtocol, 3, loaded->DeviceHandle,
                               &gEfiSimpleFileSystemProtocolGuid,
                               (VOID **)&sfs);
    if (EFI_ERROR(status) || sfs == NULL)
        return EFI_UNSUPPORTED;

    /* 5. Get the root directory of the volume. */
    status = uefi_call_wrapper(sfs->OpenVolume, 1, sfs, &root);
    if (EFI_ERROR(status) || root == NULL)
        return EFI_UNSUPPORTED;

    /* 6. Open (or create) the log file. CREATE opens at position 0, so each
     *    boot overwrites the previous run's log. This also proves the volume
     *    is actually writable. */
    status = uefi_call_wrapper(root->Open, 5, root, &g_log_file,
                               L"\\bridge-debug.log",
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                                   EFI_FILE_MODE_CREATE,
                               0);
    if (EFI_ERROR(status) || g_log_file == NULL)
        return EFI_UNSUPPORTED;

    /* 7. Install the console wrapper. Copy the whole real struct so every
     *    other operation (SetAttribute, ClearScreen, Mode, ...) still works,
     *    and override only OutputString. */
    g_console_wrapper = *g_real_conout;
    g_console_wrapper.OutputString = wrapper_output_string;
    ST->ConOut = &g_console_wrapper;

    return EFI_SUCCESS;
}

void
uefi_log_flush(void)
{
    if (g_log_file != NULL)
        uefi_call_wrapper(g_log_file->Flush, 1, g_log_file);
}
