#ifndef VHID_HID_PROFILE_H
#define VHID_HID_PROFILE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "vhid/protocol.h"

namespace vhid {

struct HidDeviceProperties {
  uint16_t vendor_id = 0;
  uint16_t product_id = 0;
  uint16_t version_number = 1;
  std::string product;
  std::string manufacturer;
  std::string serial;
  std::string transport = "Virtual";
  std::vector<uint8_t> report_descriptor;
};

class HidProfile {
 public:
  virtual ~HidProfile() = default;
  virtual const HidDeviceProperties& properties() const = 0;
  virtual std::vector<uint8_t> encode(const InputState& state) const = 0;
  virtual bool decode_output(std::span<const uint8_t> report,
                             OutputState& output) const = 0;
};

std::unique_ptr<HidProfile> make_profile(
    const DeviceDescription& description);

class VirtualDevice {
 public:
  using OutputHandler = std::function<void(const OutputState&)>;
  using RawReportHandler =
      std::function<void(HidReportType, uint8_t, std::span<const uint8_t>)>;

  virtual ~VirtualDevice() = default;
  virtual bool send(std::span<const uint8_t> report) = 0;

  static std::unique_ptr<VirtualDevice> create(
      const HidDeviceProperties& properties,
      std::shared_ptr<HidProfile> profile,
      OutputHandler output_handler,
      std::string& error);
  static std::unique_ptr<VirtualDevice> create_raw(
      const HidDeviceProperties& properties,
      RawReportHandler report_handler,
      std::string& error);
};

}  // namespace vhid

#endif
