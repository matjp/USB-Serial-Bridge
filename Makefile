# GNU-EFI build for the USB HID -> Virtual 8042 Port Bridge
# Target: x86_64 UEFI application (PE32+)
#
# Produces:
#   build/bridge.efi   - the UEFI setup application (Phase 1 boot-time setup)
#
# The bridge core code (B1-B5) is linked into the same image; at runtime the
# UEFI app copies it into the reserved region and starts the highest core via
# SIPI (see docs/architecture.md).

ARCH            := x86_64
TARGET          := bridge
DEBUG_TARGET    := bridge-debug

# GNU-EFI install locations (Alpine: gnu-efi-dev)
EFI_INC         ?= /usr/include/efi
EFI_LIB         ?= /usr/lib
EFI_CRT         ?= /usr/lib

# Project-local PIC crt0. Alpine's prebuilt crt0-efi-x86_64.o is compiled
# WITHOUT -fPIC and cannot be linked with -shared (PC32 relocations against
# ImageBase/_DYNAMIC fail). We rebuild it from the GNU-EFI source with -fPIC
# (see gnuefi/README.md) and use that here.
CRT0            := gnuefi/crt0-efi-$(ARCH).o

CC              := gcc
LD              := ld
OBJCOPY         := objcopy
NM              := nm
PATCH_EFI       := python3 tools/patch_efi.py

# Alpine's binutils does not ship the `efi-app-x86_64` objcopy target, so
# objcopy silently falls back to `pei-x86-64` and emits a PE image with an
# EMPTY optional header (Magic=0, Subsystem=0, EntryPoint=0). UEFI firmware
# rejects such an image (the P50 showed no output and fell through to the next
# boot device). tools/patch_efi.py inserts a correct PE32+ optional header and
# fixes the section headers so the image passes the EDK2 PE loader validation.
# The entry point RVA is read from the `_start` symbol in the ELF .so.
#
# On distros whose binutils DOES ship the efi-app-x86_64 target (e.g. Ubuntu,
# used by the GitHub Actions CI), objcopy already emits a valid PE32+ image, so
# the patch step is skipped automatically (see efi_needs_patch below).
define efi_entry_rva
	$(shell $(NM) $1 2>/dev/null | awk '$$3=="_start"{print "0x"$$1}')
endef

# Return "yes" if the objcopy output lacks a valid PE32+ optional header and
# therefore needs patching. Alpine's binutils (no efi-app-x86_64 target) emits
# a PE with SizeOfOptionalHeader==0, so the section table starts immediately
# after the 20-byte COFF header. A proper UEFI image has a non-zero optional
# header (PE32+ Magic 0x20B). Reads e_lfanew at file offset 0x3C, then the
# COFF SizeOfOptionalHeader at e_lfanew+4+16.
define efi_needs_patch
	$(shell python3 -c "import struct,sys; d=open('$1','rb').read(); e=struct.unpack_from('<I',d,0x3C)[0]; o=struct.unpack_from('<H',d,e+4+16)[0] if e+24<=len(d) else 0; sys.exit(0 if o>0 else 1)" 2>/dev/null && echo no || echo yes)
endef

# --- Compiler flags ---------------------------------------------------------
# -ffreestanding: no hosted runtime assumptions
# -fno-stack-protector / -fno-stack-check: no compiler-inserted stack canaries
# -fno-stack-clash-protection: no stack-clash probes
# -fpic: GNU-EFI links with -shared to produce a relocatable PE32+ image, so
#        code must be position-independent (the loader relocates it at runtime)
# -mno-red-zone: UEFI ABI forbids the red zone (interrupts may use the stack)
# -maccumulate-outgoing-args: avoid push/pop of args on the hot path
# -fshort-wchar: CHAR16 is 16-bit
# -Wall -Wextra: keep the code clean
CFLAGS          := -I$(EFI_INC) -I$(EFI_INC)/$(ARCH) -Iinclude \
                   -fno-stack-protector -fno-stack-check -fno-stack-clash-protection \
                   -fpic -mno-red-zone -maccumulate-outgoing-args \
                   -fshort-wchar -ffreestanding -fno-builtin \
                   -Wall -Wextra -O2 -g

# --- Linker flags -----------------------------------------------------------
# -nostdlib: no host crt0/libc
# -T: our linker script
# -shared: produce a relocatable PE image
LDFLAGS         := -nostdlib -znocombreloc -T linker.lds -shared -Bsymbolic \
                   -L$(EFI_LIB) -L$(EFI_CRT)

# --- Sources ----------------------------------------------------------------
# UEFI setup application (Phase 1): U1-U3 + entry point
APP_SRCS        := src/main.c \
                   src/uefi/usb_discovery.c \
                   src/uefi/core_bringup.c \
                   src/uefi/mem_reserve.c \
                   src/uefi/l1_harness.c

# Bridge core code (Phase 2): B1-B5
BRIDGE_SRCS     := src/bridge/xhci.c \
                   src/bridge/hid_parser.c \
                   src/bridge/hid_ps2.c \
                   src/bridge/virtual_ps2_access.c \
                   src/bridge/virtual_ps2_writer.c \
                   src/bridge/bridge_entry.c \
                   src/bridge/tdm.c

# Input adapter code (O1): the consumer-side virtual port reader, loaded
# into the reserved region on core 0 (see docs/architecture.md section 7.2).
ADAPTER_SRCS    := src/adapter/virtual_ps2_reader.c

SRCS            := $(APP_SRCS) $(BRIDGE_SRCS) $(ADAPTER_SRCS)
OBJS            := $(SRCS:src/%.c=build/%.o)

# --- Rules ------------------------------------------------------------------
.PHONY: all clean test debug

all: build/$(TARGET).efi

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Link into a shared ELF, then convert to PE32+ (UEFI image)
build/$(TARGET).so: $(OBJS) $(CRT0)
	$(LD) $(LDFLAGS) $(OBJS) $(CRT0) \
		-o $@ $(EFI_LIB)/libefi.a $(EFI_LIB)/libgnuefi.a

build/$(TARGET).efi: build/$(TARGET).so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel* \
		-j .rela* -j .reloc --target=efi-app-$(ARCH) $< $@
	@if [ "$(strip $(call efi_needs_patch,$@))" = "yes" ]; then \
		$(PATCH_EFI) $@ $@ $(call efi_entry_rva,$<); \
	else \
		echo "objcopy produced a valid PE32+ image; skipping patch"; \
	fi

# --- Debug build ------------------------------------------------------------
# Produces build/bridge-debug.efi with -DBRIDGE_DEBUG. The bridge records the
# actual XHCI hardware state on a successful bring-up and the Layer 1 harness
# prints it to the console (see src/bridge/xhci_status.h). The normal build is
# unchanged (silent on success).
DEBUG_CFLAGS := $(CFLAGS) -DBRIDGE_DEBUG
DEBUG_OBJS   := $(SRCS:src/%.c=build-debug/%.o)

build-debug/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(DEBUG_CFLAGS) -c $< -o $@

build-debug/$(DEBUG_TARGET).so: $(DEBUG_OBJS) $(CRT0)
	$(LD) $(LDFLAGS) $(DEBUG_OBJS) $(CRT0) \
		-o $@ $(EFI_LIB)/libefi.a $(EFI_LIB)/libgnuefi.a

build-debug/$(DEBUG_TARGET).efi: build-debug/$(DEBUG_TARGET).so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel* \
		-j .rela* -j .reloc --target=efi-app-$(ARCH) $< $@
	@if [ "$(strip $(call efi_needs_patch,$@))" = "yes" ]; then \
		$(PATCH_EFI) $@ $@ $(call efi_entry_rva,$<); \
	else \
		echo "objcopy produced a valid PE32+ image; skipping patch"; \
	fi

debug: build-debug/$(DEBUG_TARGET).efi

# --- Bootable USB layout ----------------------------------------------------
# UEFI firmware auto-boots removable media from the spec-mandated default
# path \EFI\BOOT\BOOTX64.EFI (x86_64). The P50 has no UEFI Shell, so this
# target stages the debug image under that path in the build dir. Copy the
# whole build/bootable/ tree to the root of a FAT32 USB stick.
#
#   make bootable            -> build/bootable/EFI/BOOT/BOOTX64.EFI (debug)
#   make bootable DEBUG=0    -> build/bootable/EFI/BOOT/BOOTX64.EFI (normal)
BOOTABLE_DIR := build/bootable
BOOTABLE_EFI := $(BOOTABLE_DIR)/EFI/BOOT/BOOTX64.EFI

.PHONY: bootable

bootable: build-debug/$(DEBUG_TARGET).efi build/$(TARGET).efi
	@mkdir -p $(BOOTABLE_DIR)/EFI/BOOT
	@if [ "$(DEBUG)" = "0" ]; then \
		cp build/$(TARGET).efi $(BOOTABLE_EFI); \
		echo "Staged normal build -> $(BOOTABLE_EFI)"; \
	else \
		cp build-debug/$(DEBUG_TARGET).efi $(BOOTABLE_EFI); \
		echo "Staged debug build   -> $(BOOTABLE_EFI)"; \
	fi
	@echo "Copy the $(BOOTABLE_DIR)/ tree to the root of a FAT32 USB stick."

clean:
	rm -rf build build-debug

# Layer 0 host unit tests (B2, B3, B4, O1) - no UEFI, no hardware, no OS.
# See docs/architecture.md section 9, Layer 0.
test:
	$(MAKE) -C tests run
