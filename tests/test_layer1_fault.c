/*
 * test_layer1_fault.c - Host test for the XHCI bring-up fault record ABI.
 *
 * Verifies the debugging-model contract (src/bridge/xhci_fault.h) that the
 * bridge (writer) and the Layer 1 harness (reader) both rely on:
 *   - The magic value ("XHCI").
 *   - The stage/hint enum values are sequential from 0, so the harness's
 *     string-table bounds check (stage < array_len) is correct and the
 *     one-screen readout never overruns.
 *   - The XHCI_FAULT struct layout (size + field offsets) is stable, so the
 *     bridge writing the record and the harness reading it agree on the ABI.
 *
 * NOTE: we deliberately do NOT call xhci_fault_publish()/xhci_fault_lookup()
 * here - they write to the fixed physical address XHCI_FAULT_PTR_ADDR
 * (0x10000018), which is valid in the firmware's reserved region but is an
 * unmapped address on the host. The publish/lookup transport is exercised on
 * real hardware / QEMU in Layer 1. This test pins the ABI contract instead.
 *
 * See docs/architecture.md section 9, Layer 1.
 */

#include <stdio.h>
#include <stddef.h>

#include "xhci_fault.h"

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

int
main(void)
{
    printf("Layer 1 fault-record ABI host test\n");
    printf("==================================\n");

    /* --- Test 1: magic sanity check ---------------------------------- */
    printf("\n[Test 1] Magic sanity check\n");
    CHECK_EQ(XHCI_FAULT_MAGIC, 0x58484349u);   /* "XHCI" */

    /* --- Test 2: stage enum bounds (harness string-table safety) ------ */
    printf("\n[Test 2] Stage enum bounds\n");
    /* The Layer 1 harness indexes g_stage_names[] by these values. They must
     * be sequential from 0 so the harness's bounds check is correct. */
    CHECK_EQ(XHCI_STAGE_NONE,     0u);
    CHECK_EQ(XHCI_STAGE_VERIFY,   1u);
    CHECK_EQ(XHCI_STAGE_RESET,    2u);
    CHECK_EQ(XHCI_STAGE_RINGS,    3u);
    CHECK_EQ(XHCI_STAGE_DEVICES,  4u);
    CHECK_EQ(XHCI_STAGE_TRANSFER, 5u);
    CHECK_EQ(XHCI_STAGE_RUN,      6u);
    CHECK_EQ(XHCI_STAGE_DOORBELL, 7u);
    CHECK_EQ(XHCI_STAGE_POLL,     8u);

    /* --- Test 3: hint enum bounds ------------------------------------- */
    printf("\n[Test 3] Hint enum bounds\n");
    CHECK_EQ(XHCI_FAULT_HINT_NONE,          0u);
    CHECK_EQ(XHCI_FAULT_HINT_VERIFY,        1u);
    CHECK_EQ(XHCI_FAULT_HINT_RESET_TIMEOUT, 2u);
    CHECK_EQ(XHCI_FAULT_HINT_RING_SETUP,    3u);
    CHECK_EQ(XHCI_FAULT_HINT_DEV_CTX,       4u);
    CHECK_EQ(XHCI_FAULT_HINT_TRANSFER,      5u);
    CHECK_EQ(XHCI_FAULT_HINT_RUN,           6u);
    CHECK_EQ(XHCI_FAULT_HINT_DOORBELL,      7u);
    CHECK_EQ(XHCI_FAULT_HINT_POLL,          8u);

    /* --- Test 4: struct layout (bridge/harness ABI agreement) --------- */
    printf("\n[Test 4] XHCI_FAULT struct layout\n");
    /* The bridge writes these fields; the harness reads them. They must be
     * at stable offsets so both sides agree on the record. */
    CHECK_EQ(offsetof(XHCI_FAULT, magic),           0u);
    CHECK_EQ(offsetof(XHCI_FAULT, stage),           4u);
    CHECK_EQ(offsetof(XHCI_FAULT, hint),            8u);
    CHECK_EQ(offsetof(XHCI_FAULT, usbsts),          12u);
    CHECK_EQ(offsetof(XHCI_FAULT, usbcmd),          16u);
    CHECK_EQ(offsetof(XHCI_FAULT, crcr),            20u);
    CHECK_EQ(offsetof(XHCI_FAULT, last_cc),         24u);
    CHECK_EQ(offsetof(XHCI_FAULT, last_trb_type),   28u);
    CHECK_EQ(offsetof(XHCI_FAULT, doorbell),        32u);
    CHECK_EQ(offsetof(XHCI_FAULT, reserved),        36u);
    CHECK_EQ(sizeof(XHCI_FAULT),                    64u);

    printf("\n=======================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
