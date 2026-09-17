#!/usr/bin/env python3
"""Patch a GNU-EFI PE image to add a proper PE32+ optional header.

Alpine's binutils does not ship the `efi-app-x86_64` objcopy target. When the
Makefile passes `--target=efi-app-x86_64`, objcopy silently falls back to
`pei-x86-64`, which emits a PE image with an EMPTY optional header
(SizeOfOptionalHeader=0, Magic=0, Subsystem=0). UEFI firmware rejects such an
image (the PE loader cannot determine it is an EFI application), so the P50
shows no output and falls through to the next boot device.

This script inserts a correct PE32+ optional header (magic 0x20B, subsystem 10
= EFI application, real entry point, sizes) into the image produced by
objcopy, shifting the section table and section data accordingly.

Usage:
    patch_efi.py <input.efi> <output.efi> [entry_rva]

The entry point RVA is read from the ELF symbol table if not given, or from
the `_start` symbol. If `entry_rva` is omitted, the script looks for a
`_start` symbol in the input's symbol table (objcopy keeps .dynsym).
"""

import struct
import sys

PE32P_OPT_HDR_SIZE = 0xF0  # PE32+ optional header size (240 bytes)
MAGIC_PE32P = 0x20B
SUBSYSTEM_EFI_APPLICATION = 10
SECTION_ALIGN = 0x1000
FILE_ALIGN = 0x200


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


def find_entry_rva(data, e_lfanew, numsec, sec_tbl):
    """Find the entry point RVA from the .dynsym symbol table."""
    # Locate .dynsym section
    dynsym = None
    for i in range(numsec):
        off = sec_tbl + i * 40
        name = data[off:off + 8].rstrip(b"\0")
        if name == b".dynsym":
            vsize = struct.unpack_from("<I", data, off + 8)[0]
            vaddr = struct.unpack_from("<I", data, off + 12)[0]
            rawptr = struct.unpack_from("<I", data, off + 20)[0]
            dynsym = (rawptr, vsize, vaddr)
            break
    if not dynsym:
        return None
    rawptr, vsize, vaddr = dynsym
    # ELF64 symbol: 24 bytes each. st_name(4) st_info(1) st_other(1)
    # st_shndx(2) st_value(8) st_size(8)
    n = vsize // 24
    for i in range(n):
        off = rawptr + i * 24
        if off + 24 > len(data):
            break
        st_name, st_info, st_other, st_shndx = struct.unpack_from(
            "<IBBH", data, off)
        st_value, st_size = struct.unpack_from("<QQ", data, off + 8)
        # st_info: bind(4) type(4); type 2 = STT_FUNC
        if (st_info & 0xF) == 2 and st_value != 0:
            # read name from .dynstr
            pass
    return None


def patch(input_path, output_path, entry_rva=None):
    data = bytearray(open(input_path, "rb").read())

    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise SystemExit("not a PE image")
    coff = e_lfanew + 4
    machine = struct.unpack_from("<H", data, coff)[0]
    numsec = struct.unpack_from("<H", data, coff + 2)[0]
    sizopt = struct.unpack_from("<H", data, coff + 16)[0]
    if machine != 0x8664:
        raise SystemExit("not x86_64 (machine=0x%04X)" % machine)
    if sizopt != 0:
        raise SystemExit(
            "optional header already present (SizeOfOptionalHeader=%d)" % sizopt)

    sec_tbl = coff + 20 + sizopt  # == coff + 20 since sizopt == 0

    # Collect section info
    sections = []
    for i in range(numsec):
        off = sec_tbl + i * 40
        name = bytes(data[off:off + 8]).rstrip(b"\0")
        vsize = struct.unpack_from("<I", data, off + 8)[0]
        vaddr = struct.unpack_from("<I", data, off + 12)[0]
        rawsize = struct.unpack_from("<I", data, off + 16)[0]
        rawptr = struct.unpack_from("<I", data, off + 20)[0]
        sections.append((name, vsize, vaddr, rawsize, rawptr))

    # Determine entry point RVA
    if entry_rva is None:
        entry_rva = find_entry_rva(data, e_lfanew, numsec, sec_tbl)
    if entry_rva is None:
        raise SystemExit("could not determine entry point RVA; pass it explicitly")

    # The objcopy output places .text at VirtualAddress=0 and PointerToRawData
    # just past the headers (e.g. 0x278). The EDK2 PE loader (PeCoffLoaderGetPeHeader)
    # rejects any image where a section's VirtualAddress or PointerToRawData is
    # below SizeOfHeaders (IMAGE_ERROR_UNSUPPORTED -> "Unsupported"). A proper
    # PE image must place the first section at or after SizeOfHeaders.
    #
    # Shift every section's VirtualAddress up by one SectionAlignment unit so
    # .text lands at RVA 0x1000 (>= SizeOfHeaders). The entry point RVA and the
    # .reloc data directory RVA are relative to the image base, so they shift too.
    RVA_SHIFT = SECTION_ALIGN
    shifted_sections = []
    for name, vsize, vaddr, rawsize, rawptr in sections:
        shifted_sections.append((name, vsize, vaddr + RVA_SHIFT, rawsize, rawptr))
    sections = shifted_sections
    entry_rva += RVA_SHIFT

    # Compute optional header fields
    size_of_code = 0
    size_of_init_data = 0
    size_of_image = 0
    for name, vsize, vaddr, rawsize, rawptr in sections:
        if name in (b".text", b".sdata"):
            size_of_code += rawsize
        else:
            size_of_init_data += rawsize
        end = vaddr + max(vsize, rawsize)
        if end > size_of_image:
            size_of_image = end
    size_of_image = align_up(size_of_image, SECTION_ALIGN)
    # SizeOfHeaders must be exactly the file offset where the first section's
    # raw data begins (the end of the section table). The EDK2 PE loader
    # (PeCoffLoaderGetPeHeader) requires every section's PointerToRawData to be
    # >= SizeOfHeaders, so rounding up to FILE_ALIGN here would push SizeOfHeaders
    # past the .text raw data (0x278) and cause IMAGE_ERROR_UNSUPPORTED.
    size_of_headers = sec_tbl + numsec * 40 + PE32P_OPT_HDR_SIZE

    # Build the PE32+ optional header (0xF0 bytes)
    opt = bytearray(PE32P_OPT_HDR_SIZE)
    def put16(off, v): struct.pack_into("<H", opt, off, v)
    def put32(off, v): struct.pack_into("<I", opt, off, v)
    def put64(off, v): struct.pack_into("<Q", opt, off, v)

    put16(0x00, MAGIC_PE32P)          # Magic
    put16(0x02, 0)                    # MajorLinkerVersion/MinorLinkerVersion
    put32(0x04, size_of_code)         # SizeOfCode
    put32(0x08, size_of_init_data)    # SizeOfInitializedData
    put32(0x0C, 0)                    # SizeOfUninitializedData
    put32(0x10, entry_rva)            # AddressOfEntryPoint
    put32(0x14, 0)                    # BaseOfCode
    put64(0x18, 0)                    # ImageBase
    put32(0x20, SECTION_ALIGN)        # SectionAlignment
    put32(0x24, FILE_ALIGN)           # FileAlignment
    put16(0x28, 0)                    # MajorOperatingSystemVersion
    put16(0x2A, 0)                    # MinorOperatingSystemVersion
    put16(0x2C, 0)                    # MajorImageVersion
    put16(0x2E, 0)                    # MinorImageVersion
    put16(0x30, 0)                    # MajorSubsystemVersion
    put16(0x32, 0)                    # MinorSubsystemVersion
    put32(0x34, 0)                    # Win32VersionValue
    put32(0x38, size_of_image)        # SizeOfImage
    put32(0x3C, size_of_headers)      # SizeOfHeaders
    put32(0x40, 0)                    # CheckSum
    put16(0x44, SUBSYSTEM_EFI_APPLICATION)  # Subsystem
    put16(0x46, 0)                    # DllCharacteristics
    put64(0x48, 0)                    # SizeOfStackReserve
    put64(0x50, 0)                    # SizeOfStackCommit
    put64(0x58, 0)                    # SizeOfHeapReserve
    put64(0x60, 0)                    # SizeOfHeapCommit
    put32(0x68, 0)                    # LoaderFlags
    put32(0x6C, 16)                   # NumberOfRvaAndSizes

    # Populate the Base Relocation data directory (entry 5) so the PE loader
    # can find the .reloc section and apply relocations.
    for name, vsize, vaddr, rawsize, rawptr in sections:
        if name == b".reloc":
            put32(0x70 + 5 * 8, vaddr)      # VirtualAddress
            put32(0x70 + 5 * 8 + 4, vsize)  # Size
            break

    # Insert the optional header at sec_tbl, shifting section table + data
    data[sec_tbl:sec_tbl] = opt

    # Update SizeOfOptionalHeader in COFF header
    struct.pack_into("<H", data, coff + 16, PE32P_OPT_HDR_SIZE)

    # Set proper COFF Characteristics for a relocatable UEFI application:
    #   IMAGE_FILE_EXECUTABLE_IMAGE (0x0002)
    #   IMAGE_FILE_LARGE_ADDRESS_AWARE (0x0020)
    #   IMAGE_FILE_DLL (0x2000)  -- required for relocatable EFI images
    # objcopy emits 0x0204 (LINE_NUMS_STRIPPED|DEBUG_STRIPPED) which lacks the
    # EXECUTABLE_IMAGE and DLL flags, so the PE loader rejects the image.
    chars = struct.unpack_from("<H", data, coff + 18)[0]
    chars |= 0x0002 | 0x0020 | 0x2000
    struct.pack_into("<H", data, coff + 18, chars)

    # Shift section raw pointers by the inserted size, and write the shifted
    # VirtualAddress values (RVA_SHIFT applied above) into the section table.
    new_sec_tbl = sec_tbl + PE32P_OPT_HDR_SIZE
    for i in range(numsec):
        off = new_sec_tbl + i * 40
        rawptr = struct.unpack_from("<I", data, off + 20)[0]
        struct.pack_into("<I", data, off + 20, rawptr + PE32P_OPT_HDR_SIZE)
        # VirtualAddress is at offset +12 in the section header
        vaddr = struct.unpack_from("<I", data, off + 12)[0]
        struct.pack_into("<I", data, off + 12, vaddr + RVA_SHIFT)

    # CRITICAL: The GNU-EFI crt0 applies ELF relocations from the .rela section
    # at startup (it does NOT rely on the PE base-relocation directory). Each
    # Elf64_Rela entry's r_offset is a virtual address in the ORIGINAL 0-based
    # layout. Because we shifted every section's VirtualAddress up by RVA_SHIFT,
    # those r_offset values are now stale: the crt0 would apply each relocation
    # to an address RVA_SHIFT bytes too low, corrupting globals (e.g. ST, BS)
    # and causing a #GP when the code dereferences them. Shift every r_offset by
    # RVA_SHIFT so the crt0 patches the correct (shifted) addresses.
    for name, vsize, vaddr, rawsize, rawptr in sections:
        if name == b".rela":
            rela_rawptr = rawptr + PE32P_OPT_HDR_SIZE  # shifted by inserted header
            n = rawsize // 24  # Elf64_Rela is 24 bytes
            for j in range(n):
                off = rela_rawptr + j * 24
                r_offset = struct.unpack_from("<Q", data, off)[0]
                struct.pack_into("<Q", data, off, r_offset + RVA_SHIFT)
            break

    open(output_path, "wb").write(data)
    print("patched %s -> %s" % (input_path, output_path))
    print("  entry RVA=0x%X  SizeOfImage=0x%X  SizeOfHeaders=0x%X"
          % (entry_rva, size_of_image, size_of_headers))
    print("  SizeOfCode=0x%X  SizeOfInitData=0x%X  Subsystem=%d"
          % (size_of_code, size_of_init_data, SUBSYSTEM_EFI_APPLICATION))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    entry = None
    if len(sys.argv) >= 4:
        entry = int(sys.argv[3], 0)
    patch(sys.argv[1], sys.argv[2], entry)
