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

/* Fixed physical addresses where the UEFI app publishes the mailbox base
 * addresses. Chosen to be well above typical low memory; the app reserves
 * these pages. The keyboard and mouse mailboxes are separate rings (see
 * include/mailbox.h). The USB_TOPOLOGY pointer lives at 0x10000008 (see
 * src/bridge/usb_topology.h), so the mouse mailbox pointer is placed at
 * 0x10000010 to avoid collision. */
#define MAILBOX_KBD_PTR_ADDR   0x10000000ULL
#define MAILBOX_MOUSE_PTR_ADDR 0x10000010ULL

void
mailbox_init(MAILBOX *mb)
{
    mb->head = 0;
    mb->tail = 0;
}

/* Publish the keyboard mailbox base address at the fixed pointer location. */
void
mailbox_publish_kbd(MAILBOX *mb)
{
    volatile UINT64 *slot = (volatile UINT64 *)MAILBOX_KBD_PTR_ADDR;
    *slot = (UINT64)(UINTN)mb;
}

/* Publish the mouse mailbox base address at the fixed pointer location. */
void
mailbox_publish_mouse(MAILBOX *mb)
{
    volatile UINT64 *slot = (volatile UINT64 *)MAILBOX_MOUSE_PTR_ADDR;
    *slot = (UINT64)(UINTN)mb;
}

/* Read the published keyboard mailbox base address (NULL if not published). */
MAILBOX *
mailbox_lookup_kbd(void)
{
    volatile UINT64 *slot = (volatile UINT64 *)MAILBOX_KBD_PTR_ADDR;
    return (MAILBOX *)(UINTN)*slot;
}

/* Read the published mouse mailbox base address (NULL if not published). */
MAILBOX *
mailbox_lookup_mouse(void)
{
    volatile UINT64 *slot = (volatile UINT64 *)MAILBOX_MOUSE_PTR_ADDR;
    return (MAILBOX *)(UINTN)*slot;
}
