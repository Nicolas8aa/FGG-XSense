# PS5 virtual-pad battery overlay research

Status: evidence log and research hand-off; no battery-overlay implementation
is claimed here.

## Goal

FGG-XSense receives Xbox GIP battery status correctly, but the PS-button
overlay shows the virtual Xbox pad as fully charged.  The working hypothesis
is that the overlay is using PS5 MBus battery capabilities and events rather
than FGG-XSense's input report.

## Reproducible console observations

All observations below were made on the same console with a real DualSense and
an FGG-XSense virtual DualSense present.

* FGG-XSense decodes Xbox GIP battery status (for example, `low` and
  `normal`) from the wireless adapter.
* A virtual pad created through `scePadVirtualDeviceAddDevice` is announced as
  `type:1`, `subType:22`, and `capabilityBattery:0` in the kernel log.
* ShellUI opens it as a DualSense and accepts its PS and ordinary input after
  account assignment.
* A real DualSense is announced by ShellUI as `DUALSENSE`; ShellUI's public
  `scePadGetControllerInformation` record for it is byte-for-byte the same as
  the virtual pad's successful record:

  ```text
  A4 70 33 42 80 07 38 04 0D 0D 00 01 01 00 00 00
  00 00 00 00 00 00 00 00 00 00 00 00
  ```

  This is touchpad density/resolution, dead zones, connection and class
  information; it contains no battery field.
* `scePadGetExtControllerInformation` returns `0x80920101` for both handles,
  so it is not a usable battery channel on this firmware.

## `libScePad` AddDevice behavior

The loaded `scePadVirtualDeviceAddDevice` code was copied from ShellUI with a
read-only ptrace snapshot and disassembled locally.  No proprietary code or
binary dump is stored in this repository.

The implementation zeroes a 168-byte `/dev/hid` descriptor and writes only:

| Descriptor offset | Source |
| --- | --- |
| `+0x00` | first byte from an internal device-type lookup table |
| `+0x1c` | first 32-bit word of the caller block |
| `+0x20` | second byte from that lookup table |

The ioctl is `0xC018482A` (`HID_ADD_DEVICE`).  No caller-controlled battery or
capability field is propagated from the 32-byte public AddDevice block.  This
rules out a safe public-API parameter change as a battery-overlay fix.

## MBus direction

Local PS5 log evidence for a battery-capable real DualSense shows:

1. `SCE_MBUS_EVENT_DEVICE_ADDED ... capabilityBattery:1`
2. subsequent device-function events of type `18`
3. a reported battery status and level

This makes an MBus battery function/event the most promising remaining route.
The SDK-exported `KERNEL_ADDRESS_BUS_DATA_DEVICES` was snapshotted read-only,
but it is the generic kernel device registry rather than the live MBus
controller registry, so it did not contain either known pad device ID.

Local reconnect capture subsequently confirmed the same sequence on the test
console:

```text
DEVICE_ADDED ... capabilityBattery:1
DEVICE_STATUS_UPDATED [type=0]
DEVICE_FUNCTION_CHANGED [type:18][state:0][action:2]
```

`sceMbusSetDeviceFunctionState` is exported by ShellUI's `libSceMbus`.  A
one-call, self-restoring breakpoint capture showed ShellUI calls it with the
physical device ID (`0x2b0300` in that capture) as its first argument.  Its
export wrapper consumes only that device-ID argument; it is therefore not a
public battery-level setter.  An earlier attempt to interpret memory at that
numeric device ID as a request pointer was discarded as invalid evidence.

Most importantly, a fresh FGG-XSense virtual-pad creation was captured after
the console restart.  It already emits the same function event as the real
DualSense:

```text
DEVICE_ADDED ... capabilityBattery:0
DEVICE_FUNCTION_CHANGED [type:18][state:0][action:2]
```

Therefore the missing overlay battery is not caused by an omitted type-18
function-state call.  The remaining difference is the immutable add-time
`capabilityBattery` property (and the physical device's ensuing status
update), which points back to the HID descriptor/kernel registration path.

The post-reboot Xbox reconnect independently reproduced that result for a
fresh virtual device (`0x5030d`): it was bound to the active user and emitted
the type-18 event, while retaining `capabilityBattery:0`.  Repeated pre-reboot
recreations showed the same invariant.  The MBus route is therefore exhausted
as a producer path, not merely untested.

## Safe next experiments

The only remaining technical route is a dedicated raw `/dev/hid` descriptor
experiment.  There is currently no published or locally-derived descriptor
field to target, so blind mutation would be low-information and can interrupt
controller assignment.  Do this only in an isolated one-pad session with a
recovery/reboot path, and only after identifying a concrete candidate field.

## Kernel-handler inspection boundary

A read-only `/dev/hid` file-record snapshot identified candidate kernel
file-operation data and a likely ioctl handler address.  A follow-up snapshot
of that handler did not complete before the console powered down/restarted.
There is no proof that the read caused the restart, but repeating this kind of
kernel-code snapshot is not justified on the test console.  Keep further work
at documented userland/Mbus boundaries or in isolated test sessions.

## Upstream value

These results are useful to PS5 compatibility-library projects such as
AnyPS5 as behavior documentation and test cases:

* the public controller-information path has no working battery value here;
* extended controller information is unsupported on this firmware;
* virtual AddDevice descriptor construction has only the three writes above;
* virtual-device account assignment can remain pending until ShellUI accepts
  the new pad's own PS report.

The evidence is interoperability-oriented and deliberately excludes copied
Sony executable code and console-memory snapshots.
