# FGG-XSense
#
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   make

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF   := fgg-xsense.elf
BUILD := build

CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -Isrc \
          -Ithird_party/xone -Ithird_party/compat

# No libScePad: the host process has not loaded it, so calls into the module
# fault. The ioctls it wraps are issued directly instead.

SRCS := src/main.c src/pads.c src/vpad.c src/vpad_sce.c src/shell_call.c src/lock.c src/config.c src/log.c \
        src/gip/gip_proto.c src/gip/gip_wired.c \
        src/usb/ugen.c src/usb/usb_enum.c src/sha256.c \
        src/dongle/dongle.c src/dongle/mt76_fw.c \
        src/dongle/mt76.c src/dongle/mt76_boot.c src/dongle/mt76_radio.c \
        src/dongle/link.c
OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all clean test probe pad-info-probe shellui-pad-probe shellui-pad-code-probe shellui-mbus-symbol-probe shellui-mbus-code-probe shellui-mbus-request-probe hid-fops-probe kernel-bus-probe klog-pad-probe check

all: $(ELF)

$(ELF): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# -MMD records each object's headers, so changing a header rebuilds every
# object that includes it -- without it, a stale object can disagree with the
# others about an enum or a struct.
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d) $(BUILD)/tools/probe.d

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

# Read-only hardware survey for the wireless backends: see tools/probe.c.
probe: probe.elf

probe.elf: $(BUILD)/tools/probe.o $(BUILD)/src/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Read-only comparison of the public pad-information records exposed for
# physical and virtual handles.  It neither creates nor changes a pad.
pad-info-probe: pad-info-probe.elf

pad-info-probe.elf: $(BUILD)/tools/pad_info_probe.o $(BUILD)/src/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# One-shot read-only query of the current ShellUI-local pad handle recorded
# in the kernel log. It does not start, stop, or alter the resident payload.
shellui-pad-probe: shellui-pad-probe.elf

shellui-pad-probe.elf: $(BUILD)/tools/shellui_pad_probe.o $(BUILD)/src/shell_call.o $(BUILD)/src/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Copies only the loaded libScePad AddDevice implementation to /data for
# offline disassembly; it makes no ShellUI call and does not create a pad.
shellui-pad-code-probe: shellui-pad-code-probe.elf

shellui-pad-code-probe.elf: $(BUILD)/tools/shellui_pad_code_probe.o $(BUILD)/src/shell_call.o $(BUILD)/src/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Resolves MBus battery/status candidates without invoking any MBus function.
shellui-mbus-symbol-probe: shellui-mbus-symbol-probe.elf

shellui-mbus-symbol-probe.elf: $(BUILD)/tools/shellui_mbus_symbol_probe.o $(BUILD)/src/shell_call.o $(BUILD)/src/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Copies only the MBus setter's code; it never invokes the setter.
shellui-mbus-code-probe: shellui-mbus-code-probe.elf

shellui-mbus-code-probe.elf: $(BUILD)/tools/shellui_mbus_code_probe.o $(BUILD)/src/shell_call.o $(BUILD)/src/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Intercepts and immediately resumes one normal ShellUI MBus setter call.
shellui-mbus-request-probe: shellui-mbus-request-probe.elf

shellui-mbus-request-probe.elf: $(BUILD)/tools/shellui_mbus_request_probe.o $(BUILD)/src/shell_call.o $(BUILD)/src/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Opens /dev/hid read-only and snapshots only its kernel file record.
hid-fops-probe: hid-fops-probe.elf

hid-fops-probe.elf: $(BUILD)/tools/hid_fops_probe.o $(BUILD)/src/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Saves only the MBus device-list root and first record for offline comparison.
kernel-bus-probe: kernel-bus-probe.elf

kernel-bus-probe.elf: $(BUILD)/tools/kernel_bus_probe.o $(BUILD)/src/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Watches only the next 25 seconds of ShellUI/CIM kernel-log activity.
klog-pad-probe: klog-pad-probe.elf

klog-pad-probe.elf: $(BUILD)/tools/klog_pad_probe.o $(BUILD)/src/log.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Host-side tests of everything that touches no device, built with the host's
# own compiler. Named check because `test` deploys to the console.
HOSTCC ?= gcc
HOST_TESTS := tests/test_core.c tests/stubs.c src/gip/gip_proto.c src/vpad.c src/sha256.c \
              src/dongle/mt76_fw.c

check:
	@mkdir -p $(BUILD)/host
	$(HOSTCC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Isrc -Itests \
		-Ithird_party/xone -Ithird_party/compat \
		-o $(BUILD)/host/test_core $(HOST_TESTS)
	$(BUILD)/host/test_core

clean:
	rm -rf $(BUILD) $(ELF) probe.elf
