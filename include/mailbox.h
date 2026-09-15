/*
 * mailbox.h - Cross-core mailbox ABI (the OS-agnostic "serial keyboard/mouse
 * interface"). See docs/architecture.md section 6.
 *
 * This is the formal interface contract between the bridge (producer, on the
 * highest core) and the per-OS input adapter (consumer, on core 0). It is
 * fixed and versioned - the bridge and the adapter must agree on it, but
 * neither depends on the other's OS.
 *
 * Lock-free single-producer / single-consumer ring buffer. x86 is
 * cache-coherent (MESI), so the shared memory is coherent. The producer uses
 * mfence() before advancing head; the consumer checks tail != head.
 */

#ifndef MAILBOX_H
#define MAILBOX_H

#include <efi.h>

#define MAILBOX_RING_SIZE  256
#define MAILBOX_ABI_VERSION 1

typedef struct {
    volatile UINT32 head;   /* producer (bridge core) write index */
    volatile UINT32 tail;   /* consumer (input adapter) read index */
    volatile UINT8  ring[MAILBOX_RING_SIZE];
} MAILBOX;

/* Byte-stream semantics (virtual PS/2):
 *   - Keyboard: PS/2 Set 1 scancodes, make and break (0xE0-prefixed extended
 *     codes included). The OS reuses its existing Set 1 decoder.
 *   - Mouse: 3-byte packets [buttons, dx, dy] (two's-complement deltas),
 *     matching the standard PS/2 mouse packet the OS already parses.
 */

/* Producer (bridge core): write one byte, then mfence, then head++. */
static inline void
mailbox_write(MAILBOX *mb, UINT8 byte)
{
    mb->ring[mb->head % MAILBOX_RING_SIZE] = byte;
    __asm__ __volatile__("mfence" ::: "memory");
    mb->head++;
}

/* Consumer (input adapter, core 0): returns 1 if a byte was read, else 0. */
static inline int
mailbox_read(MAILBOX *mb, UINT8 *out)
{
    if (mb->tail == mb->head)
        return 0;
    *out = mb->ring[mb->tail % MAILBOX_RING_SIZE];
    mb->tail++;
    return 1;
}

/* Initialize a mailbox (head = tail = 0). */
void mailbox_init(MAILBOX *mb);

/* Publish the mailbox base address at the fixed pointer location. */
void mailbox_publish(MAILBOX *mb);

/* Read the published mailbox base address (returns NULL if not published). */
MAILBOX *mailbox_lookup(void);

#endif /* MAILBOX_H */
