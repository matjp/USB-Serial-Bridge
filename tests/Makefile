# Host unit tests for the Layer 0 bridge modules (B2, B3, B4, O1).
#
# These modules are pure C and hardware-independent, so they can be compiled
# and run on the host (no UEFI, no hardware, no OS). See
# docs/architecture.md section 9, Layer 0.
#
# The modules are written against GNU-EFI types (<efi.h>). For the host test
# build we shadow <efi.h> with tests/efi/efi.h (a type shim) by putting
# tests/efi FIRST on the include path. The project headers (hid.h, ps2.h,
# mailbox.h) come from ../include, and the internal headers (bridge.h,
# adapter.h, hid_event.h) come from ../src.
#
# This Makefile is intended to be run from the tests/ directory
# (e.g. `make -C tests run`), so all paths are relative to tests/.
#
# Usage:
#   make            - build the test binary (build/test_layer0)
#   make run        - build and run the tests
#   make clean      - remove build artifacts

CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -g -std=c99

# Include order matters: tests/efi shadows <efi.h> with the shim.
INCLUDES := -I. -Iefi -I../include -I../src/bridge -I../src/adapter

# Layer 0 modules under test (B2, B3, B4, O1) + shared mailbox helpers.
#
# b1_stub.c is a host-only stub that defines the raw HID report globals
# (g_raw_kbd, g_raw_mouse, g_kbd_valid, g_mouse_valid). In the firmware build
# these are owned by B1 (src/bridge/xhci.c), which is NOT compiled here
# because it drives real XHCI MMIO and cannot run on the host. See
# b1_stub.c for details.
SRCS := \
	../src/bridge/hid_parser.c \
	../src/bridge/hid_ps2.c \
	../src/bridge/mailbox_writer.c \
	../src/adapter/mailbox_reader.c \
	../src/common/mailbox.c \
	b1_stub.c

OBJS := $(SRCS:../src/%.c=build/%.o)
OBJS := $(OBJS:b1_stub.c=build/b1_stub.o)

TARGET := build/test_layer0

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(OBJS) build/test_layer0.o
	$(CC) $(CFLAGS) -o $@ $^

build/%.o: ../src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/b1_stub.o: b1_stub.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/test_layer0.o: test_layer0.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

run: all
	./$(TARGET)

clean:
	rm -rf build
