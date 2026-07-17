#ifndef VHID_HAPTICS_H
#define VHID_HAPTICS_H

#include <cstddef>
#include <cstdint>
#include <span>

#include "vhid/protocol.h"

namespace vhid {

bool output_has_haptics(const OutputState& output);
bool output_has_motor(const OutputState& output, size_t index);
void set_haptic_motor(OutputState& output, size_t index,
                      const HapticMotorState& motor);

void decode_switch1_hd_rumble(std::span<const uint8_t> rumble,
                              OutputState& output);
void encode_switch1_hd_rumble(const OutputState& output, uint8_t* out);

void encode_switch2_usb_rumble(const OutputState& output, uint8_t* out);
void encode_switch2_usb_motor_rumble(const OutputState& output,
                                     size_t motor_index, uint8_t* out);
void decode_switch2_usb_motor_rumble(std::span<const uint8_t> data,
                                     HapticMotorState& motor);
void decode_switch2_usb_rumble(std::span<const uint8_t> data,
                               OutputState& output);

}  // namespace vhid

#endif
