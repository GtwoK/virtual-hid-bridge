#ifndef VHID_MAPPING_H
#define VHID_MAPPING_H

#include <array>
#include <cstdint>

#include "vhid/protocol.h"

namespace vhid {

struct AxisCalibration {
  int16_t minimum = INT16_MIN;
  int16_t center = 0;
  int16_t maximum = INT16_MAX;
  float inner_deadzone = 0.0f;
  float outer_deadzone = 0.0f;
  float curve = 1.0f;
  bool invert = false;
  bool unipolar = false;
};

struct AxisRoute {
  int8_t source_axis = -1;
  AxisCalibration calibration{};
};

struct ControllerMapping {
  std::array<int8_t, kMaxButtons> source_button_for_output{};
  std::array<int8_t, kMaxHats> source_hat_for_output{};
  std::array<AxisRoute, kMaxAxes> axes{};

  static ControllerMapping identity();
};

int16_t apply_axis_calibration(int16_t value,
                               const AxisCalibration& calibration);
InputState apply_mapping(const InputState& source,
                         const ControllerMapping& mapping);

}  // namespace vhid

#endif
