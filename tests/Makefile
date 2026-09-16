# Host unit tests for the bridge + input adapter modules.
#
# Layer 0 (test_layer0): B2, B3, B4, O1 - pure C, hardware-independent.
# Layer 2 (test_layer2): O2 input injection against a stub input queue.
# See docs/architecture.md section 9, Layers 0 and 2.
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
#   make            - build the test binaries (build/test_layer0, build/test_layer2)
#   make run        - build and run the tests
#   make clean      - remove build artifacts

CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -g -std=c99

# Include order matters: tests/efi shadows <efi.h> with the shim.
INCLUDES := -I. -Iefi -I../include -I../src/bridge -I../src/adapter

# --- Layer 0 modules under test (B2, B3, B4, O1) + shared mailbox helpers.
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
	../src/bridge/virtual_ps2_writer.c \
	../src/adapter/mailbox_reader.c \
	../src/common/mailbox.c \
	b1_stub.c

OBJS := $(SRCS:../src/%.c=build/%.o)
OBJS := $(OBJS:b1_stub.c=build/b1_stub.o)

TARGET := build/test_layer0

# --- Layer 2 modules under test (O2 input injection) + the O1 reader that
# produces the drained stream + shared mailbox helpers. O2's OS-specific
# hook is overridden in test_layer2.c with a stub input queue.
SRCS_L2 := \
	../src/adapter/mailbox_reader.c \
	../src/adapter/input_inject.c \
	../src/common/mailbox.c

OBJS_L2 := $(SRCS_L2:../src/%.c=build/%.o)

TARGET_L2 := build/test_layer2

# --- Layer 1 fault-record test: exercises the XHCI_FAULT publish/lookup
# transport (src/bridge/xhci_fault.h) on the host. Hardware-independent.
TARGET_L1F := build/test_layer1_fault

# --- XHCI fault-injection one-shot: compiles the REAL B1 driver
# (src/bridge/xhci.c) on the host with a mock register file and drives
# bridge_poll_usb() into real failure paths. Verifies the new fault code.
TARGET_XF := build/test_xhci_fault

# --- Virtual 8042 port writer test: exercises B4 (virtual-port variant,
# src/bridge/virtual_ps2_writer.c) against a mock register file that
# overrides the weak accessors. Hardware-independent.
TARGET_VP := build/test_virtual_ps2

.PHONY: all run clean

all: $(TARGET) $(TARGET_L2) $(TARGET_L1F) $(TARGET_XF) $(TARGET_VP)

$(TARGET): $(OBJS) build/test_layer0.o
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET_L2): $(OBJS_L2) build/test_layer2.o
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET_L1F): build/test_layer1_fault.o
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET_XF): build/xhci.o build/test_xhci_fault.o
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET_VP): build/bridge/virtual_ps2_writer.o build/bridge/hid_parser.o build/bridge/hid_ps2.o build/b1_stub.o build/test_virtual_ps2.o
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

build/test_layer2.o: test_layer2.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/test_layer1_fault.o: test_layer1_fault.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/test_xhci_fault.o: test_xhci_fault.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

build/test_virtual_ps2.o: test_virtual_ps2.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# xhci.c is the real B1 driver. On the host it is compiled with the mock
# register-file overrides from test_xhci_fault.c (weak symbols).
build/xhci.o: ../src/bridge/xhci.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

run: all
	./$(TARGET)
	./$(TARGET_L2)
	./$(TARGET_L1F)
	./$(TARGET_XF)
	./$(TARGET_VP)

clean:
	rm -rf build
