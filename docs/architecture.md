# Virtual HID Bridge architecture

## There is no universal controller input packet

“USB controller,” “Bluetooth controller” and “UDP controller” describe
transports, not a common report format.

USB HID and Bluetooth HID do have a common *description mechanism*: the HID
report descriptor. The descriptor identifies controls using HID usage pages,
logical ranges, report IDs and bit layouts. Different controllers still emit
different reports. macOS parses those descriptors and exposes values as
`IOHIDElement` objects, which is the bridge's default physical-device source.

That gives us generic buttons, hats and axes without writing a packet parser
for every ordinary HID device. It does not reliably tell us semantic intent:
an `Rx` axis might be a right stick, trigger or unusual control. SDL's mapping
database, Apple GameController support and user mappings provide that missing
semantic layer.

Bluetooth Classic HID and BLE HID-over-GATT both arrive in macOS's HID stack
after pairing. Vendor-specific devices can still require initialization,
feature reports, calibration reads or proprietary motion/output parsing.

USB CDC, USB vendor-class endpoints and UDP offer no HID descriptor by
themselves. They need a small transport envelope or adapter around the HID
artifacts.

## HID-over-UDP Transport

The public sender model is HID lifecycle calls plus HID reports. A sender adds a
controller with a descriptor and identity, sends input reports, and removes the
controller when it disappears. The versioned bytes on the socket are just the
current wire envelope for those calls:

- `hid_device_add`: stable ID, identity properties and complete HID report
  descriptor
- `hid_device_remove`: stable ID
- `hid_input_report`: report type, report ID and raw input bytes
- `hid_output_report`: raw host output bytes returned to the source
- `hid_get_report` / `hid_get_report_response`: feature/input report requests
- `hello`, `ping`, `pong`: discovery/liveness foundation

The report ID is carried separately and the report payload never includes an
ID prefix. Descriptors travel at device creation; realtime packets contain only
the controller ID, report metadata, sequencing/timestamp fields and report
bytes. Complete input reports use latest-value-wins semantics, while lifecycle
and output remain ordered.

An optional semantic device/state message remains available for software that
generates abstract controls but has no HID descriptor. It feeds the internal
mapping model; it is not the universal wire representation.

The envelope remains necessary because HID itself does not define network
device lifecycle, addressing, sequencing, timestamps or report return paths.
No adopted network transport supplies those pieces for arbitrary HID:

- DSU/Cemuhook is useful for emulator adapters, but has a fixed controller
  shape and no rumble return path.
- SDL mappings normalize local devices; they are not a network transport.
- Moonlight/GameStream controller messages are part of remote-play sessions.
- OSC is a transport vocabulary without a standard gamepad schema.

Future adapters can accept DSU, OSC, WebSocket/JSON or remote-play input and
produce either raw HID or the optional semantic state.

## Processing pipeline

```text
IOHID physical source / HID-over-network / future source adapter
                         |
                         v
                 semantic InputState
                         |
                         v
             mapping + axis calibration
                         |
                         v
              selected profile encoder
                         |
                         v
       IOHIDUserDevice now / HIDVirtualDevice later
                         |
                         v
             IOHID -> SDL / GameController / raw HID
```

Each logical controller becomes a separate virtual HID device with its own
descriptor, identity, mappings and calibration.

## Relationship to ViGEmBus

ViGEmBus solves the output half of this architecture on Windows. A user-mode
feeder supplies a fixed Xbox 360 or DualShock 4 state through ViGEmClient, and
the signed kernel bus publishes a highly accurate virtual version of that
known device. It does not define how physical or network input reaches the
feeder, and it is not an arbitrary-controller wire protocol.

The equivalent boundary here is `HidProfile` plus `VirtualDevice`:

- source adapters normalize input
- mapping produces semantic controller state
- a fixed recognized profile encodes exact native reports
- the virtual-device backend publishes them to the OS

The generic descriptor-builder extends beyond ViGEmBus's two fixed targets,
while exact recognized profiles pursue the same “mimic the real device”
strategy.

## Decode, map, and output profiles

Raw HID transport does not imply clean passthrough. Normal operation is:

1. decode the source controller's reports into semantic controls;
2. apply user mappings, calibration, inversion and curves;
3. encode a newly selected virtual-controller profile.

Transparent publication of the transported source descriptor is an explicit
diagnostic mode. The protocol flag is intentionally named
`kHidAllowTransparentOutput`; a raw source without that flag is not silently
republished.

The generic profile is descriptor-driven and intended for SDL, raw HID and
software accepting ordinary generic joysticks. Vendor-defined motion fields
are available to raw-HID consumers but do not automatically become
standardized SDL or Apple GameController motion.

A recognized profile is an encoder, not a label. Each implementation must
reproduce the descriptor, identity, input reports, feature reports and output
protocol expected for that controller.

## Mapping UI

The native UI should follow the useful model from Dolphin's controller screen:

- source and virtual-output device selectors
- live values beside every output control
- click an output control, then actuate a source control to bind it
- explicit unbind, invert and reset actions
- stick plots with center, range and current value
- capture of minimum, center and maximum
- inner dead zone, outer dead zone/saturation and response curve
- trigger calibration independent from sticks
- motion orientation/sign and live sensor plots
- rumble and LED test controls
- profiles keyed by stable source identity

## Reliability and security still to add

- authenticated sender pairing and per-sender keys
- descriptor fragmentation for unusual descriptors that exceed one datagram
- lifecycle acknowledgement/retransmission
- source timeout and neutral-state fail-safe
- bounded sequence reordering and counters
- physical-device output report forwarding
- USB CDC HID-message framing
- SDL/GameController semantic source adapters
- configuration persistence and migration
