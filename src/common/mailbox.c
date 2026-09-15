/*
 * mailbox.c - Shared mailbox helpers.
 *
 * The mailbox itself is a lock-free single-producer/single-consumer ring
 * (see include/mailbox.h). This file provides the shared initialization and
 * the mailbox base-address publication used by both the UEFI app (which
 * allocates and reserves the region) and the input adapter (which reads it).
 *
 * The mailbox base address is published by the boot-time UEFI app at a fixed
 * physical address; each OS adapter reads it once at startup.
 */

#include <mailbox.h>

/* Fixed physical address where the UEFI app publishes the mailbox base.
 * Chosen to be well above typical low memory; the app reserves this page. */
#define MAILBOX_PTR_ADDR  0x10000000ULL

void
mailbox_init(MAILBOX *mb)
{
    mb->head = 0;
    mb->tail = 0;
}

/* Publish the mailbox base address at the fixed pointer location. */
void
mailbox_publish(MAILBOX *mb)
{
    volatile UINT64 *slot = (volatile UINT64 *)MAILBOX_PTR_ADDR;
    *slot = (UINT64)(UINTN)mb;
}

/* Read the published mailbox base address (returns NULL if not published). */
MAILBOX *
mailbox_lookup(void)
{
    volatile UINT64 *slot = (volatile UINT64 *)MAILBOX_PTR_ADDR;
    return (MAILBOX *)(UINTN)*slot;
}
