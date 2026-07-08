#include "vhid/hid_profile.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDeviceKeys.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hidsystem/IOHIDUserDevice.h>
#include <mach/mach_time.h>

#include <dispatch/dispatch.h>

namespace vhid {
namespace {

void set_number(CFMutableDictionaryRef dictionary, CFStringRef key,
                int32_t value) {
  CFNumberRef number =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
  CFDictionarySetValue(dictionary, key, number);
  CFRelease(number);
}

void set_string(CFMutableDictionaryRef dictionary, CFStringRef key,
                const std::string& value) {
  CFStringRef string = CFStringCreateWithCString(
      kCFAllocatorDefault, value.c_str(), kCFStringEncodingUTF8);
  if (!string) return;
  CFDictionarySetValue(dictionary, key, string);
  CFRelease(string);
}

void set_usage_metadata(CFMutableDictionaryRef dictionary,
                        uint16_t usage_page, uint16_t usage) {
  if (!usage_page || !usage) return;
  set_number(dictionary, CFSTR(kIOHIDPrimaryUsagePageKey), usage_page);
  set_number(dictionary, CFSTR(kIOHIDPrimaryUsageKey), usage);
  set_number(dictionary, CFSTR(kIOHIDDeviceUsagePageKey), usage_page);
  set_number(dictionary, CFSTR(kIOHIDDeviceUsageKey), usage);

  CFMutableDictionaryRef pair = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  set_number(pair, CFSTR(kIOHIDDeviceUsagePageKey), usage_page);
  set_number(pair, CFSTR(kIOHIDDeviceUsageKey), usage);
  const void* values[] = {pair};
  CFArrayRef pairs =
      CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
  CFDictionarySetValue(dictionary, CFSTR(kIOHIDDeviceUsagePairsKey), pairs);
  CFRelease(pairs);
  CFRelease(pair);
}

class MacVirtualDevice final : public VirtualDevice {
 public:
  MacVirtualDevice(IOHIDUserDeviceRef device, dispatch_semaphore_t cancelled)
      : device_(device), cancelled_(cancelled) {}

  ~MacVirtualDevice() override {
    if (!device_) return;
    IOHIDUserDeviceCancel(device_);
    dispatch_semaphore_wait(
        cancelled_,
        dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    CFRelease(device_);
  }

  bool send(std::span<const uint8_t> report) override {
    if (!device_ || report.empty()) return false;
    return IOHIDUserDeviceHandleReportWithTimeStamp(
               device_, mach_absolute_time(), report.data(),
               static_cast<CFIndex>(report.size())) == kIOReturnSuccess;
  }

 private:
  IOHIDUserDeviceRef device_ = nullptr;
  dispatch_semaphore_t cancelled_ = nullptr;
};

}  // namespace

std::unique_ptr<VirtualDevice> VirtualDevice::create(
    const HidDeviceProperties& properties,
    std::shared_ptr<HidProfile> profile,
    OutputHandler output_handler,
    std::string& error) {
  return create_raw(
      properties,
      [profile = std::move(profile),
       output_handler = std::move(output_handler)](
          HidReportType type, uint8_t, std::span<const uint8_t> report) {
        if (type != HidReportType::output) return;
        OutputState output{};
        if (profile->decode_output(report, output) && output_handler)
          output_handler(output);
      },
      error);
}

std::unique_ptr<VirtualDevice> VirtualDevice::create_raw(
    const HidDeviceProperties& properties,
    RawReportHandler report_handler,
    std::string& error) {
  if (properties.report_descriptor.empty()) {
    error = "HID profile produced an empty report descriptor";
    return nullptr;
  }

  CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFDataRef descriptor = CFDataCreate(
      kCFAllocatorDefault, properties.report_descriptor.data(),
      static_cast<CFIndex>(properties.report_descriptor.size()));
  CFDictionarySetValue(dictionary, CFSTR(kIOHIDReportDescriptorKey),
                       descriptor);
  CFRelease(descriptor);
  set_number(dictionary, CFSTR(kIOHIDVendorIDKey), properties.vendor_id);
  set_number(dictionary, CFSTR(kIOHIDProductIDKey), properties.product_id);
  set_number(dictionary, CFSTR(kIOHIDVersionNumberKey),
             properties.version_number);
  if (properties.vendor_id_source)
    set_number(dictionary, CFSTR(kIOHIDVendorIDSourceKey),
               static_cast<int32_t>(properties.vendor_id_source));
  set_usage_metadata(dictionary, properties.primary_usage_page,
                     properties.primary_usage);
  set_string(dictionary, CFSTR(kIOHIDProductKey), properties.product);
  set_string(dictionary, CFSTR(kIOHIDManufacturerKey),
             properties.manufacturer);
  set_string(dictionary, CFSTR(kIOHIDSerialNumberKey), properties.serial);
  set_string(dictionary, CFSTR(kIOHIDTransportKey), properties.transport);

  IOHIDUserDeviceRef device = IOHIDUserDeviceCreateWithProperties(
      kCFAllocatorDefault, dictionary,
      IOHIDUserDeviceOptionsCreateOnActivate);
  CFRelease(dictionary);
  if (!device) {
    error =
        "IOHIDUserDevice creation failed; verify AMFI/signing and the "
        "com.apple.developer.hid.virtual.device entitlement";
    return nullptr;
  }

  dispatch_queue_t queue =
      dispatch_queue_create("org.virtualhidbridge.output",
                            DISPATCH_QUEUE_SERIAL);
  dispatch_semaphore_t cancelled = dispatch_semaphore_create(0);
  IOHIDUserDeviceRegisterSetReportBlock(
      device, ^IOReturn(IOHIDReportType type, uint32_t report_id,
                        const uint8_t* report, CFIndex length) {
        if (length < 0)
          return kIOReturnUnsupported;
        HidReportType wire_type;
        switch (type) {
          case kIOHIDReportTypeOutput:
            wire_type = HidReportType::output;
            break;
          case kIOHIDReportTypeFeature:
            wire_type = HidReportType::feature;
            break;
          default:
            return kIOReturnUnsupported;
        }
        if (report_handler)
          report_handler(
              wire_type, static_cast<uint8_t>(report_id),
              std::span<const uint8_t>(
                  report, static_cast<size_t>(length)));
        return kIOReturnSuccess;
      });
  IOHIDUserDeviceSetDispatchQueue(device, queue);
  IOHIDUserDeviceSetCancelHandler(device, ^{
    dispatch_semaphore_signal(cancelled);
  });
  IOHIDUserDeviceActivate(device);
  return std::make_unique<MacVirtualDevice>(device, cancelled);
}

}  // namespace vhid
