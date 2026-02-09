# Technical Research: Xbox Controller LED Control on Windows

This document covers the reverse-engineering process behind xbledctl, every dead end, the approaches that almost worked, and the final breakthrough.

## The Problem

Xbox One and Series X|S controllers have a backlit Xbox button whose brightness and animation mode can be controlled over USB. On Linux, the [xone](https://github.com/medusalix/xone) driver handles this natively. On Windows, Microsoft's driver stack provides **no user-accessible API** for LED control. The Xbox Accessories app can change the brightness, but it uses a restricted WinRT interface locked behind a capability that only Microsoft-signed UWP apps can obtain.

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

## The First "Working" Approach: libusb + UsbDk

After exhausting every user-mode Windows API, the answer seemed to be bypassing the driver stack entirely.

[UsbDk](https://github.com/daynix/UsbDk) is a Windows USB filter driver developed by Daynix for QEMU/KVM USB passthrough. It installs as a filter driver alongside the existing device driver without replacing it. With UsbDk installed and a reboot, libusb can open the USB device, claim the GIP interface, and write LED packets directly to the bulk OUT endpoint.

**This worked.** The LED changed. But there was a critical problem.

### The UsbDk Problem

UsbDk detaches the stock Windows driver when libusb claims the interface. After sending the LED command and releasing the interface, `xbox_reenumerate` (a USB hub port cycle via `IOCTL_USB_HUB_CYCLE_PORT`) was needed to reattach the driver. This caused:

- A 2-3 second window where controller input was dead
- The controller "unplugged and replugged" every time you changed brightness
- A hotplug detection loop where UsbDk's driver attach would trigger a `WM_DEVICECHANGE`, which would try to re-detect the controller, which would detach again
- Occasional permanent driver corruption requiring a physical unplug/replug

The hotplug loop was eventually fixed with a cooldown timer, but the fundamental problem remained: **UsbDk breaks controller input every time you send a command.** This is unacceptable for a tool that's supposed to run in the background.

## The WinRT Restricted API Rabbit Hole

We discovered that the Xbox Accessories app uses `LegacyGipGameControllerProvider.SetHomeLedIntensity()` from `Windows.Gaming.Input.Preview`. This is the "proper" way to control the LED, but it's locked behind the `xboxAccessoryManagement` restricted capability. Only apps signed by Microsoft with that capability in their manifest can call it.

We spent a significant amount of effort trying to obtain this capability.

### 10. Sparse MSIX Package Identity

Created a sparse MSIX package with `xboxAccessoryManagement` declared in the manifest. Sparse packages give desktop apps a package identity without full UWP packaging.

The package registered successfully. `GetCurrentPackageFullName` returned `XboxLedController_1.0.0.0_x64__ph5b3h8e7ngh8`. But `SetHomeLedIntensity()` still returned Access Denied (0x80070005).

**Why it failed**: Package identity alone doesn't grant capabilities. Windows restricted capabilities are enforced through capability SIDs in the process token, and capability SIDs only exist in AppContainer process tokens. A `mediumIL` (normal desktop) process never gets capability SIDs, regardless of what the manifest declares.

### 11. AppContainer Trust Level

Changed the sparse MSIX manifest to `uap10:TrustLevel="appContainer"` to get an AppContainer token with capability SIDs.

**Result**: The exe couldn't launch at all. AppContainer processes run in a sandbox that restricts file system access to specific app data directories. Our exe tried to load from the build directory, which AppContainer can't access.

### 12. Manual AppContainer Launcher

Built `launch_appcontainer.cpp` using `CreateAppContainerProfile` + `DeriveCapabilitySidsFromName` + `CreateProcessAsUser` with `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES` to manually create an AppContainer process with the right capability SIDs.

Multiple iterations of pain:
- `ERROR_INVALID_PARAMETER` from CreateProcess - fixed by proper attribute list setup
- `ERROR_ACCESS_DENIED` - AppContainer can't access build directory files, needed `icacls` grants
- Handle inheritance broken - needed `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`
- File lock issues on every rebuild because previous processes held the exe

**Abandoned.** The complexity was spiraling and the approach was fundamentally fragile.

### 13. Invoke-CommandInDesktopPackage

Used PowerShell's `Invoke-CommandInDesktopPackage` to run our exe inside the Xbox Accessories app's package context (`Microsoft.XboxDevices`).

**Result**: Process had the Xbox Accessories package identity but ran at mediumIL, not in AppContainer. No capability SIDs in the token. Still Access Denied.

### 14. Microsoft Detours - Hooking the Capability Check

Cloned Microsoft's [Detours](https://github.com/microsoft/Detours) library and built a DLL (`cap_hook.dll`) that hooks `CheckTokenCapability` in kernelbase.dll to always return TRUE. Used `DetourCreateProcessWithDllExW` to inject the hook into our WinRT test app.

Key Detours findings:
- DLLs injected via Detours must export ordinal #1 (`DetourFinishHelperProcess @1 NONAME` in a .def file)
- Must call `DetourRestoreAfterWith()` in DllMain and guard with `DetourIsHelperProcess()`
- Without the ordinal export, you get `STATUS_DLL_NOT_FOUND` (0xC0000135)

The hook installed successfully. `CheckTokenCapability` was ready to intercept. But it was **never called**. Not once.

**Why it failed**: The `xboxAccessoryManagement` capability check doesn't happen in our process. It happens server-side in the COM activation server that hosts the GIP provider. When we call `SetHomeLedIntensity()`, the WinRT runtime marshals the call to a separate system process, and *that process* checks the caller's token for the capability SID. Hooking our own process is useless because the security check happens elsewhere.

This was a key finding: **WinRT restricted capability enforcement is server-side, not client-side.** You cannot bypass it from user space by hooking your own process.

## The Real Breakthrough: \\.\XboxGIP Direct Device Access

After all of the above, we went back to basics and looked at what device interfaces `xboxgip.sys` actually exposes. We found `\\.\XboxGIP`, a device interface that's completely separate from the per-controller GIP device paths we tried earlier.

### Discovery

```cpp
HANDLE h = CreateFileW(L"\\\\.\\XboxGIP",
    GENERIC_READ | GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    nullptr, OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
    nullptr);
```

This opens successfully with no admin rights, no special capabilities, no third-party drivers.

### The Protocol

The `\\.\XboxGIP` interface uses its own framing, different from raw USB GIP:

```c
#pragma pack(push, 1)
struct GipHeader {
    uint64_t deviceId;      // 8 bytes - identifies which controller
    uint8_t  commandId;     // 1 byte  - GIP command (0x0A for LED)
    uint8_t  clientFlags;   // 1 byte  - 0x20 for internal
    uint8_t  sequence;      // 1 byte  - packet sequence number
    uint8_t  unknown1;      // 1 byte  - always 0
    uint32_t length;        // 4 bytes - payload length
    uint32_t unknown2;      // 4 bytes - always 0
};                          // total: 20 bytes
#pragma pack(pop)
```

The critical field is `deviceId`. You can't just make one up; you need to get it from the driver.

### Getting the deviceId

After opening the handle, you need to:

1. Send IOCTL `0x40001CD0` (`GIP_ADD_REENUMERATE_CALLER_CONTEXT`) to register as a listener
2. `ReadFile` to receive device announce messages from connected controllers
3. The announce messages have `commandId` 0x01 or 0x02 and contain the `deviceId`

```cpp
DeviceIoControl(h, 0x40001CD0, nullptr, 0, nullptr, 0, &bytes, nullptr);

// Read messages until we get a device announce
uint8_t buf[4096];
OVERLAPPED ov = {};
ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

ReadFile(h, buf, sizeof(buf), &read, &ov);
// ... handle overlapped I/O ...

GipHeader *hdr = (GipHeader*)buf;
if (hdr->commandId == 0x01 || hdr->commandId == 0x02) {
    // hdr->deviceId is what we need
}
```

### Sending the LED Command

With the `deviceId` in hand, construct and write the packet:

```cpp
uint8_t payload[] = { 0x00, 0x01, brightness };  // sub-cmd, mode, intensity
uint8_t pkt[sizeof(GipHeader) + sizeof(payload)];
GipHeader *whdr = (GipHeader*)pkt;
whdr->deviceId = deviceId;
whdr->commandId = 0x0A;
whdr->clientFlags = 0x20;
whdr->sequence = 1;
whdr->length = sizeof(payload);
memcpy(pkt + sizeof(GipHeader), payload, sizeof(payload));

WriteFile(h, pkt, sizeof(pkt), &written, &wov);
```

**It worked.** The LED changed. And unlike UsbDk, **the controller input continued to work perfectly.** No driver detach, no re-enumeration, no hotplug events, no cooldown timers.

### Why This Works

The `\\.\XboxGIP` device is a multiplexed interface to the GIP driver. It's the same interface that internal Windows components (like the Xbox Accessories app's COM server) use to communicate with controllers. By sending the IOCTL to register as a reenumeration caller, we get wired into the device announcement pipeline, which gives us valid `deviceId` values that the driver will accept for command routing.

Unlike the per-controller GIP device IOCTL (0x40001CB8), which accepts commands but silently drops them without a registered GIP client, the `\\.\XboxGIP` `WriteFile` path actually forwards the command to the USB endpoint. No capability checks, no client registration beyond the initial IOCTL.

The Windows driver stack with `\\.\XboxGIP`:

```
Application
    |
    +-- CreateFileW("\\\\.\\XboxGIP")
    |       +-- DeviceIoControl(0x40001CD0)   -> register for device announces
    |       +-- ReadFile                       -> receive deviceId
    |       +-- WriteFile(GipHeader + payload) -> LED command reaches USB endpoint
    |
    +-- XInput continues to work (separate path through dc1-controller)
```

No filter drivers. No driver detachment. No third-party dependencies.

## Summary of Approaches

| # | Approach | Result |
|---|---------|--------|
| 1 | WinRT GipGameControllerProvider.SendMessage | Silently drops data |
| 2 | GIP Device IOCTL (0x40001CB8) | Validates but doesn't forward |
| 3 | HID Output Reports | No output reports exist |
| 4 | WinUSB Direct Access | Wrong driver type |
| 5 | SDL3 HIDAPI | Blocked by xboxgip on Windows |
| 6 | XInput | No LED API |
| 7 | Steam's Xbox Enhanced Features Driver | Internal to Steam |
| 8 | GIP Device WriteFile | ERROR_INVALID_FUNCTION |
| 9 | XUSB IOCTLs | XInput protocol only |
| 10 | Sparse MSIX Package Identity | Package identity != capabilities |
| 11 | AppContainer Trust Level | Can't launch from arbitrary paths |
| 12 | Manual AppContainer Launcher | Too complex, too fragile |
| 13 | Invoke-CommandInDesktopPackage | mediumIL, no capability SIDs |
| 14 | Detours CheckTokenCapability Hook | Check is server-side, not client-side |
| 15 | libusb + UsbDk | Works but breaks controller input |
| 16 | **\\.\XboxGIP WriteFile** | **Works perfectly, no side effects** |

## References

- [medusalix/xone](https://github.com/medusalix/xone) - Linux GIP driver, LED packet format
- [SDL HIDAPI Xbox One driver](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_xboxone.c) - LED constants
- [TheNathannator: GIP protocol](https://gist.github.com/TheNathannator/c5d3b41a12db739b7ffc3d8d1a87c60a)
- [TheNathannator: Windows Xbox driver interfaces](https://gist.github.com/TheNathannator/bcebc77e653f71e77634144940871596)
- [MS-GIPUSB specification](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/e7c90904-5e21-426e-b9ad-d82adeee0dbc)
- [Microsoft Detours](https://github.com/microsoft/Detours) - DLL injection library (used for capability hook experiments)
