/*
 * mailbox_writer.c - B4: Mailbox writer (producer).
 *
 * Writes the translated PS/2 byte streams into the two cross-core mailboxes
 * (keyboard and mouse, on separate rings). Each mailbox is a lock-free
 * single-producer/single-consumer ring; the producer uses mfence() before
 * advancing head (see include/mailbox.h).
 */

#include <efi.h>
#include <mailbox.h>

#include "bridge.h"
#include "hid_event.h"

/* Write one stream to one mailbox, dropping bytes if the ring is full. */
static void
write_stream(MAILBOX *mb, const PS2_STREAM *stream)
{
    UINTN i;

    for (i = 0; i < stream->count; i++) {
        /* Overflow policy (documented): the mailbox is a lock-free
         * single-producer/single-consumer ring. If the producer would lap the
         * consumer ((head - tail) >= MAILBOX_RING_SIZE), the ring is full.
         * The consumer drains continuously, so this is unlikely; when it does
         * happen we DROP the pending byte rather than block or corrupt the
         * ring. Dropping stale input is preferable to stalling the bridge
         * core or overwriting unread data. */
        if ((mb->head - mb->tail) >= MAILBOX_RING_SIZE)
            continue;   /* ring full: drop this byte */
        mailbox_write(mb, stream->bytes[i]);
    }
}

void
bridge_write_mailbox(MAILBOX *kbd_mb, MAILBOX *mouse_mb)
{
    /* Keyboard scancodes -> keyboard ring; mouse packets -> mouse ring. */
    if (kbd_mb)
        write_stream(kbd_mb, &g_ps2_kbd_stream);
    if (mouse_mb)
        write_stream(mouse_mb, &g_ps2_mouse_stream);

    /* Consume the PS/2 output streams. */
    g_ps2_kbd_stream.count   = 0;
    g_ps2_mouse_stream.count = 0;
}
