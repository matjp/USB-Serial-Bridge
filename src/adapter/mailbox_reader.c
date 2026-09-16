/*
 * mailbox_reader.c - O1: Mailbox reader (consumer).
 *
 * Runs on core 0 inside the OS. Drains the two cross-core mailboxes
 * (keyboard and mouse, on separate rings; lock-free single-producer/
 * single-consumer; consumer checks tail != head) and produces the virtual
 * PS/2 byte streams for O2.
 */

#include <efi.h>
#include <mailbox.h>

#include "adapter.h"

/* Internal reassembled PS/2 byte streams produced by O1, consumed by O2.
 * O1 is OS-independent: it only preserves the byte streams in order; O2 does
 * the semantic interpretation (Set 1 scancodes + 3-byte mouse packets). */
typedef struct {
    UINT8  bytes[MAILBOX_RING_SIZE];
    UINTN  count;
} ADAPTER_STREAM;

/* Exposed via extern for O2 (input_inject.c). Keyboard and mouse are kept on
 * separate streams (mirroring the separate mailbox rings). */
ADAPTER_STREAM g_adapter_kbd_stream;
ADAPTER_STREAM g_adapter_mouse_stream;

/* Drain one mailbox into one stream. */
static void
drain_one(MAILBOX *mb, ADAPTER_STREAM *stream)
{
    UINT8 byte;

    /* Start a fresh batch for O2. */
    stream->count = 0;

    /* Drain until the ring is empty. The producer can never have more than
     * MAILBOX_RING_SIZE bytes pending (the ring holds at most that many), so
     * the buffer - also sized to MAILBOX_RING_SIZE - can hold a full drain;
     * the capacity check is defensive and never corrupts the buffer. */
    while (mailbox_read(mb, &byte)) {
        if (stream->count < MAILBOX_RING_SIZE)
            stream->bytes[stream->count++] = byte;
    }
}

void
adapter_drain_mailbox(MAILBOX *kbd_mb, MAILBOX *mouse_mb)
{
    if (kbd_mb)
        drain_one(kbd_mb, &g_adapter_kbd_stream);
    if (mouse_mb)
        drain_one(mouse_mb, &g_adapter_mouse_stream);
}
