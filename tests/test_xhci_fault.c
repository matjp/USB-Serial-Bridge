/*
 * test_xhci_fault.c - One-shot verification of the XHCI bring-up fault code.
 *
 * Compiles the REAL B1 driver (src/bridge/xhci.c) on the host and drives
 * bridge_poll_usb() into a real failure path, then asserts the fault record
 * is populated correctly. This verifies the NEW failure code (xhci_fault()
 * + the staged bring-up) end-to-end without hardware.
 *
 * Mechanism: xhci.c's register accessors (xhci_read32/xhci_write32) and the
 * topology/fault indirection points (usb_topology_get/xhci_fault_publish_rec/
 * xhci_fault_get) are WEAK symbols. This test provides strong definitions
 * that back them with a mock register file, so bridge_poll_usb() runs against
 * a simulated controller instead of real MMIO.
 *
 * Failure modes exercised:
 *   - verify: HCCPARAMS1 spec version < 1.0  -> XHCI_STAGE_VERIFY
 *   - reset : USBSTS.HCH never set           -> XHCI_STAGE_RESET
 *
 * See docs/architecture.md section 9, Layer 1.
 */

#include <stdio.h>
#include <string.h>

#include "xhci_fault.h"
#include "usb_topology.h"

/* --- Mock register file ------------------------------------------------ */
/* A contiguous region that xhci_init() will interpret as the XHCI MMIO
 * space. xhci_mmio_base is a 32-bit value (as in the firmware), so we use a
 * FAKE low MMIO base and translate it to the real host array in the mock
 * accessors. This keeps the 32-bit pointer arithmetic in xhci.c valid on the
 * host (where the real array would live above 4 GB). */
#define MOCK_MMIO_BASE  0x10000000u   /* fake 32-bit MMIO base */
#define MOCK_MMIO_SIZE  0x4000        /* 64 KB: cap+op+doorbell+rt */
static UINT32 g_mock[MOCK_MMIO_SIZE / 4];

/* Strong overrides of the weak accessors in xhci.c. */
UINT32
xhci_read32(volatile UINT32 *reg)
{
    UINTN off = (UINTN)reg - MOCK_MMIO_BASE;
    if (off >= MOCK_MMIO_SIZE)
        return 0;
    return g_mock[off / 4];
}

void
xhci_write32(volatile UINT32 *reg, UINT32 value)
{
    UINTN off = (UINTN)reg - MOCK_MMIO_BASE;
    if (off >= MOCK_MMIO_SIZE)
        return;
    g_mock[off / 4] = value;

    /* Model the controller's reset/run handshake so the healthy path can
     * complete bring-up. op is at mmio+cap_len (0x40); USBCMD=0x00,
     * USBSTS=0x04 relative to op (byte offsets 0x40 / 0x44). */
    if (off == 0x40) {                 /* USBCMD */
        if (value & 0x2u) {            /* HCRST asserted */
            g_mock[0x40 / 4] &= ~0x2u; /* controller clears HCRST */
            g_mock[0x44 / 4] |= 0x1u;  /* HCH set after reset */
        }
        if (value & 0x1u) {            /* RUN set */
            g_mock[0x44 / 4] &= ~0x1u; /* HCH clears once running */
        }
    }
}

/* Strong override: return the mock topology (no fixed-address lookup). */
static USB_TOPOLOGY g_topo;
const USB_TOPOLOGY *
usb_topology_get(void)
{
    return &g_topo;
}

/* Strong override: no-op (the fixed reserved-region publish is unmapped on
 * the host). The test reads the record via xhci_fault_get() instead. */
void
xhci_fault_publish_rec(void)
{
}

/* --- Test helpers ------------------------------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            g_pass++;                                                   \
            printf("  PASS: %s\n", #cond);                              \
        } else {                                                        \
            g_fail++;                                                   \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);          \
        }                                                               \
    } while (0)

#define CHECK_EQ(actual, want)                                          \
    do {                                                                \
        unsigned _a = (unsigned)(actual);                               \
        unsigned _w = (unsigned)(want);                                 \
        if (_a == _w) {                                                 \
            g_pass++;                                                   \
            printf("  PASS: %s == %s (0x%X)\n", #actual, #want, _a);    \
        } else {                                                        \
            g_fail++;                                                   \
            printf("  FAIL: %s got 0x%X want 0x%X (line %d)\n",         \
                   #actual, _a, _w, __LINE__);                          \
        }                                                               \
    } while (0)

/* Reset the mock to a "healthy" controller baseline, then apply a callback
 * that the test uses to inject the specific failure. */
static void
mock_reset(void)
{
    memset(g_mock, 0, sizeof(g_mock));

    /* Capability registers (relative to mmio base). */
    g_mock[0x04 / 4] = 0x00000001;   /* HCSPARAMS1: 1 slot, 1 ep */
    g_mock[0x08 / 4] = 0x00000000;   /* HCSPARAMS2 */
    g_mock[0x10 / 4] = 0x10000000;   /* HCCPARAMS1: spec version 1.0 (bits31:24) */
    g_mock[0x14 / 4] = 0x00001000;   /* DBOFF: doorbell at mmio+0x1000 */
    g_mock[0x18 / 4] = 0x00002000;   /* RTSOFF: runtime at mmio+0x2000 */

    /* Operational registers (relative to mmio + cap_len = 0x40). */
    g_mock[(0x40 + 0x08) / 4] = 0x00001000;   /* PAGESIZE */

    /* Topology: point at the fake MMIO base. cap_len = 0x40. */
    memset(&g_topo, 0, sizeof(g_topo));
    g_topo.xhci_mmio_base = MOCK_MMIO_BASE;
    g_topo.xhci_cap_len   = 0x40;
    g_topo.kbd.endpoint   = 0x81;   /* EP1 IN */
    g_topo.mouse.endpoint = 0x82;   /* EP2 IN */
    g_topo.kbd.interval   = 1;
    g_topo.mouse.interval = 1;
    g_topo.kbd.max_packet = 8;
    g_topo.mouse.max_packet = 3;
    g_topo.kbd.speed      = 1;      /* low speed */
    g_topo.mouse.speed    = 1;
    g_topo.kbd.port       = 1;      /* root-hub port 1 */
    g_topo.mouse.port     = 2;      /* root-hub port 2 */
}

/* bridge_poll_usb() is declared in bridge.h. */
#include "bridge.h"

/* Weak symbols defined in xhci.c (not in a header). Declared here so the
 * test can call them. */
const XHCI_FAULT *xhci_fault_get(void);
void xhci_test_reset(void);
void bridge_poll_usb(void);

int
main(void)
{
    const XHCI_FAULT *f;

    printf("XHCI bring-up fault one-shot verification\n");
    printf("==========================================\n");

    /* --- Test 1: verify failure (spec version < 1.0) ------------------ */
    printf("\n[Test 1] Verify failure (HCCPARAMS1 spec < 1.0)\n");
    mock_reset();
    xhci_test_reset();
    g_mock[0x10 / 4] = 0x00000000;   /* spec version 0 -> not XHCI >= 1.0 */

    bridge_poll_usb();
    f = xhci_fault_get();

    CHECK(f != NULL);
    if (f != NULL) {
        CHECK_EQ(f->magic, XHCI_FAULT_MAGIC);
        CHECK_EQ(f->stage, XHCI_STAGE_VERIFY);
        CHECK_EQ(f->hint,  XHCI_FAULT_HINT_VERIFY);
    }

    /* --- Test 2: reset failure (USBSTS.HCH never set) ------------------ */
    printf("\n[Test 2] Reset failure (USBSTS.HCH never set)\n");
    mock_reset();
    xhci_test_reset();
    /* Healthy baseline: USBSTS.HCH (bit 0) set so reset's first wait passes.
     * To force a reset failure, leave HCH clear. */
    g_mock[(0x40 + 0x04) / 4] = 0x00000000;   /* USBSTS: HCH clear */

    bridge_poll_usb();
    f = xhci_fault_get();

    CHECK(f != NULL);
    if (f != NULL) {
        CHECK_EQ(f->magic, XHCI_FAULT_MAGIC);
        CHECK_EQ(f->stage, XHCI_STAGE_RESET);
        CHECK_EQ(f->hint,  XHCI_FAULT_HINT_RESET_TIMEOUT);
        /* The register snapshot should reflect the mock USBSTS/USBCMD. */
        CHECK_EQ(f->usbsts, 0x00000000u);
    }

    /* --- Test 3: healthy controller -> no fault ------------------------ */
    printf("\n[Test 3] Healthy controller -> no fault\n");
    mock_reset();
    xhci_test_reset();
    /* Set USBSTS.HCH so reset's first wait passes. */
    g_mock[(0x40 + 0x04) / 4] = 0x00000001;   /* USBSTS: HCH set */

    bridge_poll_usb();
    f = xhci_fault_get();

    /* A healthy controller should NOT set a fatal fault. (The record may be
     * partially written by the poll stage, but magic must not be set.) */
    CHECK(f == NULL || f->magic != XHCI_FAULT_MAGIC);

    printf("\n=======================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
