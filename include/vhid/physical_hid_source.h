#ifndef VHID_PHYSICAL_HID_SOURCE_H
#define VHID_PHYSICAL_HID_SOURCE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "vhid/protocol.h"

namespace vhid {

class PhysicalHidSource {
 public:
  using AddHandler =
      std::function<void(uint32_t, const DeviceDescription&)>;
  using InputHandler =
      std::function<void(uint32_t, uint32_t, const InputState&)>;
  using RemoveHandler = std::function<void(uint32_t)>;

  virtual ~PhysicalHidSource() = default;

  static std::unique_ptr<PhysicalHidSource> create(
      AddHandler add_handler, InputHandler input_handler,
      RemoveHandler remove_handler, bool seize, std::string& error);
};

}  // namespace vhid

#endif
