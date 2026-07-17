# HID-over-UDP Transport Notes

The UDP transport is meant to feel like HID plus lifecycle calls:

1. add a controller with its HID report descriptor and identity;
2. send ordinary HID input reports;
3. remove the controller when it disappears.

VHB2 is the internal wire version for the small envelope around those HID
artifacts. It is not a replacement for HID descriptors and reports, and sender
code should not need to treat it as a new controller format.

All multi-byte fields are little-endian. The canonical C++ definitions are in
`include/vhid/protocol.h`; the small C-compatible sender header is
`include/vhid/wire.h`.

Message type values are grouped by role: session control, HID device lifecycle,
HID reports, then optional semantic controller state.

## Sender-facing API

Small C/C++ senders should prefer the helper functions in `include/vhid/wire.h`
instead of constructing the envelope by hand:

```c
vhid_sender_t sender;
vhid_sender_init(&sender, device_id);
vhid_make_session_open(&sender, &session, timestamp_us, packet, sizeof(packet));
vhid_make_hid_device_add(&sender, &device, timestamp_us, packet, sizeof(packet));
vhid_make_hid_input_report(&sender, report_id, report, report_size,
                           timestamp_us, packet, sizeof(packet));
vhid_make_hid_device_remove(&sender, timestamp_us, packet, sizeof(packet));
```

The caller still owns the UDP socket. Each helper returns the datagram size to
send, or `0` if the packet would be invalid or too large.

## Session lifecycle

UDP traffic is sessionful. The usual flow is:

1. `session_open` from the source to the Mac bridge.
2. `session_accept` from the Mac bridge with the same session ID.
3. HID lifecycle/report traffic inside that session.
4. `session_ping`/`session_pong` only after the session is idle.
5. `session_close` or timeout when the peer goes away.

The bridge tracks each joining endpoint as its own session. HID messages from
an endpoint are ignored until that endpoint has an active session, and
`session_close` or timeout removes the controllers owned by that session.

When `--udp-source` is set, the Mac bridge may also open the session toward a
known remote source. In that mode, traffic from other UDP endpoints is ignored.

## Controller lifecycle

1. `hid_device_add` from the source to create or replace a logical controller.
2. Repeated `hid_input_report` messages for that controller.
3. Optional `hid_output_report` messages from the Mac bridge back to the source.
4. `hid_device_remove` when the logical controller is gone.

Every logical controller uses its own `device_id` within its session. Multiple
controllers may share one UDP address and socket; their `device_id` values
distinguish them inside that session.

## HID device add

`hid_device_add` carries:

- vendor ID, product ID and version
- transport hint
- flags
- source input profile hint
- optional source output profile hint
- product/manufacturer/serial strings
- complete HID report descriptor

The sender should keep `device_id` stable for the same physical controller when
possible. The serial string is the best place to carry a stable per-controller
identity for profile recall.

`source_input_profile` selects how input reports are decoded. `0` asks the
bridge to infer a known native input codec from VID/PID. `0xff` forces generic
descriptor-backed HID input. Other non-zero values use the `DeviceProfile`
numeric IDs. If the requested or inferred input codec is unknown or not
implemented, the bridge falls back to generic descriptor-backed HID input.

`source_output_profile` selects the optional source-side feedback protocol.
`0` defaults to the source input profile when that profile is known natively.
`0xff` disables source output. Other non-zero values use the `DeviceProfile`
numeric IDs, so a generic input descriptor can explicitly ask to receive Switch
Pro output reports by setting this field to `2`, or Switch 2 Pro output reports
by setting it to `3`.

## HID reports

`hid_input_report`, `hid_output_report`, `hid_get_report` and
`hid_get_report_response` carry a `HidReportHeader` plus raw report bytes.
The descriptor and identity from `hid_device_add` are not repeated on every
input report.

The report ID is carried in the header. The report payload does not include a
report-ID prefix. For report-ID-less devices, the report ID is `0`.

Input reports are latest-value-wins. Lifecycle and output messages should remain
ordered.

Descriptor-backed generic motion sources should use the HID Sensors page
(`0x20`) for acceleration and angular velocity fields. The bridge accepts the
standard acceleration X/Y/Z and angular velocity X/Y/Z usages, applies the HID
unit exponent, treats the default units as Gs and degrees/sec, then stores
m/s² and rad/sec internally using SDL sensor axes: for a controller held in
front, +X points right, +Y toward the top edge, and +Z closer to the player.
Native input codecs own their whole source report contract, including buttons,
hats, sticks, battery and motion.

Switch 2 Pro native input uses the USB packet layout also used by SDL: report
ID `0`, a 64-byte payload, button bytes at offsets 5..8, 12-bit sticks at
offsets 11 and 14, and IMU samples at offsets `0x31..0x3c`. This is not the
public HID `0x09` report layout used by Linux/BlueZ experiments.

Source output reports are source-native. If the selected virtual output profile
and source output profile match, the bridge forwards the profile-native output
report bytes to the source. Cross-profile haptics are decoded from the virtual
output profile into internal motor state, then encoded by the source output
codec for the announced source profile. The bridge does not assign rumble
meaning to arbitrary vendor-defined HID output reports.

## Transparent mode

Transparent publication is diagnostic mode, not the normal conversion path. A
raw source must set `VHID_DEVICE_ALLOW_TRANSPARENT_OUTPUT` in `hid_device_add`
for the bridge to republish its descriptor and raw input reports unchanged.

The normal route is:

1. decode source reports using the descriptor announced at attach time;
2. map/calibrate semantic controls;
3. encode the selected virtual output profile.

The source identity and descriptor define the source-side controller type. The
selected output profile defines the virtual device seen by macOS and games.
Those two identities are independent.

## Security status

UDP pairing/authentication is not implemented yet. Treat HID-over-UDP as a
trusted-lab transport for now.
