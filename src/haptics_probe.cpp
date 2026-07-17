#include "vhid/haptics.h"
#include "vhid/hid_profile.h"
#include "vhid/protocol.h"
#include "vhid/source_codec.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDeviceKeys.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDLib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
  bool check = false;
  bool send = false;
  bool list = false;
  bool allow_real = false;
  bool payload_only = false;
  uint16_t vendor_id = 0x057e;
  uint16_t product_id = 0x2009;
  int delay_ms = 70;
  int repeat = 1;
  std::string product_filter = "Pro Controller";
  std::string serial_filter;
};

struct Switch2MotorFields {
  uint16_t high_frequency = 0;
  uint16_t high_amplitude = 0;
  uint16_t low_frequency = 0;
  uint16_t low_amplitude = 0;
  uint16_t high_frequency_hz = 0;
  uint16_t low_frequency_hz = 0;
  uint16_t high_amplitude_u16 = 0;
  uint16_t low_amplitude_u16 = 0;
};

struct ProbeCase {
  const char* name = "";
  std::array<uint8_t, 8> switch1_rumble{};
};

struct ProbeResult {
  ProbeCase probe;
  std::array<uint8_t, 10> host_report{};
  vhid::OutputState decoded{};
  vhid::SourceReport source_report;
  Switch2MotorFields source_left;
  Switch2MotorFields source_right;
};

std::string hex_byte(uint8_t value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(2) << unsigned(value);
  return out.str();
}

std::string hex_word(uint16_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::setfill('0') << std::setw(3)
      << unsigned(value);
  return out.str();
}

std::string bytes(std::span<const uint8_t> data) {
  std::ostringstream out;
  for (size_t i = 0; i < data.size(); ++i) {
    if (i) out << ' ';
    out << hex_byte(data[i]);
  }
  return out.str();
}

vhid::DeviceDescription switch_pro_description() {
  vhid::DeviceDescription description{};
  description.requested_profile =
      static_cast<uint8_t>(vhid::DeviceProfile::switch_pro);
  description.device_flags =
      vhid::kDeviceHasBattery | vhid::kDeviceHasMotion |
      vhid::kDeviceHasRumble | vhid::kDeviceHasPlayerLeds;
  description.button_count = 18;
  description.hat_count = 1;
  description.axis_count = 4;
  description.motion_flags =
      vhid::kMotionAcceleration | vhid::kMotionAngularVelocity;
  description.vendor_id = 0x057e;
  description.product_id = 0x2009;
  description.version_number = 1;
  std::strcpy(description.product, "Pro Controller");
  std::strcpy(description.manufacturer, "Nintendo Co., Ltd.");
  std::strcpy(description.serial, "vhid-haptics-probe");
  return description;
}

std::vector<ProbeCase> probe_cases() {
  return {
      {"neutral",
       {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40}},
      {"tone-320",
       {0x00, 0x81, 0x40, 0x60, 0x00, 0x81, 0x40, 0x60}},
      {"tone-380",
       {0x20, 0x81, 0x48, 0x60, 0x20, 0x81, 0x48, 0x60}},
      {"tone-453",
       {0x40, 0x81, 0x50, 0x60, 0x40, 0x81, 0x50, 0x60}},
      {"tone-539",
       {0x60, 0x81, 0x58, 0x60, 0x60, 0x81, 0x58, 0x60}},
      {"split-motor",
       {0x00, 0x81, 0x40, 0x60, 0x80, 0x81, 0x60, 0x60}},
      {"stop",
       {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40}},
  };
}

Switch2MotorFields parse_switch2_motor(std::span<const uint8_t> data,
                                        size_t offset) {
  Switch2MotorFields fields{};
  if (offset + 5 > data.size()) return fields;
  fields.high_frequency =
      static_cast<uint16_t>(data[offset] |
                            ((data[offset + 1] & 0x03u) << 8));
  fields.high_amplitude =
      static_cast<uint16_t>(((data[offset + 1] & 0xFCu) << 4) |
                            ((data[offset + 2] & 0x0Fu) << 12));
  fields.low_frequency =
      static_cast<uint16_t>((data[offset + 2] >> 4) |
                            ((data[offset + 3] & 0x3Fu) << 4));
  fields.low_amplitude =
      static_cast<uint16_t>((data[offset + 4] << 8) |
                            (data[offset + 3] & 0xC0u));

  vhid::OutputState decoded{};
  vhid::decode_switch2_usb_rumble(data.subspan(offset, 5), decoded);
  fields.high_frequency_hz = decoded.motors[0].high.frequency_hz;
  fields.low_frequency_hz = decoded.motors[0].low.frequency_hz;
  fields.high_amplitude_u16 = decoded.motors[0].high.amplitude;
  fields.low_amplitude_u16 = decoded.motors[0].low.amplitude;
  return fields;
}

std::vector<ProbeResult> run_probe() {
  auto profile = vhid::make_profile(switch_pro_description());
  auto source_codec =
      vhid::make_source_output_codec(vhid::DeviceProfile::switch_2_pro);
  if (!profile || !source_codec) return {};

  std::vector<ProbeResult> results;
  uint8_t sequence = 0;
  for (const auto& probe : probe_cases()) {
    ProbeResult result;
    result.probe = probe;
    result.host_report[0] = 0x10;
    result.host_report[1] = sequence++ & 0x0f;
    std::copy(probe.switch1_rumble.begin(), probe.switch1_rumble.end(),
              result.host_report.begin() + 2);
    if (!profile->decode_output(result.host_report, result.decoded))
      continue;
    if (!source_codec->encode_output(result.decoded, result.source_report))
      continue;
    result.source_left = parse_switch2_motor(result.source_report.data, 2);
    result.source_right = parse_switch2_motor(result.source_report.data, 0x12);
    results.push_back(std::move(result));
  }
  return results;
}

bool validate_results(const std::vector<ProbeResult>& results,
                      std::string& error) {
  if (results.size() != probe_cases().size()) {
    error = "not every probe case decoded and encoded";
    return false;
  }

  std::set<uint16_t> decoded_high_hz;
  std::set<uint16_t> decoded_low_hz;
  std::set<uint16_t> source_high_codes;
  std::set<uint16_t> source_low_codes;
  for (const auto& result : results) {
    if (result.source_report.type != vhid::HidReportType::output ||
        result.source_report.report_id != 0 ||
        result.source_report.data.size() != 64 ||
        result.source_report.data[0] != 0x02) {
      error = "Switch 2 source report shape is wrong";
      return false;
    }
    const bool active =
        result.decoded.high_frequency || result.decoded.low_frequency;
    if (!active) continue;
    decoded_high_hz.insert(result.decoded.motors[0].high.frequency_hz);
    decoded_low_hz.insert(result.decoded.motors[0].low.frequency_hz);
    source_high_codes.insert(result.source_left.high_frequency);
    source_low_codes.insert(result.source_left.low_frequency);
  }

  if (decoded_high_hz.size() < 4 || decoded_low_hz.size() < 4) {
    error = "Switch 1 sweep did not decode as varying frequencies";
    return false;
  }
  if (source_high_codes.size() < 4 || source_low_codes.size() < 4) {
    error = "Switch 2 source encoding flattened the frequency sweep";
    return false;
  }
  return true;
}

void print_results(const std::vector<ProbeResult>& results) {
  std::cout << "Switch 1 HD rumble -> Switch 2 USB rumble probe\n";
  for (const auto& result : results) {
    const auto& left = result.decoded.motors[0];
    const auto& right = result.decoded.motors[1];
    std::cout << result.probe.name << "\n"
              << "  host report: " << bytes(result.host_report) << "\n"
              << "  decoded Switch 1 left:  hi=" << left.high.frequency_hz
              << "Hz/" << hex_word(left.high.encoded_frequency)
              << " amp=" << left.high.amplitude
              << " low=" << left.low.frequency_hz << "Hz/"
              << hex_word(left.low.encoded_frequency)
              << " amp=" << left.low.amplitude << "\n"
              << "  decoded Switch 1 right: hi=" << right.high.frequency_hz
              << "Hz/" << hex_word(right.high.encoded_frequency)
              << " amp=" << right.high.amplitude
              << " low=" << right.low.frequency_hz << "Hz/"
              << hex_word(right.low.encoded_frequency)
              << " amp=" << right.low.amplitude << "\n"
              << "  source report: id=0x" << std::hex
              << unsigned(result.source_report.report_id) << std::dec
              << " bytes=" << result.source_report.data.size()
              << " data="
              << bytes(std::span<const uint8_t>(
                     result.source_report.data.data(),
                     std::min<size_t>(result.source_report.data.size(), 24)))
              << (result.source_report.data.size() > 24 ? " ..." : "")
              << "\n"
              << "  encoded Switch 2 left:  hi="
              << result.source_left.high_frequency_hz << "Hz/"
              << hex_word(result.source_left.high_frequency)
              << " amp=" << result.source_left.high_amplitude_u16
              << " low=" << result.source_left.low_frequency_hz << "Hz/"
              << hex_word(result.source_left.low_frequency)
              << " amp=" << result.source_left.low_amplitude_u16 << "\n"
              << "  encoded Switch 2 right: hi="
              << result.source_right.high_frequency_hz << "Hz/"
              << hex_word(result.source_right.high_frequency)
              << " amp=" << result.source_right.high_amplitude_u16
              << " low=" << result.source_right.low_frequency_hz << "Hz/"
              << hex_word(result.source_right.low_frequency)
              << " amp=" << result.source_right.low_amplitude_u16 << "\n";
  }
}

void set_matching_number(CFMutableDictionaryRef dictionary, CFStringRef key,
                         int32_t value) {
  CFNumberRef number =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
  CFDictionarySetValue(dictionary, key, number);
  CFRelease(number);
}

std::string cf_string(CFTypeRef value) {
  if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
  char buffer[512]{};
  if (!CFStringGetCString(static_cast<CFStringRef>(value), buffer,
                          sizeof(buffer), kCFStringEncodingUTF8)) {
    return {};
  }
  return buffer;
}

bool cf_bool(CFTypeRef value) {
  if (!value || CFGetTypeID(value) != CFBooleanGetTypeID()) return false;
  return CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}

std::string device_string(IOHIDDeviceRef device, CFStringRef key) {
  return cf_string(IOHIDDeviceGetProperty(device, key));
}

bool is_bridge_device(IOHIDDeviceRef device) {
  return cf_bool(IOHIDDeviceGetProperty(device,
                                        CFSTR("VirtualHIDBridgeDevice")));
}

std::vector<IOHIDDeviceRef> matching_devices(IOHIDManagerRef manager,
                                             const Options& options) {
  CFSetRef set = IOHIDManagerCopyDevices(manager);
  if (!set) return {};
  const CFIndex count = CFSetGetCount(set);
  std::vector<const void*> values(static_cast<size_t>(count));
  CFSetGetValues(set, values.data());
  std::vector<IOHIDDeviceRef> devices;
  for (const void* value : values) {
    auto device = static_cast<IOHIDDeviceRef>(const_cast<void*>(value));
    const std::string product =
        device_string(device, CFSTR(kIOHIDProductKey));
    const std::string serial =
        device_string(device, CFSTR(kIOHIDSerialNumberKey));
    if (!options.product_filter.empty() &&
        product.find(options.product_filter) == std::string::npos) {
      continue;
    }
    if (!options.serial_filter.empty() &&
        serial.find(options.serial_filter) == std::string::npos) {
      continue;
    }
    if (!options.allow_real && !is_bridge_device(device)) continue;
    devices.push_back(device);
  }
  CFRelease(set);
  return devices;
}

void print_device(IOHIDDeviceRef device) {
  std::cout << "  product=\""
            << device_string(device, CFSTR(kIOHIDProductKey))
            << "\" manufacturer=\""
            << device_string(device, CFSTR(kIOHIDManufacturerKey))
            << "\" serial=\""
            << device_string(device, CFSTR(kIOHIDSerialNumberKey))
            << "\" virtual-hid-bridge="
            << (is_bridge_device(device) ? "yes" : "no") << "\n";
}

bool send_reports(const Options& options,
                  const std::vector<ProbeResult>& results) {
  IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault,
                                               kIOHIDOptionsTypeNone);
  if (!manager) {
    std::cerr << "failed to create IOHIDManager\n";
    return false;
  }
  CFMutableDictionaryRef matching = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  set_matching_number(matching, CFSTR(kIOHIDVendorIDKey), options.vendor_id);
  set_matching_number(matching, CFSTR(kIOHIDProductIDKey), options.product_id);
  IOHIDManagerSetDeviceMatching(manager, matching);
  CFRelease(matching);

  IOReturn status = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
  if (status != kIOReturnSuccess) {
    std::cerr << "failed to open IOHIDManager: 0x" << std::hex << status
              << std::dec << "\n";
    CFRelease(manager);
    return false;
  }

  const auto devices = matching_devices(manager, options);
  if (options.list) {
    if (devices.empty())
      std::cout << "no matching HID devices\n";
    else
      for (auto device : devices) print_device(device);
  }
  if (!options.send) {
    CFRelease(manager);
    return true;
  }
  if (devices.empty()) {
    std::cerr << "no matching virtual HID bridge controller found\n";
    CFRelease(manager);
    return false;
  }
  if (devices.size() > 1) {
    std::cerr << "multiple matching controllers found; use --serial\n";
    for (auto device : devices) print_device(device);
    CFRelease(manager);
    return false;
  }

  IOHIDDeviceRef device = devices.front();
  status = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
  if (status != kIOReturnSuccess) {
    std::cerr << "failed to open HID device: 0x" << std::hex << status
              << std::dec << "\n";
    CFRelease(manager);
    return false;
  }

  for (int pass = 0; pass < options.repeat; ++pass) {
    for (const auto& result : results) {
      const uint8_t* report = result.host_report.data();
      CFIndex report_size =
          static_cast<CFIndex>(result.host_report.size());
      if (options.payload_only) {
        report = result.host_report.data() + 1;
        report_size = static_cast<CFIndex>(result.host_report.size() - 1);
      }
      status = IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, 0x10,
                                    report, report_size);
      if (status != kIOReturnSuccess) {
        std::cerr << "failed to send " << result.probe.name
                  << ": 0x" << std::hex << status << std::dec << "\n";
        IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
        CFRelease(manager);
        return false;
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(options.delay_ms));
    }
  }

  IOHIDDeviceClose(device, kIOHIDOptionsTypeNone);
  CFRelease(manager);
  return true;
}

void usage(const char* name) {
  std::cerr
      << "usage: " << name << " [--check] [--send] [--list]\n"
      << "       [--serial text] [--product text] [--allow-real]\n"
      << "       [--payload-only] [--repeat n] [--delay-ms n]\n";
}

bool parse_int(const char* value, int& out) {
  try {
    size_t parsed = 0;
    const int result = std::stoi(value, &parsed, 0);
    if (parsed != std::strlen(value)) return false;
    out = result;
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--check") {
      options.check = true;
    } else if (argument == "--send") {
      options.send = true;
    } else if (argument == "--list") {
      options.list = true;
    } else if (argument == "--allow-real") {
      options.allow_real = true;
    } else if (argument == "--payload-only") {
      options.payload_only = true;
    } else if (argument == "--serial" && i + 1 < argc) {
      options.serial_filter = argv[++i];
    } else if (argument == "--product" && i + 1 < argc) {
      options.product_filter = argv[++i];
    } else if (argument == "--repeat" && i + 1 < argc) {
      if (!parse_int(argv[++i], options.repeat) || options.repeat < 1)
        return false;
    } else if (argument == "--delay-ms" && i + 1 < argc) {
      if (!parse_int(argv[++i], options.delay_ms) || options.delay_ms < 0)
        return false;
    } else if (argument == "--help" || argument == "-h") {
      usage(argv[0]);
      std::exit(0);
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    usage(argv[0]);
    return 2;
  }

  const auto results = run_probe();
  std::string validation_error;
  if (!validate_results(results, validation_error)) {
    std::cerr << "haptics probe failed: " << validation_error << "\n";
    if (!results.empty()) print_results(results);
    return 1;
  }

  if (!options.check) print_results(results);
  if (options.send || options.list) {
    if (!send_reports(options, results)) return 1;
  }
  if (options.check) std::cout << "haptics probe passed\n";
  return 0;
}
