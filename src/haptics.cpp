#include "vhid/haptics.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vhid {
namespace {

constexpr uint16_t kDefaultSwitchLowFrequencyHz = 160;
constexpr uint16_t kDefaultSwitchHighFrequencyHz = 320;
constexpr uint16_t kDefaultStrongFrequencyHz = 141;
constexpr uint16_t kDefaultWeakFrequencyHz = 182;
constexpr uint16_t kSwitch2DefaultHighFrequency = 0x0187;
constexpr uint16_t kSwitch2DefaultLowFrequency = 0x0112;
constexpr uint16_t kSwitch2MaxFrequency = 0x03ff;
constexpr uint16_t kSwitch2RumbleMax = 29000;

struct SwitchAmplitudeEntry {
  uint8_t high;
  uint16_t low;
  uint16_t amplitude;
};

constexpr SwitchAmplitudeEntry kSwitchAmplitudes[] = {
    {0x00, 0x0040, 0},    {0x02, 0x8040, 10},
    {0x04, 0x0041, 12},   {0x06, 0x8041, 14},
    {0x08, 0x0042, 17},   {0x0A, 0x8042, 20},
    {0x0C, 0x0043, 24},   {0x0E, 0x8043, 28},
    {0x10, 0x0044, 33},   {0x12, 0x8044, 40},
    {0x14, 0x0045, 47},   {0x16, 0x8045, 56},
    {0x18, 0x0046, 67},   {0x1A, 0x8046, 80},
    {0x1C, 0x0047, 95},   {0x1E, 0x8047, 112},
    {0x20, 0x0048, 117},  {0x22, 0x8048, 123},
    {0x24, 0x0049, 128},  {0x26, 0x8049, 134},
    {0x28, 0x004A, 140},  {0x2A, 0x804A, 146},
    {0x2C, 0x004B, 152},  {0x2E, 0x804B, 159},
    {0x30, 0x004C, 166},  {0x32, 0x804C, 173},
    {0x34, 0x004D, 181},  {0x36, 0x804D, 189},
    {0x38, 0x004E, 198},  {0x3A, 0x804E, 206},
    {0x3C, 0x004F, 215},  {0x3E, 0x804F, 225},
    {0x40, 0x0050, 230},  {0x42, 0x8050, 235},
    {0x44, 0x0051, 240},  {0x46, 0x8051, 245},
    {0x48, 0x0052, 251},  {0x4A, 0x8052, 256},
    {0x4C, 0x0053, 262},  {0x4E, 0x8053, 268},
    {0x50, 0x0054, 273},  {0x52, 0x8054, 279},
    {0x54, 0x0055, 286},  {0x56, 0x8055, 292},
    {0x58, 0x0056, 298},  {0x5A, 0x8056, 305},
    {0x5C, 0x0057, 311},  {0x5E, 0x8057, 318},
    {0x60, 0x0058, 325},  {0x62, 0x8058, 332},
    {0x64, 0x0059, 340},  {0x66, 0x8059, 347},
    {0x68, 0x005A, 355},  {0x6A, 0x805A, 362},
    {0x6C, 0x005B, 370},  {0x6E, 0x805B, 378},
    {0x70, 0x005C, 387},  {0x72, 0x805C, 395},
    {0x74, 0x005D, 404},  {0x76, 0x805D, 413},
    {0x78, 0x005E, 422},  {0x7A, 0x805E, 431},
    {0x7C, 0x005F, 440},  {0x7E, 0x805F, 450},
    {0x80, 0x0060, 460},  {0x82, 0x8060, 470},
    {0x84, 0x0061, 480},  {0x86, 0x8061, 491},
    {0x88, 0x0062, 501},  {0x8A, 0x8062, 512},
    {0x8C, 0x0063, 524},  {0x8E, 0x8063, 535},
    {0x90, 0x0064, 547},  {0x92, 0x8064, 559},
    {0x94, 0x0065, 571},  {0x96, 0x8065, 584},
    {0x98, 0x0066, 596},  {0x9A, 0x8066, 609},
    {0x9C, 0x0067, 623},  {0x9E, 0x8067, 636},
    {0xA0, 0x0068, 650},  {0xA2, 0x8068, 665},
    {0xA4, 0x0069, 679},  {0xA6, 0x8069, 694},
    {0xA8, 0x006A, 709},  {0xAA, 0x806A, 725},
    {0xAC, 0x006B, 741},  {0xAE, 0x806B, 757},
    {0xB0, 0x006C, 773},  {0xB2, 0x806C, 790},
    {0xB4, 0x006D, 808},  {0xB6, 0x806D, 825},
    {0xB8, 0x006E, 843},  {0xBA, 0x806E, 862},
    {0xBC, 0x006F, 881},  {0xBE, 0x806F, 900},
    {0xC0, 0x0070, 920},  {0xC2, 0x8070, 940},
    {0xC4, 0x0071, 960},  {0xC6, 0x8071, 981},
    {0xC8, 0x0072, 1000},
};

uint16_t scale_amplitude_to_u16(uint16_t amplitude) {
  return static_cast<uint16_t>(
      (static_cast<uint32_t>(amplitude) * UINT16_MAX) / 1000u);
}

uint16_t scale_amplitude_from_u16(uint16_t amplitude) {
  return static_cast<uint16_t>(
      (static_cast<uint32_t>(amplitude) * 1000u + UINT16_MAX / 2) /
      UINT16_MAX);
}

const SwitchAmplitudeEntry& nearest_amplitude(uint16_t amplitude) {
  const uint16_t target = scale_amplitude_from_u16(amplitude);
  const SwitchAmplitudeEntry* best = &kSwitchAmplitudes[0];
  for (const auto& entry : kSwitchAmplitudes) {
    if (std::abs(static_cast<int>(entry.amplitude) -
                 static_cast<int>(target)) <
        std::abs(static_cast<int>(best->amplitude) -
                 static_cast<int>(target))) {
      best = &entry;
    }
  }
  return *best;
}

uint16_t decode_high_amplitude(uint8_t encoded) {
  for (const auto& entry : kSwitchAmplitudes) {
    if (entry.high == encoded) return scale_amplitude_to_u16(entry.amplitude);
  }
  const SwitchAmplitudeEntry* best = &kSwitchAmplitudes[0];
  for (const auto& entry : kSwitchAmplitudes) {
    if (std::abs(static_cast<int>(entry.high) -
                 static_cast<int>(encoded)) <
        std::abs(static_cast<int>(best->high) -
                 static_cast<int>(encoded))) {
      best = &entry;
    }
  }
  return scale_amplitude_to_u16(best->amplitude);
}

uint16_t low_amplitude_index(uint16_t encoded) {
  const uint16_t base = encoded & 0x00FFu;
  if (base < 0x40u) return 0;
  return static_cast<uint16_t>(((base - 0x40u) * 2u) +
                               ((encoded & 0x8000u) ? 1u : 0u));
}

uint16_t decode_low_amplitude(uint16_t encoded) {
  for (const auto& entry : kSwitchAmplitudes) {
    if (entry.low == encoded) return scale_amplitude_to_u16(entry.amplitude);
  }
  const uint16_t index = low_amplitude_index(encoded);
  const SwitchAmplitudeEntry* best = &kSwitchAmplitudes[0];
  for (const auto& entry : kSwitchAmplitudes) {
    if (std::abs(static_cast<int>(low_amplitude_index(entry.low)) -
                 static_cast<int>(index)) <
        std::abs(static_cast<int>(low_amplitude_index(best->low)) -
                 static_cast<int>(index))) {
      best = &entry;
    }
  }
  return scale_amplitude_to_u16(best->amplitude);
}

uint16_t scale_switch2_amplitude(uint16_t amplitude) {
  return static_cast<uint16_t>(
      (static_cast<uint32_t>(amplitude) * kSwitch2RumbleMax) / UINT16_MAX);
}

uint16_t unscale_switch2_amplitude(uint16_t amplitude) {
  return static_cast<uint16_t>(
      std::min<uint32_t>(
          (static_cast<uint32_t>(amplitude) * UINT16_MAX) / kSwitch2RumbleMax,
          UINT16_MAX));
}

uint16_t scale_switch2_frequency(uint16_t frequency_hz, bool high_band) {
  if (!frequency_hz) {
    return high_band ? kSwitch2DefaultHighFrequency
                     : kSwitch2DefaultLowFrequency;
  }
  const double reference_hz = high_band ? kDefaultSwitchHighFrequencyHz
                                        : kDefaultSwitchLowFrequencyHz;
  const double reference_code = high_band ? kSwitch2DefaultHighFrequency
                                          : kSwitch2DefaultLowFrequency;
  const long code = std::lround(reference_code *
                                static_cast<double>(frequency_hz) /
                                reference_hz);
  return static_cast<uint16_t>(
      std::clamp<long>(code, 1, kSwitch2MaxFrequency));
}

uint16_t unscale_switch2_frequency(uint16_t code, bool high_band) {
  if (!code) return 0;
  const double reference_hz = high_band ? kDefaultSwitchHighFrequencyHz
                                        : kDefaultSwitchLowFrequencyHz;
  const double reference_code = high_band ? kSwitch2DefaultHighFrequency
                                          : kSwitch2DefaultLowFrequency;
  const long frequency =
      std::lround(static_cast<double>(code) * reference_hz / reference_code);
  return static_cast<uint16_t>(
      std::clamp<long>(frequency, 1, UINT16_MAX));
}

uint8_t encoded_frequency(uint16_t frequency_hz) {
  const uint16_t clamped = std::clamp<uint16_t>(frequency_hz, 41, 1253);
  const long encoded =
      std::lround(std::log2(static_cast<double>(clamped) / 10.0) * 32.0);
  return static_cast<uint8_t>(std::clamp<long>(encoded, 0x40, 0xC0));
}

uint16_t frequency_from_encoded(uint8_t encoded) {
  const double frequency = 10.0 * std::pow(2.0, encoded / 32.0);
  return static_cast<uint16_t>(std::clamp<long>(
      std::lround(frequency), 0, UINT16_MAX));
}

uint16_t high_frequency_code(uint16_t frequency_hz) {
  const uint8_t encoded =
      std::max<uint8_t>(encoded_frequency(frequency_hz), 0x60);
  return static_cast<uint16_t>((encoded - 0x60) * 4u);
}

uint8_t low_frequency_code(uint16_t frequency_hz) {
  const uint8_t encoded =
      std::min<uint8_t>(encoded_frequency(frequency_hz), 0xBF);
  return static_cast<uint8_t>(encoded - 0x40);
}

uint16_t high_frequency_from_code(uint16_t code) {
  return frequency_from_encoded(
      static_cast<uint8_t>(std::clamp<uint16_t>(code / 4 + 0x60, 0x60, 0xC0)));
}

uint16_t low_frequency_from_code(uint8_t code) {
  return frequency_from_encoded(
      static_cast<uint8_t>(std::clamp<uint16_t>(code + 0x40, 0x40, 0xBF)));
}

HapticMotorState fallback_motor(const OutputState& output, size_t index) {
  HapticMotorState motor{};
  const uint16_t amplitude =
      index == 0 ? output.low_frequency : output.high_frequency;
  const uint16_t frequency =
      index == 0 ? kDefaultStrongFrequencyHz : kDefaultWeakFrequencyHz;
  motor.low.frequency_hz = frequency;
  motor.high.frequency_hz = frequency;
  motor.low.amplitude = amplitude;
  motor.high.amplitude = amplitude;
  return motor;
}

HapticMotorState motor_or_fallback(const OutputState& output, size_t index) {
  if (output_has_motor(output, index)) return output.motors[index];
  return fallback_motor(output, index);
}

HapticMotorState aggregate_motor_for_switch2(const OutputState& output) {
  HapticMotorState motor{};
  motor.low.frequency_hz = kDefaultSwitchLowFrequencyHz;
  motor.low.amplitude = output.low_frequency;
  motor.high.frequency_hz = kDefaultSwitchHighFrequencyHz;
  motor.high.amplitude = output.high_frequency;
  return motor;
}

void refresh_aggregate_fields(OutputState& output) {
  uint16_t low = 0;
  uint16_t high = 0;
  for (size_t i = 0; i < 2; ++i) {
    if (!output_has_motor(output, i)) continue;
    low = std::max(low, output.motors[i].low.amplitude);
    high = std::max(high, output.motors[i].high.amplitude);
  }
  output.low_frequency = low;
  output.high_frequency = high;
  if ((low || high) && !output.duration_ms) output.duration_ms = 100;
}

void decode_switch1_hd_motor(std::span<const uint8_t> data,
                             HapticMotorState& motor) {
  if (data.size() < 4) return;
  const uint16_t high_frequency =
      static_cast<uint16_t>(data[0] | ((data[1] & 0x01u) << 8));
  const uint8_t high_amplitude =
      static_cast<uint8_t>(data[1] & 0xFEu);
  const uint8_t low_frequency = static_cast<uint8_t>(data[2] & 0x7Fu);
  const uint16_t low_amplitude =
      static_cast<uint16_t>(((data[2] & 0x80u) << 8) | data[3]);

  motor.high.encoded_frequency = high_frequency;
  motor.high.frequency_hz = high_frequency_from_code(high_frequency);
  motor.high.encoded_amplitude = high_amplitude;
  motor.high.amplitude = decode_high_amplitude(high_amplitude);

  motor.low.encoded_frequency = low_frequency;
  motor.low.frequency_hz = low_frequency_from_code(low_frequency);
  motor.low.encoded_amplitude = low_amplitude;
  motor.low.amplitude = decode_low_amplitude(low_amplitude);
  std::copy(data.begin(), data.begin() + 4, motor.switch1_hd);
}

void encode_switch1_hd_motor(const HapticMotorState& motor, uint8_t* out) {
  const uint16_t high_freq =
      high_frequency_code(motor.high.frequency_hz
                              ? motor.high.frequency_hz
                              : kDefaultSwitchHighFrequencyHz);
  const uint8_t low_freq =
      low_frequency_code(motor.low.frequency_hz
                             ? motor.low.frequency_hz
                             : kDefaultSwitchLowFrequencyHz);
  const auto& high_amp = nearest_amplitude(motor.high.amplitude);
  const auto& low_amp = nearest_amplitude(motor.low.amplitude);
  out[0] = static_cast<uint8_t>(high_freq & 0xFF);
  out[1] = static_cast<uint8_t>(high_amp.high + ((high_freq >> 8) & 0xFF));
  out[2] = static_cast<uint8_t>(low_freq + ((low_amp.low >> 8) & 0xFF));
  out[3] = static_cast<uint8_t>(low_amp.low & 0xFF);
}

void encode_switch2_usb_motor(const HapticMotorState& motor, uint8_t* out) {
  const uint16_t high_freq =
      scale_switch2_frequency(motor.high.frequency_hz, true);
  const uint16_t low_freq =
      scale_switch2_frequency(motor.low.frequency_hz, false);
  const uint16_t high_amp = scale_switch2_amplitude(motor.high.amplitude);
  const uint16_t low_amp = scale_switch2_amplitude(motor.low.amplitude);
  out[0] = static_cast<uint8_t>(high_freq);
  out[1] = static_cast<uint8_t>(((high_amp >> 4) & 0xFC) |
                                ((high_freq >> 8) & 0x03));
  out[2] = static_cast<uint8_t>((high_amp >> 12) | (low_freq << 4));
  out[3] = static_cast<uint8_t>((low_amp & 0xC0) |
                                ((low_freq >> 4) & 0x3F));
  out[4] = static_cast<uint8_t>(low_amp >> 8);
}

HapticMotorState merge_motors_for_switch2(const OutputState& output) {
  HapticMotorState merged{};
  const HapticMotorState first = motor_or_fallback(output, 0);
  const HapticMotorState second = motor_or_fallback(output, 1);
  merged.low = first.low.amplitude >= second.low.amplitude ? first.low
                                                           : second.low;
  merged.high = first.high.amplitude >= second.high.amplitude ? first.high
                                                              : second.high;
  if (!merged.low.frequency_hz && !merged.low.encoded_frequency) {
    merged.low.frequency_hz = kDefaultSwitchLowFrequencyHz;
  }
  if (!merged.high.frequency_hz && !merged.high.encoded_frequency) {
    merged.high.frequency_hz = kDefaultSwitchHighFrequencyHz;
  }
  return merged;
}

HapticMotorState motor_for_switch2_usb(const OutputState& output,
                                       size_t index) {
  if (output_has_motor(output, index)) return output.motors[index];
  if (output_has_haptics(output)) return {};
  return aggregate_motor_for_switch2(output);
}

}  // namespace

bool output_has_haptics(const OutputState& output) {
  return output.haptic_flags &
         (kOutputHapticMotorLeft | kOutputHapticMotorRight);
}

bool output_has_motor(const OutputState& output, size_t index) {
  if (index >= 2) return false;
  const uint8_t flag =
      index == 0 ? kOutputHapticMotorLeft : kOutputHapticMotorRight;
  return output.haptic_flags & flag;
}

void set_haptic_motor(OutputState& output, size_t index,
                      const HapticMotorState& motor) {
  if (index >= 2) return;
  output.motors[index] = motor;
  output.haptic_flags |=
      index == 0 ? kOutputHapticMotorLeft : kOutputHapticMotorRight;
  output.motor_count = std::max<uint8_t>(output.motor_count,
                                         static_cast<uint8_t>(index + 1));
  refresh_aggregate_fields(output);
}

void decode_switch1_hd_rumble(std::span<const uint8_t> rumble,
                              OutputState& output) {
  output = {};
  if (rumble.size() < 8) return;
  for (size_t motor_index = 0; motor_index < 2; ++motor_index) {
    HapticMotorState motor{};
    decode_switch1_hd_motor(rumble.subspan(motor_index * 4, 4), motor);
    set_haptic_motor(output, motor_index, motor);
  }
  output.haptic_flags |= kOutputHapticSwitch1HdPacket;
  if (output.low_frequency || output.high_frequency) output.duration_ms = 100;
}

void encode_switch1_hd_rumble(const OutputState& output, uint8_t* out) {
  for (size_t motor_index = 0; motor_index < 2; ++motor_index) {
    uint8_t* target = out + motor_index * 4;
    if ((output.haptic_flags & kOutputHapticSwitch1HdPacket) &&
        output_has_motor(output, motor_index)) {
      std::memcpy(target, output.motors[motor_index].switch1_hd, 4);
      continue;
    }
    const HapticMotorState motor = motor_or_fallback(output, motor_index);
    encode_switch1_hd_motor(motor, target);
  }
}

void encode_switch2_usb_rumble(const OutputState& output, uint8_t* out) {
  const HapticMotorState motor = output_has_haptics(output)
                                     ? merge_motors_for_switch2(output)
                                     : aggregate_motor_for_switch2(output);
  encode_switch2_usb_motor(motor, out);
}

void encode_switch2_usb_motor_rumble(const OutputState& output,
                                     size_t motor_index, uint8_t* out) {
  const HapticMotorState motor = motor_for_switch2_usb(output, motor_index);
  encode_switch2_usb_motor(motor, out);
}

void decode_switch2_usb_motor_rumble(std::span<const uint8_t> data,
                                     HapticMotorState& motor) {
  motor = {};
  if (data.size() < 5) return;
  const uint16_t high_freq =
      static_cast<uint16_t>(data[0] | ((data[1] & 0x03u) << 8));
  const uint16_t high_amp =
      static_cast<uint16_t>(((data[1] & 0xFCu) << 4) |
                            ((data[2] & 0x0Fu) << 12));
  const uint16_t low_freq =
      static_cast<uint16_t>((data[2] >> 4) | ((data[3] & 0x3Fu) << 4));
  const uint16_t low_amp =
      static_cast<uint16_t>((data[4] << 8) | (data[3] & 0xC0u));
  motor.high.encoded_frequency = high_freq;
  motor.high.frequency_hz = unscale_switch2_frequency(high_freq, true);
  motor.high.encoded_amplitude = high_amp;
  motor.high.amplitude = unscale_switch2_amplitude(high_amp);
  motor.low.encoded_frequency = low_freq;
  motor.low.frequency_hz = unscale_switch2_frequency(low_freq, false);
  motor.low.encoded_amplitude = low_amp;
  motor.low.amplitude = unscale_switch2_amplitude(low_amp);
}

void decode_switch2_usb_rumble(std::span<const uint8_t> data,
                               OutputState& output) {
  output = {};
  HapticMotorState motor{};
  decode_switch2_usb_motor_rumble(data, motor);
  set_haptic_motor(output, 0, motor);
  set_haptic_motor(output, 1, motor);
  if (output.low_frequency || output.high_frequency) output.duration_ms = 100;
}

}  // namespace vhid
