/*
 * log_file.c - UEFI debug log to the boot volume (FAT32).
 *
 * Implementation of the console-wrapper logging scheme:
 *
 *   1. Resolve the boot device via EFI_LOADED_IMAGE_PROTOCOL on our own image
 *      handle (loaded->DeviceHandle).
 *   2. Open EFI_SIMPLE_FILE_SYSTEM_PROTOCOL on that device and OpenVolume to
 *      get the root directory.
 *   3. Delete any existing L"\\bridge-debug.log" so the file always starts
 *      empty (guaranteed overwrite), then open it with READ|WRITE|CREATE.
 *   4. Copy the real ST->ConOut struct into a static wrapper, override only
 *      OutputString, and repoint ST->ConOut at the wrapper. Every subsequent
 *      GNU-EFI Print() call goes through the wrapper, which mirrors the text
 *      to the real console AND appends it to the log file.
 *
 * DEBUG-ONLY: the entire implementation is guarded by #ifdef BRIDGE_DEBUG.
 * In the normal build uefi_log_init()/uefi_log_flush() are no-ops, so the
 * release image never opens or writes to the boot volume.
 *
 * All firmware calls go through uefi_call_wrapper per GNU-EFI convention.
 * No standard headers are used; types map to <efi.h>.
 */

#include <efi.h>
#include <efilib.h>
#include "log_file.h"

#ifdef BRIDGE_DEBUG

/* The open log file handle, or NULL if logging is not active. */
static EFI_FILE *g_log_file = NULL;

/* The real console protocol, saved before we install the wrapper. */
static SIMPLE_TEXT_OUTPUT_INTERFACE *g_real_conout = NULL;

/* Static wrapper struct; ST->ConOut is repointed at this. */
static SIMPLE_TEXT_OUTPUT_INTERFACE g_console_wrapper;

/*
 * Append a NUL-terminated CHAR16 string to the log file.
 * Returns EFI_SUCCESS if written, EFI_NOT_READY if no file is open.
 */
static EFI_STATUS
log_write_string(const CHAR16 *str)
{
    UINTN len = 0;
    UINTN size;

    if (g_log_file == NULL)
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
 */
static EFI_STATUS EFIAPI
wrapper_output_string(SIMPLE_TEXT_OUTPUT_INTERFACE *This, CHAR16 *String)
{
    (void)This; /* wrapper is a singleton; the real console is g_real_conout */

    if (g_real_conout != NULL)
        uefi_call_wrapper(g_real_conout->OutputString, 2, g_real_conout,
                          String);
    log_write_string(String);
    return EFI_SUCCESS;
}

EFI_STATUS
uefi_log_init(EFI_HANDLE image)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
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

    /* 2. Open the file system on the boot device. */
    status = uefi_call_wrapper(BS->HandleProtocol, 3, loaded->DeviceHandle,
                               &gEfiSimpleFileSystemProtocolGuid,
                               (VOID **)&sfs);
    if (EFI_ERROR(status) || sfs == NULL)
        return EFI_UNSUPPORTED;

    /* 3. Get the root directory of the volume. */
    status = uefi_call_wrapper(sfs->OpenVolume, 1, sfs, &root);
    if (EFI_ERROR(status) || root == NULL)
        return EFI_UNSUPPORTED;

    /* 4. Delete any existing log so the file always starts empty (guaranteed
     *    overwrite - a plain CREATE opens at position 0 but does NOT truncate,
     *    so a shorter new log would leave stale trailing bytes). Ignore the
     *    error if the file does not exist yet. */
    {
        EFI_FILE *old = NULL;
        status = uefi_call_wrapper(root->Open, 5, root, &old,
                                   L"\\bridge-debug.log",
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                                   0);
        if (!EFI_ERROR(status) && old != NULL)
            uefi_call_wrapper(old->Delete, 1, old);
    }

    /* 5. Create a fresh log file. */
    status = uefi_call_wrapper(root->Open, 5, root, &g_log_file,
                               L"\\bridge-debug.log",
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                                   EFI_FILE_MODE_CREATE,
                               0);
    if (EFI_ERROR(status) || g_log_file == NULL)
        return EFI_UNSUPPORTED;

    /* 6. Install the console wrapper. Copy the whole real struct so every
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

#else /* !BRIDGE_DEBUG */

/* Normal (release) build: logging is compiled out entirely. These no-ops keep
 * the symbols defined so main.c links, but the release image never opens or
 * writes to the boot volume. */
EFI_STATUS
uefi_log_init(EFI_HANDLE image)
{
    (void)image;
    return EFI_UNSUPPORTED;
}

void
uefi_log_flush(void)
{
}

#endif /* BRIDGE_DEBUG */
