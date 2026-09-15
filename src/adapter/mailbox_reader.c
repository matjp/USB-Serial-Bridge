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

void
adapter_drain_mailbox(MAILBOX *mb)
{
    UINT8 byte;

    /* TODO(Firmware Coder):
     *   1. Drain available bytes via mailbox_read(mb, &byte).
     *   2. Reassemble the virtual PS/2 byte stream (keyboard make/break
     *      sequences, mouse 3-byte packets).
     *   3. Hand the stream to O2 for injection. */
    (void)mb;
    (void)byte;
}
