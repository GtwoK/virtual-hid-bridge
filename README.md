# Virtual HID Bridge for macOS

Virtual HID Bridge is a source-agnostic controller bridge for macOS. It accepts
physical HID controllers and network-delivered HID reports, maps or republishes
them, then creates one virtual HID device per logical controller.

## Current state

This is an early but testable foundation:

- physical USB and Bluetooth HID discovery through `IOHIDManager`
- descriptor-backed and native-profile HID source transport over UDP on port
  `48660`
- one logical device per sender-provided device ID
- generated generic HID gamepads with configurable buttons, hats, axes,
  battery and HID Sensors-page motion
- initial Switch 1 Pro Controller and Switch 2 Pro Controller output profiles
  with Nintendo identity, native packet encoders and source haptics forwarding;
  Switch 1 uses Nintendo HD rumble motor packets, while Switch 2 Pro uses the
  known USB report-format-5 packet layout rather than the public HID `0x09`
  input path
- mapping/calibration core and unit tests
- an entitlement-sensitive macOS virtual-device publisher for signed builds
- a small AppKit UI that starts/stops the bridge, edits runtime settings and
  identity overrides, and shows bridge logs/device lifecycle

PlayStation, Xbox and other exact output profiles, player LEDs, configuration
persistence, authenticated networking and the full mapping UI are future
milestones. A recognized profile needs its real
descriptor, identity, input report layout and output protocol; changing only
the displayed name or VID/PID is not enough.

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
ad-hoc signs both `build/vhid-bridge` and the bundled helper with the included
entitlement, then signs the app bundle. Production distribution requires an
Apple-authorized provisioning profile for
`com.apple.developer.hid.virtual.device` and a real signing identity/profile
pair.

```sh
cmake --build build --target vhid-sign-local
```

The standalone terminal helper and the app-bundled helper are separate files.
Run `cmake --build build --target vhid-sign-local` again after rebuilding either
one. On a Mac that is not authorized for the restricted virtual HID entitlement,
macOS may still terminate the signed helper at launch even though
`codesign --verify` succeeds.

## Run from Terminal

Physical USB/Bluetooth controllers and local HID-over-UDP senders are enabled by
default:

```sh
build/vhid-bridge
python3 examples/send_demo.py
```

To test native Switch Pro output reports over UDP, run the bridge with the
Switch output profile and have the demo sender explicitly request Switch Pro
source output:

```sh
build/vhid-bridge --output-profile switch-pro
python3 examples/send_demo.py --source-input-profile generic-hid \
  --source-output-profile switch-pro
```

UDP senders open a session, wait for `session_accept`, add controllers with HID
descriptors, then send HID reports. For a network sender that waits for a
Mac-side session open, bind the helper on the LAN-facing interface and point it
at the sender:

```sh
build/vhid-bridge --no-physical --udp-source SENDER_IP
```

`--bind` is the local Mac listen address. When `--udp-source` is used without an
explicit `--bind`, the helper auto-binds to `0.0.0.0` so LAN senders can reply.
`--udp-source` is the remote sender address; the helper sends `session_open`
there, waits for `session_accept`, then uses idle
`session_ping`/`session_pong` keepalives. HID device/report packets travel on
the same UDP socket after the session is active. Without `--udp-source`, the
helper listens for sources that initiate `session_open`.

Useful options:

```text
--no-physical       Ignore locally connected HID controllers
--seize-physical    Request exclusive access to source controllers
--dry-run           Parse and map without creating virtual devices
--trace-rumble      Log host rumble decode and source output reports
--bind 0.0.0.0      Accept HID-over-UDP from trusted LAN devices
--listen-port PORT  Listen on a custom UDP port
--udp-source HOST   Open a UDP session to a source, HOST[:PORT]
--output-profile P  Select semantic output: generic, standard-gamepad,
                    switch-pro, switch2-pro
```

For deterministic haptics checks, build and run:

```sh
cmake --build build --target vhid-haptics-probe
build/vhid-haptics-probe
```

The probe feeds known Switch 1 HD rumble reports through the Switch 1 output
decoder and the selected source-output codec, then prints the decoded Switch 1
frequencies alongside the encoded Switch 2 USB rumble fields. With the UI or
helper already running and a virtual Switch 1 Pro visible, add `--send` to send
the same reports through macOS HID output; by default it only targets devices
created by Virtual HID Bridge.

`--output-profile` applies to physical HID sources, descriptor-backed UDP HID
sources and semantic UDP sources. The source controller descriptor defines the
incoming controls; the selected output profile defines the virtual controller
macOS and games see.

The UI's `Source/default` choice uses the source-requested profile. Descriptor-
backed UDP HID sources currently default to the generated generic profile.
`Generic HID` and `Standard Gamepad` both use that generated generic profile
until `Standard Gamepad` grows a separate fixed gamepad descriptor.

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

While the helper is running, stdin accepts `profile source`,
`profile generic`, `profile standard-gamepad`, `profile switch-pro`, and
`profile switch2-pro` to rebuild active decoded controllers with a different
virtual output profile without restarting UDP sessions or physical discovery.

## Run the UI

```sh
open build/vhid-bridge-ui.app
```

The current UI provides:

- bridge executable path
- bind address, listen port and optional UDP source
- physical-input, dry-run and network testing toggles
- output profile selection, including Switch 1 and Switch 2 Pro Controller
- live output-profile switching for decoded sources
- optional rumble trace logging
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
- UDP HID sources can select generic descriptor-backed input explicitly, ask
  the bridge to infer a known native input codec from VID/PID, or name a known
  input codec. Unknown or unimplemented input codecs fall back to generic
  descriptor decoding.
- The Switch 2 Pro native codec uses the USB packet layout also used by SDL:
  report ID `0`, 64-byte packets, buttons at bytes 5..8, sticks at bytes
  11..16 and IMU fields at bytes `0x31..0x3c`. It is intentionally separate
  from the Linux/BlueZ HID `0x09` report shape.
- Source input and source output codecs are independent. A generic-HID input
  source may explicitly request Switch Pro output reports for its return path,
  and a native source may disable source output entirely.
- Decoded UDP HID sources feed the same internal `InputState` used by physical
  controllers, then are mapped/calibrated and encoded through the selected
  virtual output profile.
- Motion over UDP uses standard HID Sensors-page acceleration and angular
  velocity fields. The bridge converts source acceleration from Gs to m/s² and
  angular velocity from degrees/sec to rad/sec internally, using SDL sensor
  axes for controller-relative motion.
- Source output reports use source-native protocols. The bridge can default a
  known source output profile from the source input profile, or a UDP source can
  explicitly announce one in `hid_device_add`; it does not infer rumble
  semantics from arbitrary vendor-defined HID output reports.

The C-compatible sender helpers in `include/vhid/wire.h` keep that envelope out
of application code. A UDP sender can usually be structured as:

```c
vhid_sender_t sender;
vhid_sender_init(&sender, device_id);
vhid_make_session_open(&sender, &session, timestamp_us, packet, sizeof(packet));
vhid_make_hid_device_add(&sender, &device, timestamp_us, packet, sizeof(packet));
vhid_make_hid_input_report(&sender, report_id, report, report_size,
                           timestamp_us, packet, sizeof(packet));
vhid_make_hid_device_remove(&sender, timestamp_us, packet, sizeof(packet));
```

The transport also supports an optional semantic message for software that
generates abstract controls and has no HID reports of its own. It feeds the
same internal mapping model but is not the primary transport.

The UDP protocol is not authenticated yet. Keep it loopback-only unless testing
on a trusted isolated LAN.

## Code boundaries

- `src/main.cpp`: source lifecycle, UDP listener and controller routing
- `src/ui_app.mm`: AppKit launcher/monitor UI
- `src/physical_hid_source_macos.mm`: physical USB/Bluetooth HID source
- `src/virtual_device_macos.mm`: entitlement-sensitive system publisher
- `src/protocol.cpp`: HID-over-UDP envelope parser/builder
- `src/source_codec.cpp`: descriptor-backed and native source input/output codecs
- `src/mapping.cpp`: button routes and per-axis calibration
- `src/generic_hid_profile.cpp`: generated build-your-own HID descriptor/reports
- `src/switch_pro_profile.cpp`: Switch 1 Pro descriptor, report encoder and host setup replies
- `src/switch2_pro_profile.cpp`: Switch 2 Pro USB report-format-5 encoder and haptics envelope
- `include/vhid/wire.h`: C-compatible sender helpers and wire constants

See `docs/protocol.md` and `docs/architecture.md` for the current design and
next implementation steps.
