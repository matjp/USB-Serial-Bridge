/*
 * usb_topology.h - Recorded fixed USB topology (shared U1 -> B1).
 *
 * Captured once by U1 (usb_discovery.c) during Phase 1, before
 * ExitBootServices, and consumed by B1 (xhci.c) at runtime on the bridge
 * core. The USB_TOPOLOGY object itself lives in the reserved region (see
 * U3, mem_reserve.c) so the bridge core can read it after ExitBootServices.
 *
 * Fixed topology: exactly one keyboard and one mouse, no hotplug, no runtime
 * enumeration. See docs/implementation-spec.md section 1.1.
 */

#ifndef USB_TOPOLOGY_H
#define USB_TOPOLOGY_H

#include <efi.h>

/* Fixed physical address where U3 publishes the USB_TOPOLOGY base address.
 * Chosen to sit in the reserved pointer page (0x10000000..0x10000028) in
 * the reserved region. The OS never allocates over this fixed pointer page
 * (it is reserved by U3). */
#define USB_TOPOLOGY_PTR_ADDR  0x10000008ULL

/* One pre-discovered interrupt IN endpoint. */
typedef struct {
    UINT8  device_addr;   /* USB device address (1..127) */
    UINT8  endpoint;      /* endpoint number, low 4 bits = addr, bit7 = IN */
    UINT8  interval;      /* bInterval (in frames/microframes) */
    UINT16 max_packet;    /* wMaxPacketSize */
    UINT8  speed;         /* 0=full, 1=low, 2=high, 3=super (we only use low/full) */
    UINT8  port;          /* root-hub port number (1-based) the device is on */
} USB_ENDPOINT;

/* Fixed topology: exactly one keyboard and one mouse. */
typedef struct {
    USB_ENDPOINT kbd;            /* keyboard interrupt IN endpoint */
    USB_ENDPOINT mouse;          /* mouse interrupt IN endpoint */
    UINT32       xhci_mmio_base; /* XHCI MMIO base (BAR0), for B1 direct drive */
    UINT32       xhci_cap_len;   /* CAPLENGTH, for B1 register offsets */
} USB_TOPOLOGY;

/* Publish the USB_TOPOLOGY base address at the fixed pointer location.
 * Called by U3 after it copies the discovered topology into the reserved
 * region. */
static inline void
usb_topology_publish(USB_TOPOLOGY *topo)
{
    volatile UINT64 *slot = (volatile UINT64 *)USB_TOPOLOGY_PTR_ADDR;
    *slot = (UINT64)(UINTN)topo;
}

/* Read the published USB_TOPOLOGY base address (NULL if not published).
 * Called by B1 on the bridge core at runtime. */
static inline USB_TOPOLOGY *
usb_topology_lookup(void)
{
    volatile UINT64 *slot = (volatile UINT64 *)USB_TOPOLOGY_PTR_ADDR;
    return (USB_TOPOLOGY *)(UINTN)*slot;
}

#endif /* USB_TOPOLOGY_H */
