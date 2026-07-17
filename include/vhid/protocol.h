#ifndef VHID_PROTOCOL_H
#define VHID_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace vhid {

// HID-over-UDP envelope, version 2. All integer and IEEE-754 fields are
// little-endian. UDP is the first transport, but the envelope is
// transport-neutral so the same HID lifecycle messages can ride USB CDC or a
// stream framing layer later.
constexpr uint32_t kWireMagic = 0x32424856u;  // "VHB2" in little-endian memory.
constexpr uint8_t kWireVersion = 2;
constexpr uint16_t kDefaultUdpPort = 48660;
constexpr size_t kMaxDatagramSize = 1400;
constexpr size_t kMaxButtons = 64;
constexpr size_t kMaxHats = 4;
constexpr size_t kMaxAxes = 16;
constexpr uint8_t kHidSourceInputProfileInfer = 0;
constexpr uint8_t kHidSourceOutputProfileDefault = 0;
constexpr uint8_t kHidSourceInputProfileDescriptor = 0xff;
constexpr uint8_t kHidSourceOutputProfileNone = 0xff;

enum class MessageType : uint8_t {
  session_open = 1,
  session_accept = 2,
  session_close = 3,
  session_ping = 4,
  session_pong = 5,

  hid_device_add = 16,
  hid_device_remove = 17,

  hid_input_report = 32,
  hid_output_report = 33,
  hid_get_report = 34,
  hid_get_report_response = 35,

  semantic_device_add = 48,
  semantic_input_state = 49,
};

enum class HidReportType : uint8_t {
  input = 0,
  output = 1,
  feature = 2,
};

enum class HidTransport : uint8_t {
  virtual_device = 0,
  usb = 1,
  bluetooth = 2,
  bluetooth_le = 3,
  network = 4,
};

enum HidDeviceWireFlags : uint8_t {
  // Explicit diagnostic mode: publish the transported descriptor and reports
  // unchanged. Normal conversion sources are decoded and routed to a selected
  // output profile instead.
  kHidAllowTransparentOutput = 1u << 0,
};

enum class DeviceProfile : uint8_t {
  generic = 0,
  standard_gamepad = 1,
  switch_pro = 2,
  switch_2_pro = 3,
  dualshock_4 = 4,
  dualsense = 5,
  xbox = 6,
};

enum DeviceFlags : uint8_t {
  kDeviceHasBattery = 1u << 0,
  kDeviceHasMotion = 1u << 1,
  kDeviceHasRumble = 1u << 2,
  kDeviceHasPlayerLeds = 1u << 3,
  kDeviceHasRgbLed = 1u << 4,
};

enum AxisFlags : uint8_t {
  kAxisUnipolar = 1u << 0,
  kAxisInvertDefault = 1u << 1,
};

enum MotionFlags : uint8_t {
  kMotionAcceleration = 1u << 0,
  kMotionAngularVelocity = 1u << 1,
  kMotionOrientation = 1u << 2,
};

#pragma pack(push, 1)
struct MessageHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint16_t flags;
  uint32_t payload_size;
  uint32_t device_id;
  uint32_t sequence;
  uint64_t timestamp_us;
};

struct SessionPayload {
  uint32_t session_id;
  uint32_t peer_id;
  uint32_t capabilities;
  uint32_t keepalive_interval_us;
  uint32_t timeout_us;
  char name[32];
};

struct HidDeviceAddHeader {
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t version_number;
  uint8_t transport;
  uint8_t flags;
  uint16_t descriptor_size;
  uint8_t product_size;
  uint8_t manufacturer_size;
  uint8_t serial_size;
  uint8_t source_input_profile;
  uint8_t source_output_profile;
  uint8_t reserved;
};

// Report bytes never contain the report ID prefix. The ID is carried here so
// input, output, and feature reports have one unambiguous representation.
struct HidReportHeader {
  uint8_t report_type;
  uint8_t report_id;
  uint16_t data_size;
};

struct AxisDescriptor {
  uint16_t usage_page;
  uint16_t usage;
  int32_t logical_min;
  int32_t logical_max;
  uint8_t flags;
  uint8_t reserved[3];
};

struct DeviceDescription {
  uint8_t requested_profile;
  uint8_t device_flags;
  uint8_t button_count;
  uint8_t hat_count;
  uint8_t axis_count;
  uint8_t motion_flags;
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t version_number;
  char product[64];
  char manufacturer[32];
  char serial[32];
  AxisDescriptor axes[kMaxAxes];
};

// Button indices use W3C Standard Gamepad order for standard devices:
// 0 south, 1 east, 2 west, 3 north, 4/5 shoulders, 6/7 triggers,
// 8 select, 9 start, 10/11 stick clicks, 12-15 d-pad, 16 guide,
// 17 capture. Generic devices may assign their own meanings.
//
// Hat values are HID directions 0..7 clockwise from north; 8 is neutral.
// Axis values are normalized signed 16-bit. Unipolar axes use 0..32767.
// Motion uses SDL sensor axes and SI units: for a controller held in front,
// +X points right, +Y toward the top edge, and +Z closer to the player.
// Acceleration is m/s^2, angular velocity is rad/s, and orientation is
// quaternion x/y/z/w.
struct InputState {
  uint64_t buttons;
  uint8_t hats[kMaxHats];
  int16_t axes[kMaxAxes];
  float acceleration[3];
  float angular_velocity[3];
  float orientation[4];
  uint8_t battery_percent;
  uint8_t reserved[3];
};

struct HapticBandState {
  uint16_t frequency_hz;
  uint16_t amplitude;
  uint16_t encoded_frequency;
  uint16_t encoded_amplitude;
};

struct HapticMotorState {
  HapticBandState low;
  HapticBandState high;
  uint8_t switch1_hd[4];
};

enum OutputHapticFlags : uint8_t {
  kOutputHapticMotorLeft = 1u << 0,
  kOutputHapticMotorRight = 1u << 1,
  kOutputHapticSwitch1HdPacket = 1u << 2,
};

struct OutputState {
  uint16_t low_frequency;
  uint16_t high_frequency;
  uint16_t duration_ms;
  uint8_t player_leds;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t reserved;
  uint8_t haptic_flags;
  uint8_t motor_count;
  uint8_t reserved2[2];
  HapticMotorState motors[2];
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 28);
static_assert(sizeof(SessionPayload) == 52);
static_assert(sizeof(HidDeviceAddHeader) == 16);
static_assert(sizeof(HidReportHeader) == 4);
static_assert(sizeof(AxisDescriptor) == 16);
static_assert(sizeof(InputState) == 88);

struct ParsedMessage {
  const MessageHeader* header = nullptr;
  std::span<const uint8_t> payload;
};

struct ParsedHidDeviceAdd {
  const HidDeviceAddHeader* header = nullptr;
  std::span<const uint8_t> descriptor;
  std::string_view product;
  std::string_view manufacturer;
  std::string_view serial;
};

struct ParsedHidReport {
  const HidReportHeader* header = nullptr;
  std::span<const uint8_t> data;
};

bool host_is_little_endian();
bool valid_device_description(const DeviceDescription& description);
bool parse_message(std::span<const uint8_t> bytes, ParsedMessage& out);
bool parse_hid_device_add(std::span<const uint8_t> payload,
                          ParsedHidDeviceAdd& out);
bool parse_hid_report(std::span<const uint8_t> payload,
                      ParsedHidReport& out);

std::vector<uint8_t> make_hid_device_add(
    uint32_t device_id, uint32_t sequence, uint64_t timestamp_us,
    const HidDeviceAddHeader& device,
    std::span<const uint8_t> descriptor, std::string_view product,
    std::string_view manufacturer, std::string_view serial);
std::vector<uint8_t> make_hid_report(
    MessageType type, uint32_t device_id, uint32_t sequence,
    uint64_t timestamp_us, HidReportType report_type, uint8_t report_id,
    std::span<const uint8_t> data);

template <typename Payload>
std::array<uint8_t, sizeof(MessageHeader) + sizeof(Payload)> make_message(
    MessageType type, uint32_t device_id, uint32_t sequence,
    uint64_t timestamp_us, const Payload& payload, uint16_t flags = 0) {
  std::array<uint8_t, sizeof(MessageHeader) + sizeof(Payload)> out{};
  MessageHeader header{kWireMagic, kWireVersion, static_cast<uint8_t>(type),
                       flags, sizeof(Payload), device_id, sequence,
                       timestamp_us};
  std::memcpy(out.data(), &header, sizeof(header));
  std::memcpy(out.data() + sizeof(header), &payload, sizeof(payload));
  return out;
}

}  // namespace vhid

#endif
