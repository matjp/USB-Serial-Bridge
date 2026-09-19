/*
 * madt.c - U2: ACPI MADT patch (rule 4 of the ExitBootServices handoff).
 *
 * After the bridge AP has detached from UEFI and is running its own
 * bare-metal poll loop, we mark that core as DISABLED in the ACPI MADT
 * (Multiple APIC Description Table) so the OS believes the core is
 * missing/dead and never tries to bring it up or send it an INIT IPI.
 *
 * The OS enumerates processors from the MADT's Local APIC entries. Each
 * entry has a Flags field whose bit 0 (ENABLED) tells the OS whether the
 * processor is present and usable. Flipping that bit to 0 for the bridge
 * core hides it from the OS.
 *
 * We locate the RSDP via the EFI_ACPI_TABLE_PROTOCOL, walk RSDP -> RSDT/XSDT
 * -> MADT, find the Local APIC entry whose APIC ID matches the bridge AP's
 * (captured during bring-up as g_bridge_apic_id), and clear its enabled bit.
 *
 * See docs/architecture.md section 7.3, module U2.
 */

#include <efi.h>
#include <efilib.h>

#include "uefi.h"
#include "../bridge/usb_topology.h"

/* The bridge AP's APIC ID, captured during bring-up (defined in
 * core_bringup.c). */
extern UINT32 g_bridge_apic_id;

/* ------------------------------------------------------------------ */
/* ACPI table structures (subset needed for the MADT walk).            */
/* ------------------------------------------------------------------ */

/* Root System Description Pointer (ACPI 2.0+). */
typedef struct {
    CHAR8  Signature[8];      /* "RSD PTR " */
    UINT8  Checksum;
    UINT8  OemId[6];
    UINT8  Revision;
    UINT32 RsdtAddress;       /* 32-bit RSDT address (ACPI 1.0) */
    UINT32 Length;
    UINT64 XsdtAddress;       /* 64-bit XSDT address (ACPI 2.0+) */
    UINT8  ExtendedChecksum;
    UINT8  Reserved[3];
} ACPI_RSDP;

/* System Description Table header (common to RSDT/XSDT/MADT). */
typedef struct {
    CHAR8  Signature[4];
    UINT32 Length;
    UINT8  Revision;
    UINT8  Checksum;
    CHAR8  OemId[6];
    CHAR8  OemTableId[8];
    UINT32 OemRevision;
    UINT32 CreatorId;
    UINT32 CreatorRevision;
} ACPI_SDT_HEADER;

/* Multiple APIC Description Table (MADT). */
typedef struct {
    ACPI_SDT_HEADER Header;
    UINT32 LocalApicAddress;
    UINT32 Flags;
    /* Followed by variable-length APIC structure entries. */
} ACPI_MADT;

/* MADT Local APIC entry (Type 0). */
typedef struct {
    UINT8  Type;              /* 0 = Local APIC */
    UINT8  Length;            /* 8 */
    UINT8  AcpiProcessorId;
    UINT8  ApicId;
    UINT32 Flags;             /* bit 0 = ENABLED */
} MADT_LOCAL_APIC;

#define MADT_TYPE_LOCAL_APIC  0
#define MADT_LOCAL_APIC_ENABLED  (1u << 0)

/* EFI_ACPI_TABLE_PROTOCOL GUID. */
#define EFI_ACPI_TABLE_PROTOCOL_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, \
      {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d} }

/* EFI_ACPI_TABLE_VERSION is a bitmask of ACPI table versions. Some GNU-EFI
 * versions do not define it; supply a compatible type if absent. */
#ifndef EFI_ACPI_TABLE_VERSION
typedef UINT32 EFI_ACPI_TABLE_VERSION;
#endif

typedef
EFI_STATUS
(EFIAPI *EFI_ACPI_TABLE_GET_ACPI_TABLE) (
    IN  UINTN  Index,
    OUT VOID   **Table,
    OUT EFI_ACPI_TABLE_VERSION *Version,
    OUT UINTN  *TableKey
);

typedef struct {
    EFI_ACPI_TABLE_GET_ACPI_TABLE GetAcpiTable;
} EFI_ACPI_TABLE_PROTOCOL;

/* ------------------------------------------------------------------ */
/* MADT walk + patch.                                                  */
/* ------------------------------------------------------------------ */

/* Walk the MADT's variable-length entries and clear the ENABLED bit of
 * the Local APIC entry whose APIC ID matches the bridge AP. Returns TRUE
 * if the entry was found and patched. */
static BOOLEAN
madt_disable_apic(MADT_LOCAL_APIC *madt, UINT32 madt_length)
{
    UINT8 *p = (UINT8 *)madt + sizeof(ACPI_MADT);
    UINT8 *end = (UINT8 *)madt + madt_length;

    while (p + 2 <= end) {
        UINT8 type = p[0];
        UINT8 len  = p[1];

        if (len < 2)
            break;   /* Malformed entry; stop. */

        if (type == MADT_TYPE_LOCAL_APIC && len >= 8) {
            MADT_LOCAL_APIC *lapic = (MADT_LOCAL_APIC *)p;
            if (lapic->ApicId == (UINT8)g_bridge_apic_id) {
                /* Clear the ENABLED bit so the OS treats the core as
                 * missing/dead. */
                lapic->Flags &= ~MADT_LOCAL_APIC_ENABLED;
                return TRUE;
            }
        }

        p += len;
    }

    return FALSE;
}

/* Locate the MADT via the RSDP and disable the bridge AP's Local APIC
 * entry. Returns EFI_SUCCESS if the MADT was found and patched (or if no
 * matching entry exists - the OS simply won't see the core). */
static EFI_STATUS
madt_patch_from_rsdp(ACPI_RSDP *rsdp)
{
    ACPI_SDT_HEADER *sdt = NULL;
    UINT64 table_addr = 0;
    UINT32 table_len = 0;
    UINTN i;

    /* Prefer the XSDT (ACPI 2.0+) for 64-bit table addresses. */
    if (rsdp->Revision >= 2 && rsdp->XsdtAddress != 0) {
        sdt = (ACPI_SDT_HEADER *)(UINTN)rsdp->XsdtAddress;
        if (sdt->Signature[0] == 'X' && sdt->Signature[1] == 'S' &&
            sdt->Signature[2] == 'D' && sdt->Signature[3] == 'T') {
            UINT64 *entry = (UINT64 *)((UINT8 *)sdt + sizeof(ACPI_SDT_HEADER));
            UINTN count = (sdt->Length - sizeof(ACPI_SDT_HEADER)) / 8;
            for (i = 0; i < count; i++) {
                ACPI_SDT_HEADER *t = (ACPI_SDT_HEADER *)(UINTN)entry[i];
                if (t != NULL && t->Signature[0] == 'A' &&
                    t->Signature[1] == 'P' && t->Signature[2] == 'I' &&
                    t->Signature[3] == 'C') {
                    table_addr = entry[i];
                    table_len  = t->Length;
                    break;
                }
            }
        }
    }

    /* Fall back to the RSDT (ACPI 1.0, 32-bit addresses). */
    if (table_addr == 0 && rsdp->RsdtAddress != 0) {
        sdt = (ACPI_SDT_HEADER *)(UINTN)rsdp->RsdtAddress;
        if (sdt->Signature[0] == 'R' && sdt->Signature[1] == 'S' &&
            sdt->Signature[2] == 'D' && sdt->Signature[3] == 'T') {
            UINT32 *entry = (UINT32 *)((UINT8 *)sdt + sizeof(ACPI_SDT_HEADER));
            UINTN count = (sdt->Length - sizeof(ACPI_SDT_HEADER)) / 4;
            for (i = 0; i < count; i++) {
                ACPI_SDT_HEADER *t = (ACPI_SDT_HEADER *)(UINTN)entry[i];
                if (t != NULL && t->Signature[0] == 'A' &&
                    t->Signature[1] == 'P' && t->Signature[2] == 'I' &&
                    t->Signature[3] == 'C') {
                    table_addr = entry[i];
                    table_len  = t->Length;
                    break;
                }
            }
        }
    }

    if (table_addr == 0)
        return EFI_NOT_FOUND;

    madt_disable_apic((MADT_LOCAL_APIC *)(UINTN)table_addr, table_len);
    return EFI_SUCCESS;
}

/* Locate the RSDP via the EFI_ACPI_TABLE_PROTOCOL and patch the MADT. */
EFI_STATUS
uefi_disable_bridge_ap_in_madt(void)
{
    EFI_ACPI_TABLE_PROTOCOL *acpi = NULL;
    EFI_GUID acpi_guid = EFI_ACPI_TABLE_PROTOCOL_GUID;
    EFI_STATUS status;
    VOID *rsdp = NULL;
    EFI_ACPI_TABLE_VERSION version;
    UINTN table_key = 0;

    /* If no bridge AP was captured, there is nothing to hide. */
    if (g_bridge_apic_id == 0xFFFFFFFFu)
        return EFI_SUCCESS;

    status = uefi_call_wrapper(
        BS->LocateProtocol, 3, &acpi_guid, NULL, (VOID **)&acpi);
    if (EFI_ERROR(status) || acpi == NULL)
        return EFI_UNSUPPORTED;

    status = uefi_call_wrapper(
        acpi->GetAcpiTable, 4, 0, &rsdp, &version, &table_key);
    if (EFI_ERROR(status) || rsdp == NULL)
        return EFI_NOT_FOUND;

    return madt_patch_from_rsdp((ACPI_RSDP *)rsdp);
}
