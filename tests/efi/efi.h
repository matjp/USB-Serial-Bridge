/*
 * efi.h - Host-side stand-in for the GNU-EFI <efi.h> header.
 *
 * The Layer 0 modules do `#include <efi.h>`. On the host test build we put
 * this directory (tests/efi) on the include path BEFORE the real GNU-EFI
 * include dir, so <efi.h> resolves here instead of /usr/include/efi/efi.h.
 * This file just pulls in the type shim.
 *
 * This is ONLY for the host test harness. The firmware build uses the real
 * GNU-EFI headers.
 */

#ifndef EFI_EFI_H
#define EFI_EFI_H

#include "efi_shim.h"

#endif /* EFI_EFI_H */
