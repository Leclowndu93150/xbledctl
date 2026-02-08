# Technical Research: Xbox Controller LED Control on Windows

This document covers the reverse-engineering process behind xbledctl, the dead ends, the breakthroughs, and the final working approach.

## The Problem

Xbox One and Series X|S controllers have a backlit Xbox button whose brightness and animation mode can be controlled over USB. On Linux, the [xone](https://github.com/medusalix/xone) driver handles this natively. On Windows, Microsoft's driver stack provides **no user-accessible API** for LED control. The Xbox Accessories app can change the brightness, but it uses an internal interface not available to third-party software.

We set out to change that.

## Hardware

- **Controller**: Xbox Series X|S (Model 1914)
- **VID/PID**: `045E:0B12` (USB), `045E:02FF` (child HID gamepad)
- **Firmware**: 5.23.6.0
- **Protocol**: [GIP (Game Input Protocol)](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc)

## The GIP LED Packet

From the xone Linux driver source, the LED command is 7 bytes on the wire:

```
0x0A  0x20  <seq>  0x03  0x00  <mode>  <brightness>
 |     |     |      |     |      |         +-- 0-50
 |     |     |      |     |      +-- 0x00=off, 0x01=on, 0x02-0x09=animations
 |     |     |      |     +-- sub-command (always 0)
 |     |     |      +-- payload length (3 bytes)
 |     |     +-- sequence number (1-255, incrementing)
 |     +-- flags (GIP_OPT_INTERNAL)
 +-- command ID (GIP_CMD_LED)
```

Simple enough. The hard part is getting this packet to the controller on Windows.

## What We Tried (and Why It Failed)

### 1. WinRT GipGameControllerProvider.SendMessage

The `Windows.Gaming.Input.Custom` namespace has a `GipGameControllerProvider` with a `SendMessage` method that accepts a GIP command ID, message class, and payload. By registering a custom `ICustomGameControllerFactory` with `GameControllerFactoryManager`, you can intercept controller enumeration and capture the provider.

**Result**: `SendMessage` accepts all commands and returns success, but nothing reaches the controller. The factory-captured provider is not properly wired to the USB endpoint. Even with a Win32 message pump and `DispatcherQueueController` for proper WinRT event dispatch, the provider remains non-functional.

The WinRT `Gamepad.Vibration` API also silently fails from console applications. It requires a foreground window context.

### 2. GIP Device IOCTL (0x40001CB8)

The GIP device interface (`{020BC73C-0DCA-4EE3-96D5-AB006ADA5938}`) on `dc1-controller` responds to exactly one IOCTL: `0x40001CB8`.

We discovered the correct buffer format through systematic fuzzing:

```
Offset  Size  Content
0       4     Flags (u32 LE, 0-2 accepted)
4       4     Command ID (u32 LE, e.g. 0x0A)
8       4     Payload length (u32 LE)
12      N     Full GIP packet (header + payload)
12+N    pad   Zero-padded to 256 bytes
```

**Result**: Returns `STATUS_SUCCESS`, but the data never reaches the USB endpoint. The driver validates the buffer format strictly and responds correctly, but it doesn't forward the command. This is likely because the GIP driver requires callers to register as a GIP client before commands are actually dispatched, a step that has no public API.

### 3. HID Output Reports

The child HID gamepad (`045E:02FF`) is an XInput-compatible device. We tried:
- `HidD_SetOutputReport` with various report IDs
- `WriteFile` directly to the HID handle

**Result**: `OutputReportByteLength` is 0. The HID interface exposes no output reports at all. It's strictly read-only: input reports for gamepad state and nothing else.

### 4. WinUSB Direct Access

We attempted to open the USB device interface (`{A5DCBF10-6530-11D2-901F-00C04FB951ED}`) with WinUSB to write directly to the bulk endpoint.

**Result**: `WinUsb_Initialize` returns `ERROR_INVALID_FUNCTION`. WinUSB only works with devices running the WinUSB-compatible driver. The controller uses `dc1-controller`, which is not WinUSB-compatible.

### 5. SDL3 HIDAPI

SDL's Xbox One HIDAPI driver (`SDL_hidapi_xboxone.c`) contains LED control code. We tried forcing SDL to use its HIDAPI path instead of XInput.

**Result**: On Windows, SDL always falls back to XInput for Xbox controllers because the `xboxgip` driver blocks HIDAPI access. The HIDAPI LED code is effectively dead code on Windows. It only runs on macOS and Linux.

### 6. XInput

XInput's `XInputSetState` sends rumble commands that the controller actually responds to. This is the only user-mode API we found that reaches the hardware. However, **XInput has no LED control API**. The rumble success confirmed that the controller hardware is fine and the USB connection works; the problem is entirely in the software stack.

### 7. Steam's Xbox Enhanced Features Driver

Steam installs an "Xbox Controller Enhanced Features Driver" (`oem70.inf`). We investigated whether it exposes HIDAPI access to the controller.

**Result**: The driver is installed but does not provide a separate HIDAPI-accessible interface for Xbox controllers. It appears to only function within Steam's own input pipeline.

### 8. GIP Device WriteFile

Direct `WriteFile` to the GIP device path with raw GIP packets of various sizes and formats.

**Result**: `ERROR_INVALID_FUNCTION` (1) for all attempts. The GIP device does not accept writes through the standard file I/O path.

### 9. XUSB IOCTLs

The XUSB interface (`{EC87F1E3-C13B-4100-B5F7-8B84D54260CB}`) is the XInput side. We scanned for IOCTLs that might accept LED commands.

**Result**: No XUSB IOCTLs respond to LED-related data. The interface is designed exclusively for the XInput protocol (gamepad state + rumble).

## The Breakthrough: libusb + UsbDk

After exhausting every user-mode Windows API, the answer turned out to be bypassing the driver stack entirely.

[UsbDk](https://github.com/daynix/UsbDk) is a Windows USB filter driver developed by Daynix for QEMU/KVM USB passthrough. It installs as a filter driver that sits *alongside* the existing device driver without replacing it. This is the key difference from Zadig (which replaces the driver and breaks XInput). UsbDk lets libusb access the USB device while the stock driver continues to function normally.

With UsbDk installed and a reboot (required for the filter driver to attach to USB devices), libusb can:

1. Enumerate the controller via `libusb_get_device_list`
2. Open the device with `libusb_open`
3. Auto-detach the kernel driver with `libusb_set_auto_detach_kernel_driver`
4. Claim the GIP interface with `libusb_claim_interface`
5. Write GIP packets directly to the bulk OUT endpoint with `libusb_bulk_transfer`

The LED command reaches the controller immediately. Brightness changes are visible in real-time. All LED modes (solid, blink, fade) work as documented by the xone driver.

## Why This Works When Nothing Else Did

The Windows driver stack for Xbox controllers looks like this:

```
Application
    |
    +-- XInput (xinput1_4.dll)
    |       +-- XUSB IOCTL -> dc1-controller -> USB bulk OUT (rumble only)
    |
    +-- WinRT (Windows.Gaming.Input)
    |       +-- GIP provider -> xboxgip.sys -> (dropped, no registered client)
    |
    +-- GIP IOCTL (0x40001CB8)
            +-- dc1-controller -> (validated but not forwarded)
```

XInput rumble works because `dc1-controller` has a hardcoded internal path for the rumble command to the USB endpoint. Everything else goes through layers that accept the data but never actually write it to USB. There's no public mechanism to register a GIP client that would enable the write path.

UsbDk bypasses all of this:

```
Application
    |
    +-- libusb -> UsbDk filter driver -> USB bulk OUT endpoint -> controller
```

No GIP client registration needed. No driver replacement. Just raw USB access through a filter driver.

## References

- [medusalix/xone](https://github.com/medusalix/xone) - Linux GIP driver, LED packet format
- [SDL HIDAPI Xbox One driver](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_xboxone.c) - LED constants
- [TheNathannator: GIP protocol](https://gist.github.com/TheNathannator/c5d3b41a12db739b7ffc3d8d1a87c60a)
- [TheNathannator: Windows Xbox driver interfaces](https://gist.github.com/TheNathannator/bcebc77e653f71e77634144940871596)
- [MS-GIPUSB specification](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc)
- [daynix/UsbDk](https://github.com/daynix/UsbDk) - USB filter driver for Windows
