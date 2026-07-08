# VHB2 protocol notes

Virtual HID Bridge Protocol v2, or VHB2, is a thin framing layer around standard
HID artifacts. It is not a replacement for HID descriptors and reports.

All multi-byte fields are little-endian. The canonical C++ definitions are in
`include/vhid/protocol.h`; the small C-compatible sender header is
`include/vhid/wire.h`.

## Message lifecycle

1. Optional `hello` from the Mac bridge to a known UDP source.
2. `hid_device_add` from the source to create or replace a logical controller.
3. Repeated `hid_input_report` messages for that controller.
4. Optional `hid_output_report` messages from the Mac bridge back to the source.
5. `hid_device_remove` when the logical controller is gone.

Every logical controller uses its own `device_id`. Multiple controllers may
share one UDP address and socket; their `device_id` values distinguish them.

## HID device add

`hid_device_add` carries:

- vendor ID, product ID and version
- transport hint
- flags
- product/manufacturer/serial strings
- complete HID report descriptor

The sender should keep `device_id` stable for the same physical controller when
possible. The serial string is the best place to carry a stable per-controller
identity for profile recall.

## HID reports

`hid_input_report`, `hid_output_report`, `hid_get_report` and
`hid_get_report_response` all carry a `HidReportHeader` plus raw report bytes.

The report ID is carried in the header. The report payload does not include a
report-ID prefix. For report-ID-less devices, the report ID is `0`.

Input reports are latest-value-wins. Lifecycle and output messages should remain
ordered.

## Transparent mode

Transparent publication is diagnostic mode, not the normal conversion path. A
raw source must set `VHID_DEVICE_ALLOW_TRANSPARENT_OUTPUT` in `hid_device_add`
for the bridge to republish its descriptor and raw input reports unchanged.

The normal route is:

1. decode source reports;
2. map/calibrate semantic controls;
3. encode the selected virtual output profile.

The bridge does not yet implement the full source-decoder/output-profile path.
That is the next large backend milestone.

## Security status

UDP pairing/authentication is not implemented yet. Treat VHB2-over-UDP as a
trusted-lab transport for now.
