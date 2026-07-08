# Handoff notes for the next session

## What this repo is

This is a standalone macOS virtual-controller bridge. It should remain
source-agnostic: no firmware project, emulator, or controller family should be
assumed in the core naming or architecture.

The immediate goal is a general-purpose tool that can accept arbitrary controller
input, map/calibrate it, and publish virtual HID devices that SDL, macOS HID
clients and eventually GameController-aware software can see.

## Current implementation

- CMake project with C++20/Objective-C++.
- `vhid-bridge`: command-line bridge.
- `vhid-bridge-ui.app`: basic AppKit UI that launches the bridge executable.
- UDP HID-over-UDP receiver for raw HID descriptors/reports and semantic state.
- C-compatible UDP sender helpers in `include/vhid/wire.h`.
- Physical HID source using `IOHIDManager`.
- Generic virtual HID profile builder.
- macOS virtual-device backend in `src/virtual_device_macos.mm`.
- Runtime identity overrides from both CLI and UI:
  - VID/PID/version
  - product/manufacturer/serial
  - transport hint

## Known limitations

- The UI is a launcher/monitor, not yet an embedded bridge engine.
- No saved configuration or profile store.
- No real mapping editor yet.
- No exact recognized controller output profiles yet.
- Raw-HID transparent publication is implemented only when the source opts in.
- UDP transport is unauthenticated.
- Descriptor fragmentation and reliable lifecycle acknowledgements are not
  implemented.
- Device table input counts are currently log-derived and incomplete; the bridge
  needs a structured status channel for accurate live telemetry.

## Recommended next steps

1. Add a structured bridge status channel so the UI does not scrape stdout.
2. Add persistent configuration:
   - sender/source identity
   - virtual output profile
   - mapping
   - calibration
   - identity overrides
3. Implement the first real source-decoder to semantic-state path.
4. Implement the first recognized output profile.
5. Build the mapping UI:
   - source value monitor
   - output control list
   - click-to-bind
   - stick and trigger calibration
   - rumble/LED test controls
6. Add UDP sender authentication/pairing before using non-isolated networks.

## Build/test commands

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For local entitlement testing:

```sh
codesign --force --sign - --entitlements vhid.entitlements build/vhid-bridge
build/vhid-bridge --no-physical --udp-source SENDER_IP
open build/vhid-bridge-ui.app
```

If the UI is used, make sure its bridge executable field points at the signed
`build/vhid-bridge` binary.
