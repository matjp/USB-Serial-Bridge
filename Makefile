# GNU-EFI build for the USB HID -> Polled Mailbox Bridge
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
                   src/bridge/mailbox_writer.c \
                   src/bridge/bridge_entry.c \
                   src/bridge/tdm.c

# Input adapter (per-OS): O1-O2
ADAPTER_SRCS    := src/adapter/mailbox_reader.c \
                   src/adapter/input_inject.c

# Shared logic used by both app and bridge
COMMON_SRCS     := src/common/mailbox.c

SRCS            := $(APP_SRCS) $(BRIDGE_SRCS) $(ADAPTER_SRCS) $(COMMON_SRCS)
OBJS            := $(SRCS:src/%.c=build/%.o)

# --- Rules ------------------------------------------------------------------
.PHONY: all clean test

all: build/$(TARGET).efi

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Link into a shared ELF, then convert to PE32+ (UEFI image)
build/$(TARGET).so: $(OBJS) $(CRT0)
	$(LD) $(LDFLAGS) $(OBJS) $(CRT0) \
		-o $@ $(EFI_LIB)/libgnuefi.a

build/$(TARGET).efi: build/$(TARGET).so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel* \
		-j .rela* -j .reloc --target=efi-app-$(ARCH) $< $@

clean:
	rm -rf build

# Layer 0 host unit tests (B2, B3, B4, O1) - no UEFI, no hardware, no OS.
# See docs/architecture.md section 9, Layer 0.
test:
	$(MAKE) -C tests run
