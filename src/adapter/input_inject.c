/*
 * input_inject.c - O2: Input injection into the OS input path.
 *
 * Injects the virtual PS/2 byte stream into the OS's existing input path.
 * The OS reuses its existing Set 1 decoder and 3-byte mouse-packet parser
 * unchanged - the adapter only feeds bytes into the same queue the OS's
 * native PS/2 driver would use.
 *
 * NOTE: This is a scaffold. The OS-specific injection hook is filled in by
 * the Firmware Coder (see docs/architecture.md section 7.2, module O2).
 */

#include <efi.h>

#include "adapter.h"

void
adapter_inject_input(void)
{
    /* TODO(Firmware Coder):
     *   1. Feed the reassembled PS/2 byte stream into the OS's existing
     *      input queue (the same queue its native PS/2 driver uses).
     *   2. No changes to the OS decoder/parser - it is reused unchanged. */
}
