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

void
bridge_write_mailbox(MAILBOX *mb)
{
    /* TODO(Firmware Coder):
     *   1. Frame the translated PS/2 byte stream (keyboard make/break
     *      sequences, mouse 3-byte packets).
     *   2. Write each byte via mailbox_write(mb, byte). */
    (void)mb;
}
