/*
 * mailbox_writer.c - B4: Mailbox writer (producer).
 *
 * Writes the translated PS/2 byte stream into the cross-core mailbox. The
 * mailbox is a lock-free single-producer/single-consumer ring; the producer
 * uses mfence() before advancing head (see include/mailbox.h).
 *
 * NOTE: This is a scaffold. The byte-stream framing (keyboard make/break
 * sequences, mouse 3-byte packets) is filled in by the Firmware Coder
 * (see docs/architecture.md section 7.1, module B4).
 */

#include <efi.h>
#include <mailbox.h>

#include "bridge.h"
#include "hid_event.h"

void
bridge_write_mailbox(MAILBOX *mb)
{
    UINTN i;

    for (i = 0; i < g_ps2_stream.count; i++) {
        /* Overflow policy (documented): the mailbox is a lock-free
         * single-producer/single-consumer ring. If the producer would lap the
         * consumer ((head - tail) >= MAILBOX_RING_SIZE), the ring is full.
         * The consumer drains continuously, so this is unlikely; when it does
         * happen we DROP the pending byte rather than block or corrupt the
         * ring. Dropping stale input is preferable to stalling the bridge
         * core or overwriting unread data. */
        if ((mb->head - mb->tail) >= MAILBOX_RING_SIZE)
            continue;   /* ring full: drop this byte */
        mailbox_write(mb, g_ps2_stream.bytes[i]);
    }

    /* Consume the PS/2 output stream. */
    g_ps2_stream.count = 0;
}
