/*
 * mailbox_reader.c - O1: Mailbox reader (consumer).
 *
 * Runs on core 0 inside the OS. Drains the cross-core mailbox (lock-free
 * single-producer/single-consumer ring; consumer checks tail != head) and
 * produces the virtual PS/2 byte stream for O2.
 *
 * NOTE: This is a scaffold. The drain loop and byte-stream reassembly are
 * filled in by the Firmware Coder (see docs/architecture.md section 7.2,
 * module O1).
 */

#include <efi.h>
#include <mailbox.h>

#include "adapter.h"

/* Internal reassembled PS/2 byte stream produced by O1, consumed by O2.
 * O1 is OS-independent: it only preserves the byte stream in order; O2 does
 * the semantic interpretation (Set 1 scancodes + 3-byte mouse packets). */
typedef struct {
    UINT8  bytes[MAILBOX_RING_SIZE];
    UINTN  count;
} ADAPTER_STREAM;

/* Exposed via extern for O2 (input_inject.c). */
ADAPTER_STREAM g_adapter_stream;

void
adapter_drain_mailbox(MAILBOX *mb)
{
    UINT8 byte;

    /* Start a fresh batch for O2. */
    g_adapter_stream.count = 0;

    /* Drain until the ring is empty. The producer can never have more than
     * MAILBOX_RING_SIZE bytes pending (the ring holds at most that many), so
     * the buffer - also sized to MAILBOX_RING_SIZE - can hold a full drain;
     * the capacity check is defensive and never corrupts the buffer. */
    while (mailbox_read(mb, &byte)) {
        if (g_adapter_stream.count < MAILBOX_RING_SIZE)
            g_adapter_stream.bytes[g_adapter_stream.count++] = byte;
    }
}
