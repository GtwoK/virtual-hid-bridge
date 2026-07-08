# Virtual HID Bridge for macOS

Virtual HID Bridge is a source-agnostic controller bridge for macOS. It accepts
physical HID controllers and network-delivered HID reports, maps or republishes
them, then creates one virtual HID device per logical controller.

## Current state

This is an early but testable foundation:

- physical USB and Bluetooth HID discovery through `IOHIDManager`
- descriptor-and-raw-report HID over UDP on port `48660`
- one logical device per sender-provided device ID
- generated generic HID gamepads with configurable buttons, hats, axes,
  battery, vendor-defined motion and rumble output
- mapping/calibration core and unit tests
- an entitlement-sensitive macOS virtual-device publisher for signed builds
- a small AppKit UI that starts/stops the bridge, edits runtime settings and
  identity overrides, and shows bridge logs/device lifecycle

Recognized Switch, PlayStation, Xbox and other exact output profiles,
configuration persistence, authenticated networking and the full mapping UI are
future milestones. A recognized profile needs its real descriptor, identity,
input report layout and output protocol; changing only the displayed name or
VID/PID is not enough.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The command-line bridge is `build/vhid-bridge`. The UI app bundle is
`build/vhid-bridge-ui.app`. The app bundle embeds its own bridge helper at
`build/vhid-bridge-ui.app/Contents/MacOS/vhid-bridge`, and the UI launches that
bundled helper by default.

For local entitlement testing, build the signing target after each build. It
ad-hoc signs the bundled helper with the included entitlement, then signs the
app bundle. Production distribution requires an Apple-authorized provisioning
profile for `com.apple.developer.hid.virtual.device` and a real signing
identity/profile pair.

```sh
cmake --build build --target vhid-sign-local
```

If you run `build/vhid-bridge` directly from Terminal instead of through the
app, that standalone executable still needs to be signed with the virtual HID
entitlement separately.

## Run from Terminal

Physical USB/Bluetooth controllers and local HID-over-UDP senders are enabled by
default:

```sh
build/vhid-bridge
python3 examples/send_demo.py
```

For a network sender that waits for a Mac-side UDP transport HELLO, bind the
helper on the LAN-facing interface and point it at the sender:

```sh
build/vhid-bridge --no-physical --udp-source SENDER_IP
```

`--bind` is the local Mac listen address. When `--udp-source` is used without an
explicit `--bind`, the helper auto-binds to `0.0.0.0` so LAN senders can reply.
`--udp-source` is the remote sender address; the helper sends a transport HELLO
there once per second and receives HID device/report packets back on the same UDP
socket.

Useful options:

```text
--no-physical       Ignore locally connected HID controllers
--seize-physical    Request exclusive access to source controllers
--dry-run           Parse and map without creating virtual devices
--bind 0.0.0.0      Accept HID-over-UDP from trusted LAN devices
--listen-port PORT  Listen on a custom UDP port
--udp-source HOST   Send a UDP transport HELLO to a source, HOST[:PORT]
```

Virtual-device identity overrides are available from both Terminal and the UI:

```text
--override-vendor-id N
--override-product-id N
--override-version N
--override-product NAME
--override-manufacturer NAME
--override-serial TEXT
--override-transport virtual|usb|bluetooth|ble|network
```

Numbers may be decimal or `0x` hexadecimal. Blank UI fields mean “use the source
device value or the profile default.”

When attached to a terminal, press `q` then Enter to quit; `Ctrl-C` and
`SIGTERM` also work.

## Run the UI

```sh
open build/vhid-bridge-ui.app
```

The current UI provides:

- bridge executable path
- bind address, listen port and optional UDP source
- physical-input, dry-run and network testing toggles
- VID/PID/version/product/manufacturer/serial/transport identity overrides
- a device table and bridge log pane

The UI does not yet persist profiles or edit per-button/per-axis mappings. The
next UI milestone is a real mapping/configuration editor with live values,
calibration and per-controller profiles.

## Input Formats

There is no universal packet layout shared by USB, Bluetooth and UDP.

- USB and Bluetooth HID devices provide a HID report descriptor describing their
  raw reports. macOS parses that descriptor into `IOHIDElement` values. The
  bridge consumes those elements, so ordinary HID devices do not need a
  hard-coded packet parser.
- Vendor-specific controllers may require initialization, feature reports or
  interpretation beyond their generic HID elements. Those belong in optional
  source profiles or an SDL/GameController-based source adapter.
- USB CDC and USB vendor-class devices are byte transports, not game
  controllers. Their sender wraps HID descriptors and raw reports in the bridge
  HID-over-UDP envelope.
- UDP has no descriptor or controller schema. A sender adds a controller once
  with its HID descriptor and identity, then sends ordinary HID input reports.
  The per-report envelope only carries the controller ID, report type, report
  ID, sequence and timestamp.

The C-compatible sender helpers in `include/vhid/wire.h` keep that envelope out
of application code. A UDP sender can usually be structured as:

```c
vhid_sender_t sender;
vhid_sender_init(&sender, device_id);
vhid_make_hid_device_add(&sender, &device, timestamp_us, packet, sizeof(packet));
vhid_make_hid_input_report(&sender, report_id, report, report_size,
                           timestamp_us, packet, sizeof(packet));
vhid_make_hid_device_remove(&sender, timestamp_us, packet, sizeof(packet));
```

The transport also retains an optional semantic message for software that
generates abstract controls and has no HID reports of its own. It feeds the same
internal mapping model but is not the primary transport.

The UDP protocol is not authenticated yet. Keep it loopback-only unless testing
on a trusted isolated LAN.

## Code boundaries

- `src/main.cpp`: source lifecycle, UDP listener and controller routing
- `src/ui_app.mm`: AppKit launcher/monitor UI
- `src/physical_hid_source_macos.mm`: physical USB/Bluetooth HID source
- `src/virtual_device_macos.mm`: entitlement-sensitive system publisher
- `src/protocol.cpp`: HID-over-UDP envelope parser/builder
- `src/mapping.cpp`: button routes and per-axis calibration
- `src/generic_hid_profile.cpp`: generated build-your-own HID descriptor/reports
- `include/vhid/wire.h`: C-compatible sender helpers and wire constants

See `docs/handoff.md`, `docs/protocol.md` and `docs/architecture.md` for the
current design and next implementation steps.
