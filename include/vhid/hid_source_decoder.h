#ifndef VHID_HID_SOURCE_DECODER_H
#define VHID_HID_SOURCE_DECODER_H

#include <memory>
#include <span>
#include <string>

#include "vhid/protocol.h"

namespace vhid {

class HidSourceDecoder {
 public:
  virtual ~HidSourceDecoder() = default;

  virtual const DeviceDescription& description() const = 0;
  virtual bool decode_input(const ParsedHidReport& report,
                            InputState& state) = 0;

  static std::unique_ptr<HidSourceDecoder> create(
      const ParsedHidDeviceAdd& source, std::string& error);
};

}  // namespace vhid

#endif
