/*
 * adapter.h - Internal interface for the per-OS input read (Phase 2).
 *
 * The OS's PS/2 driver reads the virtual 8042 port region (O1) and feeds the
 * virtual PS/2 byte stream into the OS's existing input path. The OS reuses
 * its existing Set 1 decoder and 3-byte mouse-packet parser unchanged. See
 * docs/architecture.md section 7.2.
 */

#ifndef ADAPTER_H
#define ADAPTER_H

#include <efi.h>
#include <virtual_ps2.h>

/* O1: Read the virtual 8042 port region (status/data, read-and-clear) and
 * feed the byte to the OS's existing KBD/mouse handler. */
void adapter_read_virtual_ps2(void);

#endif /* ADAPTER_H */
