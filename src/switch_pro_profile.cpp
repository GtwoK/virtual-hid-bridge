#include "vhid/hid_profile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace vhid {
namespace {

constexpr uint8_t kInputSubcommandReply = 0x21;
constexpr uint8_t kInputFull = 0x30;
constexpr uint8_t kInputUsbResponse = 0x81;
constexpr uint8_t kOutputRumbleSubcommand = 0x01;
constexpr uint8_t kOutputRumble = 0x10;
constexpr uint8_t kOutputUsbCommand = 0x80;
constexpr uint8_t kUsbStatus = 0x01;
constexpr uint8_t kUsbHandshake = 0x02;
constexpr uint8_t kUsbBaudrate = 0x03;
constexpr uint8_t kUsbNoTimeout = 0x04;
constexpr uint8_t kUsbEnableTimeout = 0x05;
constexpr uint8_t kSubRequestDeviceInfo = 0x02;
constexpr uint8_t kSubSetInputMode = 0x03;
constexpr uint8_t kSubSpiRead = 0x10;
constexpr uint8_t kSubSetPlayerLeds = 0x30;
constexpr uint8_t kSubGetPlayerLeds = 0x31;
constexpr uint8_t kSubSetHomeLight = 0x38;
constexpr uint8_t kSubEnableImu = 0x40;
constexpr uint8_t kSubSetImuSensitivity = 0x41;
constexpr uint8_t kSubEnableVibration = 0x48;
constexpr uint8_t kSubGetRegulatedVoltage = 0x50;

constexpr std::array<uint8_t, 203> kSwitchProReportDescriptor = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01,
    0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0A, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x0A, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0B, 0x29, 0x0E, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0B, 0x01,
    0x00, 0x01, 0x00, 0xA1, 0x00, 0x0B, 0x30, 0x00,
    0x01, 0x00, 0x0B, 0x31, 0x00, 0x01, 0x00, 0x0B,
    0x32, 0x00, 0x01, 0x00, 0x0B, 0x35, 0x00, 0x01,
    0x00, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xC0, 0x0B,
    0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19,
    0x0F, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95,
    0x34, 0x81, 0x03, 0x06, 0x00, 0xFF, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03,
    0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F,
    0x81, 0x03, 0x85, 0x01, 0x09, 0x03, 0x75, 0x08,
    0x95, 0x3F, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04,
    0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x80,
    0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3F,
    0x91, 0x83, 0xC0,
};

std::string bounded_string(const char* text, size_t size,
                           const char* fallback) {
  const size_t length = strnlen(text, size);
  return length ? std::string(text, length) : fallback;
}

bool button_pressed(const InputState& state, uint8_t index) {
  return index < kMaxButtons && (state.buttons & (uint64_t{1} << index));
}

uint16_t scaled_u16(uint32_t value, uint32_t minimum, uint32_t maximum) {
  if (maximum <= minimum || value <= minimum) return 0;
  if (value >= maximum) return 0xFFFF;
  return static_cast<uint16_t>(((value - minimum) * 0xFFFFu) /
                               (maximum - minimum));
}

uint16_t decode_low_rumble_amplitude(uint32_t value) {
  if (value >= 0x8000u)
    return scaled_u16(value, 0x803F, 0x8072);
  return scaled_u16(value, 0x0040, 0x007F);
}

uint8_t switch_battery_connection(uint8_t battery_percent) {
  if (battery_percent == 0) return 0x91;
  if (battery_percent >= 80) return 0x81;
  if (battery_percent >= 50) return 0x61;
  if (battery_percent >= 20) return 0x41;
  return 0x21;
}

void apply_hat(uint8_t hat, uint8_t& dpad_byte) {
  if (hat > 7) return;
  const bool up = hat == 0 || hat == 1 || hat == 7;
  const bool right = hat == 1 || hat == 2 || hat == 3;
  const bool down = hat == 3 || hat == 4 || hat == 5;
  const bool left = hat == 5 || hat == 6 || hat == 7;
  if (down) dpad_byte |= 0x01;
  if (up) dpad_byte |= 0x02;
  if (right) dpad_byte |= 0x04;
  if (left) dpad_byte |= 0x08;
}

void pack_stick(uint8_t* out, int16_t x, int16_t y) {
  const auto scale = [](int16_t value) {
    const int32_t scaled = 2048 + (static_cast<int32_t>(value) * 1536 / 32768);
    return static_cast<uint16_t>(std::clamp(scaled, 0, 4095));
  };
  const uint16_t ux = scale(x);
  const uint16_t uy = scale(y);
  out[0] = static_cast<uint8_t>(ux & 0xFF);
  out[1] = static_cast<uint8_t>((ux >> 8) | ((uy & 0x0F) << 4));
  out[2] = static_cast<uint8_t>(uy >> 4);
}

void append_i16(uint8_t* out, int16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8);
}

OutputState decode_rumble_data(std::span<const uint8_t> rumble) {
  OutputState output{};
  if (rumble.size() < 8) return output;
  for (size_t motor = 0; motor < 2; ++motor) {
    const auto sample = rumble.subspan(motor * 4, 4);
    const uint32_t high_amplitude = sample[1] & 0xFEu;
    const uint32_t low_amplitude =
        ((sample[2] & 0x80u) << 8) | sample[3];
    output.high_frequency =
        std::max(output.high_frequency,
                 scaled_u16(high_amplitude, 0x00, 0xC8));
    output.low_frequency =
        std::max(output.low_frequency,
                 decode_low_rumble_amplitude(low_amplitude));
  }
  if (output.low_frequency || output.high_frequency)
    output.duration_ms = 100;
  return output;
}

void encode_calibration_pair(uint8_t* out, uint16_t first,
                             uint16_t second) {
  out[0] = static_cast<uint8_t>(first);
  out[1] = static_cast<uint8_t>(((first >> 8) & 0x0F) |
                                ((second & 0x0F) << 4));
  out[2] = static_cast<uint8_t>(second >> 4);
}

std::array<uint8_t, 18> make_stick_factory_calibration() {
  constexpr uint16_t kCenter = 0x800;
  constexpr uint16_t kRange = 0x600;
  std::array<uint8_t, 18> data{};
  encode_calibration_pair(&data[0], kRange, kRange);
  encode_calibration_pair(&data[3], kCenter, kCenter);
  encode_calibration_pair(&data[6], kRange, kRange);
  encode_calibration_pair(&data[9], kCenter, kCenter);
  encode_calibration_pair(&data[12], kRange, kRange);
  encode_calibration_pair(&data[15], kRange, kRange);
  return data;
}

std::array<uint8_t, 24> make_imu_factory_calibration() {
  return {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x3B, 0x34, 0x3B, 0x34, 0x3B, 0x34,
  };
}

std::array<uint8_t, 6> make_stable_mac(const std::string& serial) {
  uint32_t hash = 2166136261u;
  for (const unsigned char value : serial) {
    hash ^= value;
    hash *= 16777619u;
  }
  return {0x98, 0xB6, 0xE9, static_cast<uint8_t>(hash >> 16),
          static_cast<uint8_t>(hash >> 8), static_cast<uint8_t>(hash)};
}

int16_t clamp_i16(long value) {
  return static_cast<int16_t>(std::clamp(value, -32768l, 32767l));
}

class SwitchProProfile final : public HidProfile {
 public:
  explicit SwitchProProfile(const DeviceDescription& description) {
    has_hat_ = description.hat_count > 0;
    properties_.vendor_id = 0x057E;
    properties_.product_id = 0x2009;
    properties_.version_number = 0x0210;
    properties_.primary_usage_page = 0x01;
    properties_.primary_usage = 0x04;
    properties_.vendor_id_source = 1;
    properties_.product = "Pro Controller";
    properties_.manufacturer = "Nintendo Co., Ltd.";
    properties_.serial = bounded_string(description.serial,
                                        sizeof(description.serial),
                                        "vhid-switch-pro-1");
    mac_ = make_stable_mac(properties_.serial);
    properties_.transport = "USB";
    properties_.report_descriptor.assign(kSwitchProReportDescriptor.begin(),
                                         kSwitchProReportDescriptor.end());
    std::fill(std::begin(last_state_.hats), std::end(last_state_.hats),
              uint8_t{8});
    last_subcommand_reply_ = encode_subcommand_reply(0, 0x80);
    timer_ = 0;
  }

  const HidDeviceProperties& properties() const override {
    return properties_;
  }

  std::vector<uint8_t> encode(const InputState& state) const override {
    last_state_ = state;
    std::vector<uint8_t> report(64);
    report[0] = kInputFull;
    report[1] = timer_++;
    report[2] = switch_battery_connection(state.battery_percent);

    if (button_pressed(state, 2)) report[3] |= 0x01;   // Y
    if (button_pressed(state, 3)) report[3] |= 0x02;   // X
    if (button_pressed(state, 0)) report[3] |= 0x04;   // B
    if (button_pressed(state, 1)) report[3] |= 0x08;   // A
    if (button_pressed(state, 5)) report[3] |= 0x40;   // R
    if (button_pressed(state, 7)) report[3] |= 0x80;   // ZR

    if (button_pressed(state, 8)) report[4] |= 0x01;   // Minus
    if (button_pressed(state, 9)) report[4] |= 0x02;   // Plus
    if (button_pressed(state, 11)) report[4] |= 0x04;  // Right stick
    if (button_pressed(state, 10)) report[4] |= 0x08;  // Left stick
    if (button_pressed(state, 16)) report[4] |= 0x10;  // Home
    if (button_pressed(state, 17)) report[4] |= 0x20;  // Capture

    if (button_pressed(state, 13)) report[5] |= 0x01;  // Down
    if (button_pressed(state, 12)) report[5] |= 0x02;  // Up
    if (button_pressed(state, 15)) report[5] |= 0x04;  // Right
    if (button_pressed(state, 14)) report[5] |= 0x08;  // Left
    if (button_pressed(state, 4)) report[5] |= 0x40;   // L
    if (button_pressed(state, 6)) report[5] |= 0x80;   // ZL
    if (has_hat_ && (report[5] & 0x0F) == 0) {
      apply_hat(state.hats[0], report[5]);
    }

    pack_stick(&report[6], state.axes[0], state.axes[1]);
    pack_stick(&report[9], state.axes[2], state.axes[3]);
    report[12] = 0x09;
    encode_motion(state, report);
    return report;
  }

  bool decode_output(std::span<const uint8_t> report,
                     OutputState& output) const override {
    const bool includes_report_id =
        !report.empty() && (report[0] == 0x01 || report[0] == 0x10);
    const size_t rumble_offset = includes_report_id ? 2 : 1;
    if (report.size() < rumble_offset + 8) return false;
    output = decode_rumble_data(report.subspan(rumble_offset, 8));
    return true;
  }

  bool handle_host_report(HidReportType type, uint8_t report_id,
                          std::span<const uint8_t> report,
                          std::vector<uint8_t>& response) override {
    if (type != HidReportType::output) return false;
    const auto packet = report_with_id(report_id, report);
    if (packet.empty()) return false;
    const uint8_t command = packet[0];
    const std::span<const uint8_t> payload(packet.data() + 1,
                                           packet.size() - 1);
    switch (command) {
      case kOutputUsbCommand:
        return handle_usb_command(payload, response);
      case kOutputRumbleSubcommand:
        return handle_subcommand(payload, response);
      case kOutputRumble:
        return true;
      default:
        return false;
    }
  }

  bool get_host_report(HidReportType type, uint8_t report_id,
                       std::span<uint8_t> report,
                       size_t& report_size) override {
    if (type == HidReportType::input && report_id == kInputFull) {
      return copy_get_report_payload(encode(last_state_), report,
                                     report_size);
    }
    if (type == HidReportType::input && report_id == kInputSubcommandReply) {
      return copy_get_report_payload(last_subcommand_reply_, report,
                                     report_size);
    }
    if (type == HidReportType::feature && report_id == 0x02) {
      return copy_get_report_payload(last_subcommand_reply_, report,
                                     report_size);
    }
    return false;
  }

 private:
  static std::vector<uint8_t> report_with_id(
      uint8_t report_id, std::span<const uint8_t> report) {
    if (!report.empty() && report[0] == report_id)
      return std::vector<uint8_t>(report.begin(), report.end());
    std::vector<uint8_t> packet;
    packet.reserve(report.size() + (report_id ? 1 : 0));
    if (report_id) packet.push_back(report_id);
    packet.insert(packet.end(), report.begin(), report.end());
    return packet;
  }

  static bool copy_get_report_payload(const std::vector<uint8_t>& source,
                                      std::span<uint8_t> report,
                                      size_t& report_size) {
    if (source.empty()) return false;
    const size_t payload_size = source.size() - 1;
    if (report.size() < payload_size) return false;
    std::copy(source.begin() + 1, source.end(), report.begin());
    report_size = payload_size;
    return true;
  }

  static void encode_motion(const InputState& state,
                            std::vector<uint8_t>& report) {
    const bool has_motion =
        state.acceleration[0] != 0.0f || state.acceleration[1] != 0.0f ||
        state.acceleration[2] != 0.0f || state.angular_velocity[0] != 0.0f ||
        state.angular_velocity[1] != 0.0f || state.angular_velocity[2] != 0.0f;
    const float accel_z =
        has_motion ? state.acceleration[2] : 9.80665f;
    const int16_t accel[] = {
        clamp_i16(std::lround(state.acceleration[0] / 9.80665f * 256.0f)),
        clamp_i16(std::lround(state.acceleration[1] / 9.80665f * 256.0f)),
        clamp_i16(std::lround(accel_z / 9.80665f * 256.0f)),
    };
    constexpr float kRadiansToDegrees = 57.2957795131f;
    const int16_t gyro[] = {
        clamp_i16(std::lround(state.angular_velocity[0] *
                              kRadiansToDegrees * 16.0f)),
        clamp_i16(std::lround(state.angular_velocity[1] *
                              kRadiansToDegrees * 16.0f)),
        clamp_i16(std::lround(state.angular_velocity[2] *
                              kRadiansToDegrees * 16.0f)),
    };
    for (size_t sample = 0; sample < 3; ++sample) {
      const size_t base = 13 + sample * 12;
      append_i16(&report[base + 0], accel[0]);
      append_i16(&report[base + 2], accel[1]);
      append_i16(&report[base + 4], accel[2]);
      append_i16(&report[base + 6], gyro[0]);
      append_i16(&report[base + 8], gyro[1]);
      append_i16(&report[base + 10], gyro[2]);
    }
  }

  bool handle_usb_command(std::span<const uint8_t> payload,
                          std::vector<uint8_t>& response) {
    if (payload.empty()) return true;
    switch (payload[0]) {
      case kUsbStatus:
        response.assign(64, 0);
        response[0] = kInputUsbResponse;
        response[1] = kUsbStatus;
        response[3] = 0x03;
        for (size_t i = 0; i < mac_.size(); ++i)
          response[4 + i] = mac_[mac_.size() - 1 - i];
        return true;
      case kUsbHandshake:
      case kUsbBaudrate:
        response.assign(64, 0);
        response[0] = kInputUsbResponse;
        response[1] = payload[0];
        return true;
      case kUsbNoTimeout:
      case kUsbEnableTimeout:
        return true;
      default:
        return true;
    }
  }

  bool handle_subcommand(std::span<const uint8_t> payload,
                         std::vector<uint8_t>& response) {
    if (payload.size() < 10) return true;
    const uint8_t subcommand = payload[9];
    const std::span<const uint8_t> arguments(payload.data() + 10,
                                             payload.size() - 10);
    response = encode_subcommand_reply(subcommand, 0x80);
    switch (subcommand) {
      case kSubRequestDeviceInfo:
        response[13] = 0x82;
        response[15] = 0x03;
        response[16] = 0x48;
        response[17] = 0x03;
        response[18] = 0x02;
        std::copy(mac_.begin(), mac_.end(), response.begin() + 19);
        response[25] = 0x01;
        response[26] = 0x01;
        break;
      case kSubSetInputMode:
        if (!arguments.empty()) input_mode_ = arguments[0];
        break;
      case kSubSpiRead:
        response[13] = 0x90;
        append_spi_read_response(arguments, response);
        break;
      case kSubSetPlayerLeds:
        if (!arguments.empty()) player_leds_ = arguments[0];
        break;
      case kSubGetPlayerLeds:
        response[13] = 0xB0;
        response[15] = player_leds_;
        break;
      case kSubSetHomeLight:
      case kSubSetImuSensitivity:
        break;
      case kSubEnableImu:
        imu_enabled_ = !arguments.empty() && arguments[0] != 0;
        break;
      case kSubEnableVibration:
        vibration_enabled_ = !arguments.empty() && arguments[0] != 0;
        break;
      case kSubGetRegulatedVoltage:
        response[13] = 0xD0;
        response[15] = 0x28;
        response[16] = 0x05;
        break;
      default:
        break;
    }
    last_subcommand_reply_ = response;
    return true;
  }

  std::vector<uint8_t> encode_subcommand_reply(uint8_t subcommand,
                                               uint8_t ack) const {
    auto report = encode(last_state_);
    report[0] = kInputSubcommandReply;
    report[13] = ack;
    report[14] = subcommand;
    std::fill(report.begin() + 15, report.end(), uint8_t{0});
    return report;
  }

  void append_spi_read_response(std::span<const uint8_t> arguments,
                                std::vector<uint8_t>& response) const {
    if (arguments.size() < 5) return;
    const uint32_t address =
        static_cast<uint32_t>(arguments[0]) |
        (static_cast<uint32_t>(arguments[1]) << 8) |
        (static_cast<uint32_t>(arguments[2]) << 16) |
        (static_cast<uint32_t>(arguments[3]) << 24);
    const size_t length = std::min<size_t>(arguments[4], 29);
    response[15] = static_cast<uint8_t>(address);
    response[16] = static_cast<uint8_t>(address >> 8);
    response[17] = static_cast<uint8_t>(address >> 16);
    response[18] = static_cast<uint8_t>(address >> 24);
    response[19] = static_cast<uint8_t>(length);
    std::fill(response.begin() + 20, response.begin() + 20 + length,
              uint8_t{0xFF});
    read_spi(address,
             std::span<uint8_t>(response.data() + 20, length));
  }

  void read_spi(uint32_t address, std::span<uint8_t> out) const {
    const auto sticks = make_stick_factory_calibration();
    const auto imu = make_imu_factory_calibration();
    overlay_spi(address, out, 0x603D, sticks);
    overlay_spi(address, out, 0x6020, imu);
  }

  static void overlay_spi(uint32_t request_address, std::span<uint8_t> out,
                          uint32_t region_address,
                          std::span<const uint8_t> region) {
    if (out.empty() || region.empty()) return;
    const uint64_t request_begin = request_address;
    const uint64_t request_end = request_begin + out.size();
    const uint64_t region_begin = region_address;
    const uint64_t region_end = region_begin + region.size();
    const uint64_t copy_begin = std::max(request_begin, region_begin);
    const uint64_t copy_end = std::min(request_end, region_end);
    if (copy_begin >= copy_end) return;
    std::copy(region.begin() + (copy_begin - region_begin),
              region.begin() + (copy_end - region_begin),
              out.begin() + (copy_begin - request_begin));
  }

  HidDeviceProperties properties_{};
  std::array<uint8_t, 6> mac_{};
  mutable InputState last_state_{};
  mutable std::vector<uint8_t> last_subcommand_reply_{};
  bool has_hat_ = false;
  uint8_t input_mode_ = kInputFull;
  uint8_t player_leds_ = 0;
  bool imu_enabled_ = false;
  bool vibration_enabled_ = false;
  mutable uint8_t timer_ = 0;
};

}  // namespace

std::unique_ptr<HidProfile> make_switch_pro_profile(
    const DeviceDescription& description) {
  return std::make_unique<SwitchProProfile>(description);
}

}  // namespace vhid
