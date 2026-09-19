/*
 * bridge_debug.c - Persistent virtual debug serial for the bridge core.
 *
 * Implements the writer side of the virtual debug serial (see
 * bridge_debug.h). The bridge writes NUL-terminated diagnostic lines into a
 * fixed ring buffer in the reserved region; a reader (BSP harness now, OS
 * later) drains it. Debug builds only (BRIDGE_DEBUG).
 *
 * The buffer is a ring of fixed-size line slots. A monotonically increasing
 * write sequence number lets a reader detect wrap and ordering. The bridge
 * writes complete lines only, so a reader never sees a torn line.
 */

#include <efi.h>
#include <efistdarg.h>

#include "bridge_debug.h"

#ifdef BRIDGE_DEBUG

/* ------------------------------------------------------------------ */
/* Minimal string helpers (ISO C99 freestanding, no std headers).      */
/* ------------------------------------------------------------------ */

static UINTN
dbg_strlen(const CHAR8 *s)
{
    UINTN n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

/* Write an unsigned value in the given base (10 or 16) into buf, returning
 * the number of characters written (not NUL-terminated). */
static UINTN
dbg_utoa(UINT32 value, UINT32 base, CHAR8 *buf)
{
    CHAR8 tmp[16];
    UINTN i = 0, j = 0;
    static const CHAR8 digits[] = "0123456789abcdef";

    if (value == 0)
        tmp[i++] = '0';
    while (value != 0) {
        tmp[i++] = digits[value % base];
        value /= base;
    }
    while (i > 0)
        buf[j++] = tmp[--i];
    return j;
}

/* ------------------------------------------------------------------ */
/* Ring buffer writer.                                                 */
/* ------------------------------------------------------------------ */

/* Initialize the ring buffer. Called once by the UEFI app after the
 * reserved page is zeroed, before the bridge is started. */
void
bridge_debug_init(void)
{
    volatile BRIDGE_DEBUG_HDR *hdr;
    volatile UINT8 *p;
    UINTN i;

    hdr = (volatile BRIDGE_DEBUG_HDR *)(UINTN)BRIDGE_DEBUG_BUF_ADDR;

    /* Zero the whole buffer (header + line slots). */
    p = (volatile UINT8 *)(UINTN)BRIDGE_DEBUG_BUF_ADDR;
    for (i = 0; i < BRIDGE_DEBUG_BUF_SIZE; i++)
        p[i] = 0;

    hdr->magic = BRIDGE_DEBUG_MAGIC;
    hdr->write_seq = 0;
    hdr->next_slot = 0;
    hdr->reserved = 0;
}

/* Write one NUL-terminated line into the ring buffer. */
void
bridge_debug_puts(const CHAR8 *line)
{
    volatile BRIDGE_DEBUG_HDR *hdr;
    volatile CHAR8 *slot;
    UINTN len, i, slot_idx;

    hdr = (volatile BRIDGE_DEBUG_HDR *)(UINTN)BRIDGE_DEBUG_BUF_ADDR;

    /* If the buffer is not present (not reserved/zeroed), bail. */
    if (hdr->magic != BRIDGE_DEBUG_MAGIC)
        return;

    len = dbg_strlen(line);
    if (len >= BRIDGE_DEBUG_LINE_LEN)
        len = BRIDGE_DEBUG_LINE_LEN - 1;

    slot_idx = hdr->next_slot;
    slot = (volatile CHAR8 *)(UINTN)(BRIDGE_DEBUG_BUF_ADDR +
                                     sizeof(BRIDGE_DEBUG_HDR) +
                                     slot_idx * BRIDGE_DEBUG_LINE_LEN);

    /* Copy the line (bounded) and NUL-terminate. */
    for (i = 0; i < len; i++)
        slot[i] = line[i];
    slot[len] = '\0';

    /* Advance the ring. Publish the line before bumping the sequence so a
     * reader sees a complete line. */
    __asm__ __volatile__("mfence" ::: "memory");
    hdr->write_seq++;
    hdr->next_slot = (slot_idx + 1) % BRIDGE_DEBUG_LINE_COUNT;
}

/* Format a line into a small stack buffer and write it. Supports a minimal
 * subset: %s (CHAR8*), %d/%u (UINT32), %x (UINT32 hex), %p (pointer). */
void
bridge_debug_printf(const CHAR8 *fmt, ...)
{
    CHAR8 buf[BRIDGE_DEBUG_LINE_LEN];
    CHAR8 num[16];
    UINTN out = 0;
    UINTN i = 0;
    va_list ap;

    va_start(ap, fmt);

    while (fmt[i] != '\0' && out < BRIDGE_DEBUG_LINE_LEN - 1) {
        if (fmt[i] != '%') {
            buf[out++] = fmt[i++];
            continue;
        }
        i++;   /* skip '%' */
        switch (fmt[i]) {
        case 's': {
            const CHAR8 *s = va_arg(ap, const CHAR8 *);
            if (s == NULL)
                s = (const CHAR8 *)"(null)";
            while (*s != '\0' && out < BRIDGE_DEBUG_LINE_LEN - 1)
                buf[out++] = *s++;
            i++;
            break;
        }
        case 'd':
        case 'u': {
            UINT32 v = va_arg(ap, UINT32);
            UINTN n = dbg_utoa(v, 10, num);
            UINTN k;
            for (k = 0; k < n && out < BRIDGE_DEBUG_LINE_LEN - 1; k++)
                buf[out++] = num[k];
            i++;
            break;
        }
        case 'x': {
            UINT32 v = va_arg(ap, UINT32);
            UINTN n = dbg_utoa(v, 16, num);
            UINTN k;
            for (k = 0; k < n && out < BRIDGE_DEBUG_LINE_LEN - 1; k++)
                buf[out++] = num[k];
            i++;
            break;
        }
        case 'p': {
            UINTN v = (UINTN)va_arg(ap, void *);
            UINTN n = dbg_utoa((UINT32)v, 16, num);
            UINTN k;
            buf[out++] = '0';
            buf[out++] = 'x';
            for (k = 0; k < n && out < BRIDGE_DEBUG_LINE_LEN - 1; k++)
                buf[out++] = num[k];
            i++;
            break;
        }
        case '%':
            buf[out++] = '%';
            i++;
            break;
        default:
            buf[out++] = '%';
            i++;
            break;
        }
    }

    va_end(ap);

    buf[out] = '\0';
    bridge_debug_puts(buf);
}

#endif /* BRIDGE_DEBUG */
