/*
 * xhci.c - B1: XHCI driver for the two pre-discovered endpoints.
 *
 * Drives the single keyboard and mouse interrupt IN endpoints discovered at
 * boot (U1). XHCI >= 1.0 only (C6); no SuperSpeed support - keyboards and
 * mice are low/full-speed on USB 2.0 root-hub ports, so only two low/full-
 * speed interrupt IN endpoints are driven. No enumeration at runtime, no
 * hotplug, no interrupts - minimal periodic polling.
 *
 * The bridge drives the XHCI controller directly via MMIO (the UEFI USB
 * protocols are boot-time only and may not survive ExitBootServices). The
 * controller is reset and a minimal device/endpoint context is re-created
 * for the two known endpoints (per docs/implementation-spec.md section 2.1);
 * the bridge does NOT re-enumerate.
 *
 * NOTE: This is a hardware-validation (Layer 1) module. The register
 * programming follows the XHCI 1.x specification. The root-hub port number
 * in the slot context is recorded by U1 (usb_discovery.c) by scanning the
 * root-hub PORTSC registers and is read from the topology here.
 */

#include <efi.h>
#include <hid.h>

#include "bridge.h"
#include "usb_topology.h"
#include "xhci_fault.h"
#include "xhci_status.h"

/* Freestanding firmware: <efi.h> does not pull in <string.h>. Declare the
 * libc memset we use to clear the fault record (host build provides it). */
void *memset(void *s, int c, UINTN n);

/* ------------------------------------------------------------------ */
/* Raw HID report globals consumed by B2 (hid_parser.c).               */
/*                                                                     */
/* These are the single source of truth for the raw boot-protocol      */
/* reports. B1 (this file) owns them; B2 reads them and clears the     */
/* g_*_valid flags after consuming.                                    */
/* ------------------------------------------------------------------ */
HID_KBD_REPORT   g_raw_kbd;
HID_MOUSE_REPORT g_raw_mouse;
BOOLEAN g_kbd_valid   = FALSE;
BOOLEAN g_mouse_valid = FALSE;

/* ------------------------------------------------------------------ */
/* XHCI register offsets (relative to the appropriate base).           */
/* ------------------------------------------------------------------ */

/* Capability registers (relative to xhci_mmio_base). */
#define XHCI_CAP_HCIVERSION   0x00
#define XHCI_CAP_CAPLENGTH    0x00
#define XHCI_CAP_HCSPARAMS1   0x04
#define XHCI_CAP_HCSPARAMS2   0x08
#define XHCI_CAP_HCCPARAMS1   0x10
#define XHCI_CAP_DBOFF        0x14
#define XHCI_CAP_RTSOFF       0x18

/* Operational registers (relative to xhci_mmio_base + CAPLENGTH). */
#define XHCI_OP_USBCMD       0x00
#define XHCI_OP_USBSTS       0x04
#define XHCI_OP_PAGESIZE     0x08
#define XHCI_OP_CRCR         0x18
#define XHCI_OP_DCBAAP       0x30
#define XHCI_OP_CONFIG       0x38

/* USBCMD bits. */
#define USBCMD_RUN           0x00000001
#define USBCMD_HCRST         0x00000002
#define USBCMD_INTE          0x00000004

/* USBSTS bits. */
#define USBSTS_HCH           0x00000001
#define USBSTS_HSE           0x00000004
#define USBSTS_CNR           0x00001000

/* CRCR bits. */
#define CRCR_RCS             0x00000001
#define CRCR_CS              0x00000002
#define CRCR_CA              0x00000004
#define CRCR_CRR             0x00000008

/* Doorbell targets. */
#define DB_TARGET_COMMAND    0
#define DB_TARGET_EP0        1

/* TRB types. */
#define TRB_TYPE_NORMAL      1
#define TRB_TYPE_SETUP       2
#define TRB_TYPE_DATA        3
#define TRB_TYPE_STATUS      4
#define TRB_TYPE_LINK        6
#define TRB_TYPE_TRANSFER_EVENT 32
#define TRB_TYPE_COMMAND_COMPLETION 33

/* Completion codes. */
#define CC_SUCCESS           1
#define CC_SHORT_PACKET      13

/* Endpoint types (bEndpointContext EP Type field). */
#define EP_TYPE_INTERRUPT_IN 3

/* Slot context speed field. */
#define SLOT_SPEED_FULL      0
#define SLOT_SPEED_LOW       1

/* Command opcodes (bCommandOpcode in the command TRB). */
#define CMD_ENABLE_SLOT      0
#define CMD_ADDRESS_DEVICE   1
#define CMD_CONFIGURE_EP     2

/* ------------------------------------------------------------------ */
/* XHCI register block (subset needed).                                */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile UINT32 *cap;      /* capability registers (HCSPARAMS1, HCCPARAMS) */
    volatile UINT32 *op;       /* operational registers (CMD, STS, CRCR, DCBAAP) */
    volatile UINT32 *doorbell; /* doorbell array */
    volatile UINT32 *rt;       /* runtime registers (ERSTBA, ERDP) */
    UINT32 max_slots;          /* from HCSPARAMS1 */
    UINT32 max_eps;            /* from HCSPARAMS1 */
    UINT32 max_scratchpad;     /* from HCSPARAMS1 */
    UINT32 page_size;          /* from PAGESIZE */
} XHCI;

/* TRB (16 bytes). */
typedef struct {
    UINT32 field0;
    UINT32 field1;
    UINT32 field2;
    UINT32 field3;
} TRB;

/* Event Ring Segment Table entry (one segment). */
typedef struct {
    UINT32 seg_addr_low;
    UINT32 seg_addr_high;
    UINT32 seg_size;
    UINT32 reserved;
} ERST_SEG;

/* Slot context (32 bytes = 8 dwords). */
typedef struct {
    UINT32 dword[8];
} SLOT_CONTEXT;

/* Endpoint context (32 bytes = 8 dwords). */
typedef struct {
    UINT32 dword[8];
} EP_CONTEXT;

/* Device context: slot context + EP0 + the interrupt IN endpoint. */
typedef struct {
    SLOT_CONTEXT slot;
    EP_CONTEXT   ep0;
    EP_CONTEXT   ep_in;
} DEVICE_CONTEXT;

/* Transfer ring (a small ring of Normal TRBs).
 *
 * Each ring holds a single Normal TRB pointing at the endpoint's report
 * buffer. A periodic IN endpoint needs only one transfer in flight at a
 * time: after each completed transfer the software re-arms the TRB (flips
 * its cycle bit back to the producer's cycle) and rings the doorbell again.
 * A single TRB avoids the ring-wrap cycle-bit bookkeeping entirely. */
#define TR_RING_SIZE 1
typedef struct {
    TRB    trbs[TR_RING_SIZE];
    UINT32 enq;    /* enqueue index (always 0 for a single-TRB ring) */
    UINT32 cycle;  /* current cycle bit */
} TRANSFER_RING;

/* ------------------------------------------------------------------ */
/* Static XHCI structures (live in the bridge's reserved data region). */
/* ------------------------------------------------------------------ */

#define MAX_DEVICES 2

/* Command ring. */
#define CMD_RING_SIZE 16
static TRB    g_cmd_ring[CMD_RING_SIZE];
static UINT32 g_cmd_ring_enq;
static UINT32 g_cmd_ring_cycle;

/* Event ring. */
#define EVT_RING_SIZE 16
static TRB    g_evt_ring[EVT_RING_SIZE];
static UINT32 g_evt_ring_deq;
static UINT32 g_evt_ring_cycle;

/* ERST (one segment). */
static ERST_SEG g_erst[1];

/* DCBAA - device context base address array (entry 0 reserved). */
static UINT64 g_dcbaa[MAX_DEVICES + 1];

/* Device contexts (one per device). */
static DEVICE_CONTEXT g_dev_ctx[MAX_DEVICES];

/* Transfer rings (one per endpoint). */
static TRANSFER_RING g_tr_kbd;
static TRANSFER_RING g_tr_mouse;

/* Data buffers for the received reports. */
static UINT8 g_kbd_buf[HID_KBD_REPORT_SIZE];
static UINT8 g_mouse_buf[HID_MOUSE_REPORT_SIZE];

/* Fatal-fault flag: set when the controller is unusable; the bridge halts. */
static BOOLEAN g_xhci_fatal;

/* Debugging model: a structured fault record (see xhci_fault.h) that records
 * WHICH bring-up stage failed, WHAT the controller reported, and WHAT the
 * bridge was doing. Published in the reserved region so the Layer 1 harness
 * on core 0 can read it after the bridge halts. */
static XHCI_FAULT g_xhci_fault_rec;

/* One-time bring-up latch (file scope so the host test can reset it). */
static BOOLEAN g_xhci_initialized = FALSE;

#ifdef BRIDGE_DEBUG
/* Debug builds only: a success record capturing the ACTUAL XHCI hardware
 * state after bring-up, so the harness can print it for downstream
 * verification (see xhci_status.h). */
static XHCI_STATUS g_xhci_status;
#endif /* BRIDGE_DEBUG */

/* ------------------------------------------------------------------ */
/* Weak indirection points for the host fault-injection test.          */
/*                                                                     */
/* The host test (tests/test_xhci_fault.c) overrides these to supply a */
/* mock topology and to read the fault record WITHOUT touching the     */
/* fixed reserved-region pointer slots (0x10000008 / 0x10000018),      */
/* which are unmapped on the host. In the firmware build no strong     */
/* definitions exist, so the real lookups/publish below are used.      */
/* ------------------------------------------------------------------ */

/* Return the USB topology. Defaults to the reserved-region lookup. */
__attribute__((weak)) const USB_TOPOLOGY *
usb_topology_get(void)
{
    return usb_topology_lookup();
}

/* Publish the fault record. Defaults to the reserved-region publish. */
__attribute__((weak)) void
xhci_fault_publish_rec(void)
{
    xhci_fault_publish(&g_xhci_fault_rec);
}

/* Return the fault record (NULL if no fault). Defaults to the reserved-
 * region lookup. */
__attribute__((weak)) const XHCI_FAULT *
xhci_fault_get(void)
{
    return &g_xhci_fault_rec;
}

/* Reset the bring-up latch and fault state. Used by the host fault-injection
 * test to run multiple independent failure cases in one process. In the
 * firmware build this is never called (the bridge runs once and halts). */
__attribute__((weak)) void
xhci_test_reset(void)
{
    g_xhci_fatal = FALSE;
    g_xhci_initialized = FALSE;
    memset(&g_xhci_fault_rec, 0, sizeof(g_xhci_fault_rec));
}

/* ------------------------------------------------------------------ */
/* Register access helpers.                                            */
/*                                                                     */
/* These are weak so the host fault-injection test can override them   */
/* with a mock register file (see tests/test_xhci_fault.c). In the     */
/* firmware build no strong definition exists, so the real MMIO        */
/* accessors below are used.                                           */
/* ------------------------------------------------------------------ */

__attribute__((weak)) UINT32
xhci_read32(volatile UINT32 *reg)
{
    return *reg;
}

__attribute__((weak)) void
xhci_write32(volatile UINT32 *reg, UINT32 value)
{
    *reg = value;
}

/* Record a fatal fault at the given stage with the current controller state.
 * Sets g_xhci_fatal and fills the fault record for the harness readout. */
static void
xhci_fault(XHCI *xhci, UINT32 stage, UINT32 hint)
{
    g_xhci_fatal = TRUE;

    g_xhci_fault_rec.magic         = XHCI_FAULT_MAGIC;
    g_xhci_fault_rec.stage         = stage;
    g_xhci_fault_rec.hint          = hint;
    g_xhci_fault_rec.usbsts        = xhci ? xhci_read32(&xhci->op[XHCI_OP_USBSTS / 4]) : 0;
    g_xhci_fault_rec.usbcmd        = xhci ? xhci_read32(&xhci->op[XHCI_OP_USBCMD / 4]) : 0;
    g_xhci_fault_rec.crcr          = xhci ? xhci_read32(&xhci->op[XHCI_OP_CRCR / 4]) : 0;
    g_xhci_fault_rec.last_cc       = 0;
    g_xhci_fault_rec.last_trb_type = 0;
    g_xhci_fault_rec.doorbell      = 0;
}

#ifdef BRIDGE_DEBUG
/* Debug builds only: fill and publish the success record capturing the ACTUAL
 * XHCI hardware state after bring-up, so the harness can print it for
 * downstream verification (see xhci_status.h). Packs the two endpoints into
 * 32-bit words (addr|ep<<8|int<<16|spd<<24). */
static void
xhci_status_record(const XHCI *xhci, const USB_TOPOLOGY *topo)
{
    g_xhci_status.magic           = XHCI_STATUS_MAGIC;
    g_xhci_status.usbsts          = xhci_read32(&xhci->op[XHCI_OP_USBSTS / 4]);
    g_xhci_status.usbcmd          = xhci_read32(&xhci->op[XHCI_OP_USBCMD / 4]);
    g_xhci_status.crcr            = xhci_read32(&xhci->op[XHCI_OP_CRCR / 4]);
    g_xhci_status.max_slots       = xhci->max_slots;
    g_xhci_status.max_eps         = xhci->max_eps;
    g_xhci_status.max_scratchpad  = xhci->max_scratchpad;
    g_xhci_status.page_size       = xhci->page_size;
    g_xhci_status.xhci_mmio_base  = topo->xhci_mmio_base;
    g_xhci_status.xhci_cap_len    = topo->xhci_cap_len;
    /* B1 assigns its own device addresses during bring-up (kbd = device 1,
     * mouse = device 2, see xhci_setup_devices). The UEFI-assigned address
     * is not exposed by EFI_USB_IO_PROTOCOL and is not used here. */
    g_xhci_status.kbd             = (UINT32)1
                                  | ((UINT32)topo->kbd.endpoint << 8)
                                  | ((UINT32)topo->kbd.interval << 16)
                                  | ((UINT32)topo->kbd.speed << 24);
    g_xhci_status.mouse           = (UINT32)2
                                  | ((UINT32)topo->mouse.endpoint << 8)
                                  | ((UINT32)topo->mouse.interval << 16)
                                  | ((UINT32)topo->mouse.speed << 24);
    g_xhci_status.kbd_max_packet  = topo->kbd.max_packet;
    g_xhci_status.mouse_max_packet= topo->mouse.max_packet;

    xhci_status_publish(&g_xhci_status);
}
#endif /* BRIDGE_DEBUG */

/* Poll a register until a bit is set (returns TRUE) or a timeout elapses. */
static BOOLEAN
xhci_wait_bit_set(volatile UINT32 *reg, UINT32 mask, UINTN timeout_loops)
{
    while (timeout_loops-- > 0) {
        if (xhci_read32(reg) & mask)
            return TRUE;
    }
    return FALSE;
}

/* Poll a register until a bit is clear (returns TRUE) or a timeout elapses. */
static BOOLEAN
xhci_wait_bit_clear(volatile UINT32 *reg, UINT32 mask, UINTN timeout_loops)
{
    while (timeout_loops-- > 0) {
        if (!(xhci_read32(reg) & mask))
            return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* XHCI init: locate registers, verify XHCI >= 1.0 (C6).               */
/* ------------------------------------------------------------------ */

/* Initialize the XHCI register block from the recorded topology. */
static void
xhci_init(XHCI *xhci, const USB_TOPOLOGY *topo)
{
    UINT64 mmio = topo->xhci_mmio_base;
    UINT32 cap_len = topo->xhci_cap_len;
    UINT32 db_off, rt_off;
    UINT32 hcsparams1, hcsparams2;

    xhci->cap = (volatile UINT32 *)(UINTN)mmio;
    xhci->op  = (volatile UINT32 *)(UINTN)(mmio + cap_len);

    /* Doorbell and runtime register offsets from the capability registers. */
    db_off = xhci_read32(&xhci->cap[XHCI_CAP_DBOFF / 4]) & 0xFFFFFFF0u;
    rt_off = xhci_read32(&xhci->cap[XHCI_CAP_RTSOFF / 4]) & 0xFFFFFFF0u;
    xhci->doorbell = (volatile UINT32 *)(UINTN)(mmio + db_off);
    xhci->rt       = (volatile UINT32 *)(UINTN)(mmio + rt_off);

    hcsparams1 = xhci_read32(&xhci->cap[XHCI_CAP_HCSPARAMS1 / 4]);
    hcsparams2 = xhci_read32(&xhci->cap[XHCI_CAP_HCSPARAMS2 / 4]);

    xhci->max_slots      = hcsparams1 & 0xFF;
    xhci->max_eps        = ((hcsparams1 >> 8) & 0xFF) + 1;
    xhci->max_scratchpad = (hcsparams2 >> 27) & 0x1F;
    xhci->page_size      = xhci_read32(&xhci->op[XHCI_OP_PAGESIZE / 4]);
}

/* Verify the controller is XHCI >= 1.0 (C6). Belt-and-suspenders on top of
 * U1's check: read HCIVERSION (capability offset 0x00, bits 16:31, BCD)
 * and check it is >= 0x0100 (1.0). */
static BOOLEAN
xhci_verify_version(const XHCI *xhci)
{
    UINT32 hciversion = xhci_read32(&xhci->cap[XHCI_CAP_HCIVERSION / 4]);
    UINT32 spec_version = (hciversion >> 16) & 0xFFFF;
    return spec_version >= 0x0100;   /* 0x0100 = XHCI 1.0 */
}

/* ------------------------------------------------------------------ */
/* Controller reset.                                                   */
/* ------------------------------------------------------------------ */

/* Reset the controller (USBCMD HCRST) and wait for USBSTS HCHalted. */
static BOOLEAN
xhci_reset(XHCI *xhci)
{
    volatile UINT32 *cmd = &xhci->op[XHCI_OP_USBCMD / 4];
    volatile UINT32 *sts = &xhci->op[XHCI_OP_USBSTS / 4];

    /* Stop the controller first (clear RUN). */
    xhci_write32(cmd, xhci_read32(cmd) & ~USBCMD_RUN);
    if (!xhci_wait_bit_set(sts, USBSTS_HCH, 1000000))
        return FALSE;

    /* Assert HCRST. */
    xhci_write32(cmd, xhci_read32(cmd) | USBCMD_HCRST);
    if (!xhci_wait_bit_clear(cmd, USBCMD_HCRST, 1000000))
        return FALSE;

    /* Wait for HCHalted to be set after reset. */
    if (!xhci_wait_bit_set(sts, USBSTS_HCH, 1000000))
        return FALSE;

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Command ring + event ring setup.                                    */
/* ------------------------------------------------------------------ */

/* Set up the command ring and event ring, and point the controller at them. */
static BOOLEAN
xhci_setup_rings(XHCI *xhci)
{
    volatile UINT32 *crcr = &xhci->op[XHCI_OP_CRCR / 4];
    volatile UINT32 *erstsz = &xhci->rt[0x08 / 4];
    volatile UINT32 *erstba = &xhci->rt[0x10 / 4];
    volatile UINT32 *erdp   = &xhci->rt[0x18 / 4];
    UINT64 cmd_ring_addr, erst_addr, evt_ring_addr;
    UINTN i;

    /* Initialize the command ring (all TRBs, cycle bit 1). */
    for (i = 0; i < CMD_RING_SIZE; i++)
        g_cmd_ring[i].field3 = 0;
    g_cmd_ring_enq  = 0;
    g_cmd_ring_cycle = 1;

    /* Initialize the event ring (all TRBs, cycle bit 0). */
    for (i = 0; i < EVT_RING_SIZE; i++)
        g_evt_ring[i].field3 = 0;
    g_evt_ring_deq   = 0;
    g_evt_ring_cycle = 0;

    /* ERST: one segment pointing at the event ring. */
    evt_ring_addr = (UINT64)(UINTN)g_evt_ring;
    g_erst[0].seg_addr_low  = (UINT32)(evt_ring_addr & 0xFFFFFFFFu);
    g_erst[0].seg_addr_high = (UINT32)(evt_ring_addr >> 32);
    g_erst[0].seg_size      = EVT_RING_SIZE;
    g_erst[0].reserved      = 0;

    /* Program the ERST (size, base, then dequeue pointer). */
    xhci_write32(erstsz, 1);   /* one segment */
    erst_addr = (UINT64)(UINTN)g_erst;
    xhci_write32(&erstba[0], (UINT32)(erst_addr & 0xFFFFFFFFu));
    xhci_write32(&erstba[1], (UINT32)(erst_addr >> 32));
    xhci_write32(&erdp[0], (UINT32)(evt_ring_addr & 0xFFFFFFFFu));
    xhci_write32(&erdp[1], (UINT32)(evt_ring_addr >> 32));

    /* Program the command ring control register. */
    cmd_ring_addr = (UINT64)(UINTN)g_cmd_ring;
    xhci_write32(&crcr[0], (UINT32)(cmd_ring_addr & 0xFFFFFFFFu) | CRCR_RCS);
    xhci_write32(&crcr[1], (UINT32)(cmd_ring_addr >> 32));

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Device context setup.                                               */
/* ------------------------------------------------------------------ */

/* Program the slot context for a device. */
static void
slot_context_init(SLOT_CONTEXT *sc, UINT8 speed, UINT8 port)
{
    UINTN i;
    for (i = 0; i < 8; i++)
        sc->dword[i] = 0;

    /* DWORD 0: Context Entries (bits 7:0) = 2 (EP0 + 1 IN endpoint),
     * Speed (bits 31:28). */
    sc->dword[0] = 2 | ((UINT32)speed << 28);

    /* DWORD 1: Root Hub Port Number (bits 7:0), Interrupter Target (bits
     * 23:22) = 0, Slot State (bits 31:27) = 0 (Disabled). */
    sc->dword[1] = (UINT32)port & 0xFF;

    /* DWORD 2: Device Address (bits 7:0) - set by Address Device command. */
    sc->dword[2] = 0;
}

/* Program the interrupt IN endpoint context. */
static void
ep_context_init(EP_CONTEXT *ec, UINT16 max_packet, UINT8 interval,
                UINT64 tr_dequeue)
{
    UINTN i;
    UINT32 interval_xhci;

    for (i = 0; i < 8; i++)
        ec->dword[i] = 0;

    /* Convert USB bInterval (frames for low/full speed) to the XHCI
     * interval (log2 of the interval in 125us microframes). */
    interval_xhci = 0;
    {
        UINT32 microframes = (UINT32)interval * 8;
        while (microframes > 1) {
            microframes >>= 1;
            interval_xhci++;
        }
    }

    /* DWORD 0: Max Packet Size (bits 15:8). */
    ec->dword[0] = ((UINT32)max_packet & 0xFFFF) << 8;

    /* DWORD 1: CErr (bits 5:4) = 3, EP Type (bits 9:8) = Interrupt IN (3),
     * Interval (bits 19:16). */
    ec->dword[1] = (3u << 4) | (EP_TYPE_INTERRUPT_IN << 8) |
                   ((interval_xhci & 0xFF) << 16);

    /* DWORD 2/3: TR Dequeue Pointer (64-bit, 16-byte aligned). */
    ec->dword[2] = (UINT32)(tr_dequeue & 0xFFFFFFFFu);
    ec->dword[3] = (UINT32)(tr_dequeue >> 32);

    /* DWORD 4: Average TRB Length (bits 15:0) = max_packet. */
    ec->dword[4] = (UINT32)max_packet & 0xFFFF;
}

/* Set up the DCBAA and device contexts for the two devices. */
static void
xhci_setup_devices(XHCI *xhci, const USB_TOPOLOGY *topo)
{
    UINT64 dcbaa_addr = (UINT64)(UINTN)g_dcbaa;
    UINTN i;

    /* Clear the DCBAA. */
    for (i = 0; i <= MAX_DEVICES; i++)
        g_dcbaa[i] = 0;

    /* Point DCBAA entries at the device contexts. */
    g_dcbaa[1] = (UINT64)(UINTN)&g_dev_ctx[0];
    g_dcbaa[2] = (UINT64)(UINTN)&g_dev_ctx[1];

    /* Program DCBAAP. */
    xhci_write32(&xhci->op[XHCI_OP_DCBAAP / 4],
                 (UINT32)(dcbaa_addr & 0xFFFFFFFFu));
    xhci_write32(&xhci->op[XHCI_OP_DCBAAP / 4 + 1],
                 (UINT32)(dcbaa_addr >> 32));

    /* Set the max device slots enabled in CONFIG. */
    xhci_write32(&xhci->op[XHCI_OP_CONFIG / 4], MAX_DEVICES);

    /* Keyboard: device 1, slot context + interrupt IN endpoint. */
    slot_context_init(&g_dev_ctx[0].slot, topo->kbd.speed, topo->kbd.port);
    ep_context_init(&g_dev_ctx[0].ep_in,
                    topo->kbd.max_packet,
                    topo->kbd.interval,
                    (UINT64)(UINTN)g_tr_kbd.trbs);

    /* Mouse: device 2, slot context + interrupt IN endpoint. */
    slot_context_init(&g_dev_ctx[1].slot, topo->mouse.speed, topo->mouse.port);
    ep_context_init(&g_dev_ctx[1].ep_in,
                    topo->mouse.max_packet,
                    topo->mouse.interval,
                    (UINT64)(UINTN)g_tr_mouse.trbs);
}

/* ------------------------------------------------------------------ */
/* Transfer ring setup.                                                */
/* ------------------------------------------------------------------ */

/* Fill a transfer ring with Normal TRBs pointing at a data buffer. */
static void
transfer_ring_init(TRANSFER_RING *tr, UINT8 *buf, UINTN buf_len)
{
    UINTN i;
    UINT64 buf_addr = (UINT64)(UINTN)buf;

    for (i = 0; i < TR_RING_SIZE; i++) {
        tr->trbs[i].field0 = (UINT32)(buf_addr & 0xFFFFFFFFu);
        tr->trbs[i].field1 = (UINT32)(buf_addr >> 32);
        /* Transfer Length (bits 16:0), Interrupter Target (bits 31:22) = 0. */
        tr->trbs[i].field2 = (UINT32)buf_len & 0x1FFFF;
        /* Cycle bit (bit 0) = 1, TRB Type (bits 10:6) = Normal (1). */
        tr->trbs[i].field3 = (1u << 0) | (TRB_TYPE_NORMAL << 6);
    }
    tr->enq  = 0;
    tr->cycle = 1;
}

/* ------------------------------------------------------------------ */
/* Doorbell + polling.                                                 */
/* ------------------------------------------------------------------ */

/* Ring the doorbell for an endpoint to start/continue a periodic IN. */
static void
xhci_ring_endpoint_doorbell(XHCI *xhci, UINT8 endpoint, UINT32 slot)
{
    UINT32 ep_num = endpoint & 0x0F;
    UINT32 db_target = 2 * ep_num + 1;   /* IN endpoint doorbell target */
    xhci_write32(&xhci->doorbell[slot], db_target);
}

/* Re-arm a transfer ring after a completed transfer and ring the doorbell
 * to start the next periodic IN transfer.
 *
 * In XHCI, after the controller consumes a Normal TRB on a periodic IN
 * endpoint it toggles the TRB's cycle bit (returning ownership to software)
 * and posts a Transfer Event. The software must flip the cycle bit back to
 * the producer's cycle and ring the doorbell again, or the endpoint stops
 * transferring after the first report. Each ring holds a single TRB (all
 * TRBs point at the same report buffer), so re-arming is just flipping the
 * one TRB's cycle bit back. */
static void
xhci_rearm_transfer(XHCI *xhci, TRANSFER_RING *tr, UINT8 endpoint, UINT32 slot)
{
    /* Flip the consumed TRB's cycle bit back to the producer's cycle. */
    tr->trbs[tr->enq].field3 ^= (1u << 0);

    /* Ring the doorbell to start the next transfer. */
    xhci_ring_endpoint_doorbell(xhci, endpoint, slot);
}

/* Poll the event ring for a Transfer Event. Returns TRUE and fills *out
 * with the event TRB if one is ready. */
static BOOLEAN
xhci_poll_transfer_event(XHCI *xhci, TRB *out)
{
    TRB *evt = &g_evt_ring[g_evt_ring_deq];

    if (((evt->field3 >> 0) & 1u) != g_evt_ring_cycle)
        return FALSE;

    if (((evt->field3 >> 6) & 0x3F) == TRB_TYPE_TRANSFER_EVENT) {
        *out = *evt;
        g_evt_ring_deq = (g_evt_ring_deq + 1) % EVT_RING_SIZE;
        if (g_evt_ring_deq == 0)
            g_evt_ring_cycle ^= 1;
        /* Update ERDP. */
        xhci_write32(&xhci->rt[0x18 / 4],
                     (UINT32)((UINT64)(UINTN)g_evt_ring & 0xFFFFFFFFu));
        xhci_write32(&xhci->rt[0x1C / 4],
                     (UINT32)((UINT64)(UINTN)g_evt_ring >> 32));
        return TRUE;
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/* bridge_poll_usb(): the periodic-IN poll entry point.                */
/* ------------------------------------------------------------------ */

void
bridge_poll_usb(void)
{
    XHCI xhci;
    const USB_TOPOLOGY *topo;
    TRB evt;
    UINT64 trb_ptr;
    UINT32 cc;

    /* If a fatal fault was detected, do nothing (the bridge halts). */
    if (g_xhci_fatal)
        return;

    topo = usb_topology_get();
    if (topo == NULL) {
        xhci_fault(NULL, XHCI_STAGE_NONE, XHCI_FAULT_HINT_NONE);
        return;
    }

    xhci_init(&xhci, topo);

    /* Publish the fault record once so the harness can read it after a halt. */
    xhci_fault_publish_rec();

    /* One-time controller bring-up. Each stage records its own fault so the
     * harness can report exactly where the UEFI->XHCI handoff failed. */
    if (!g_xhci_initialized) {
        /* Belt-and-suspenders XHCI >= 1.0 check (C6). */
        if (!xhci_verify_version(&xhci)) {
            xhci_fault(&xhci, XHCI_STAGE_VERIFY, XHCI_FAULT_HINT_VERIFY);
            return;
        }

        if (!xhci_reset(&xhci)) {
            xhci_fault(&xhci, XHCI_STAGE_RESET, XHCI_FAULT_HINT_RESET_TIMEOUT);
            return;
        }

        if (!xhci_setup_rings(&xhci)) {
            xhci_fault(&xhci, XHCI_STAGE_RINGS, XHCI_FAULT_HINT_RING_SETUP);
            return;
        }

        xhci_setup_devices(&xhci, topo);

        /* Set up the transfer rings. */
        transfer_ring_init(&g_tr_kbd, g_kbd_buf, HID_KBD_REPORT_SIZE);
        transfer_ring_init(&g_tr_mouse, g_mouse_buf, HID_MOUSE_REPORT_SIZE);

        /* Start the controller (RUN). */
        xhci_write32(&xhci.op[XHCI_OP_USBCMD / 4], USBCMD_RUN);
        if (!xhci_wait_bit_clear(&xhci.op[XHCI_OP_USBSTS / 4], USBSTS_HCH,
                                 1000000)) {
            xhci_fault(&xhci, XHCI_STAGE_RUN, XHCI_FAULT_HINT_RUN);
            return;
        }

        /* Ring the doorbells to start the periodic IN transfers. */
        xhci_ring_endpoint_doorbell(&xhci, topo->kbd.endpoint, 1);
        xhci_ring_endpoint_doorbell(&xhci, topo->mouse.endpoint, 2);
        g_xhci_fault_rec.doorbell = 2 * (topo->mouse.endpoint & 0x0F) + 1;

        g_xhci_initialized = TRUE;

#ifdef BRIDGE_DEBUG
        /* Debug builds: record the actual hardware state for the harness. */
        xhci_status_record(&xhci, topo);
#endif
    }

    /* Poll the event ring for completed transfers. */
    while (xhci_poll_transfer_event(&xhci, &evt)) {
        cc = (evt.field3 >> 24) & 0xFF;
        g_xhci_fault_rec.last_cc       = cc;
        g_xhci_fault_rec.last_trb_type = (evt.field3 >> 6) & 0x3F;

        if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
            /* Record the poll-stage fault but keep polling (a transient
             * error on one endpoint should not halt the whole bridge). */
            g_xhci_fault_rec.stage = XHCI_STAGE_POLL;
            g_xhci_fault_rec.hint  = XHCI_FAULT_HINT_POLL;
            continue;
        }

        /* Identify which endpoint completed by matching the TRB pointer. */
        trb_ptr = ((UINT64)evt.field1 << 32) | evt.field0;
        if (trb_ptr == (UINT64)(UINTN)g_tr_kbd.trbs) {
            g_raw_kbd.modifier = g_kbd_buf[0];
            g_raw_kbd.reserved = g_kbd_buf[1];
            g_raw_kbd.key[0]   = g_kbd_buf[2];
            g_raw_kbd.key[1]   = g_kbd_buf[3];
            g_raw_kbd.key[2]   = g_kbd_buf[4];
            g_raw_kbd.key[3]   = g_kbd_buf[5];
            g_raw_kbd.key[4]   = g_kbd_buf[6];
            g_raw_kbd.key[5]   = g_kbd_buf[7];
            g_kbd_valid = TRUE;

            /* Re-arm the transfer ring and ring the doorbell so the next
             * periodic IN transfer is scheduled. */
            xhci_rearm_transfer(&xhci, &g_tr_kbd, topo->kbd.endpoint, 1);
        } else if (trb_ptr == (UINT64)(UINTN)g_tr_mouse.trbs) {
            g_raw_mouse.buttons = g_mouse_buf[0];
            g_raw_mouse.dx      = (INT8)g_mouse_buf[1];
            g_raw_mouse.dy      = (INT8)g_mouse_buf[2];
            g_mouse_valid = TRUE;

            /* Re-arm the transfer ring and ring the doorbell so the next
             * periodic IN transfer is scheduled. */
            xhci_rearm_transfer(&xhci, &g_tr_mouse, topo->mouse.endpoint, 2);
        }
    }
}

/* Return TRUE if the USB controller is unusable (non-XHCI>=1.0 or a fatal
 * fault). The bridge halts cleanly when this is set. */
BOOLEAN
bridge_usb_fatal(void)
{
    return g_xhci_fatal;
}
