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

# GNU-EFI install locations (Ubuntu: gnu-efi)
EFI_INC         ?= /usr/include/efi
EFI_LIB         ?= /usr/lib
EFI_CRT         ?= /usr/lib
# Standard GNU-EFI x86_64 linker script (elf_x86_64_efi.lds)
EFI_LDS         ?= $(EFI_LIB)/elf_x86_64_efi.lds

CC              := gcc
LD              := ld
OBJCOPY         := objcopy

# We build on GitHub Actions (Ubuntu), whose binutils ships the
# `efi-app-x86_64` objcopy target. objcopy therefore emits a valid PE32+ UEFI
# image directly from the linked .so - no post-processing is needed.

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
# -T: the standard GNU-EFI linker script (elf_x86_64_efi.lds)
# -shared: produce a relocatable PE image
LDFLAGS         := -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic \
                   -L$(EFI_LIB) -L$(EFI_CRT)

# --- Sources ----------------------------------------------------------------
# UEFI setup application (Phase 1): U1-U3 + entry point
APP_SRCS        := src/main.c \
                   src/uefi/usb_discovery.c \
                   src/uefi/core_bringup.c \
                   src/uefi/mem_reserve.c \
                   src/uefi/l1_harness.c \
                   src/uefi/log_file.c

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

# Link into a shared ELF, then convert to PE32+ (UEFI image).
# The GNU-EFI crt0 (crt0-efi-x86_64.o) MUST be linked first: it provides the
# _start entry point and the PE32+ header/relocation setup that objcopy needs
# to emit a valid UEFI image. Without it the .so links (shared libs need no
# defined entry point) but objcopy produces a malformed PE (bad signature),
# which OVMF cannot load.
build/$(TARGET).so: $(OBJS)
	$(LD) $(LDFLAGS) $(EFI_CRT)/crt0-efi-$(ARCH).o $(OBJS) \
		-o $@ $(EFI_LIB)/libefi.a $(EFI_LIB)/libgnuefi.a

build/$(TARGET).efi: build/$(TARGET).so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym \
		-j .rel -j .rela -j .rel.* -j .rela.* -j .reloc \
		--target=efi-app-$(ARCH) --subsystem=10 $< $@

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

build-debug/$(DEBUG_TARGET).so: $(DEBUG_OBJS)
	$(LD) $(LDFLAGS) $(EFI_CRT)/crt0-efi-$(ARCH).o $(DEBUG_OBJS) \
		-o $@ $(EFI_LIB)/libefi.a $(EFI_LIB)/libgnuefi.a

build-debug/$(DEBUG_TARGET).efi: build-debug/$(DEBUG_TARGET).so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym \
		-j .rel -j .rela -j .rel.* -j .rela.* -j .reloc \
		--target=efi-app-$(ARCH) --subsystem=10 $< $@

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
