#include "vhid/physical_hid_source.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDDeviceKeys.h>
#include <IOKit/hid/IOHIDElement.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hid/IOHIDValue.h>
#include <IOKit/IOKitLib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <dispatch/dispatch.h>

namespace vhid {
namespace {

enum class BindingKind { button, hat, axis };

struct Binding {
  IOHIDElementRef element = nullptr;
  BindingKind kind = BindingKind::axis;
  uint8_t index = 0;
  uint32_t usage_page = 0;
  uint32_t usage = 0;
  CFIndex logical_min = 0;
  CFIndex logical_max = 0;
};

struct SourceDevice {
  uint32_t id = 0;
  uint32_t sequence = 0;
  InputState state{};
  std::unordered_map<IOHIDElementRef, Binding> bindings;
};

int32_t number_property(IOHIDDeviceRef device, CFStringRef key,
                        int32_t fallback = 0) {
  CFTypeRef value = IOHIDDeviceGetProperty(device, key);
  int32_t number = fallback;
  if (value && CFGetTypeID(value) == CFNumberGetTypeID())
    CFNumberGetValue(static_cast<CFNumberRef>(value),
                     kCFNumberSInt32Type, &number);
  return number;
}

std::string string_property(IOHIDDeviceRef device, CFStringRef key,
                            const char* fallback) {
  CFTypeRef value = IOHIDDeviceGetProperty(device, key);
  if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return fallback;
  std::array<char, 256> buffer{};
  if (!CFStringGetCString(static_cast<CFStringRef>(value), buffer.data(),
                          buffer.size(), kCFStringEncodingUTF8))
    return fallback;
  return buffer.data();
}

void copy_text(char* destination, size_t size, const std::string& source) {
  std::snprintf(destination, size, "%s", source.c_str());
}

bool is_axis_usage(uint32_t page, uint32_t usage) {
  if (page != kHIDPage_GenericDesktop) return false;
  switch (usage) {
    case kHIDUsage_GD_X:
    case kHIDUsage_GD_Y:
    case kHIDUsage_GD_Z:
    case kHIDUsage_GD_Rx:
    case kHIDUsage_GD_Ry:
    case kHIDUsage_GD_Rz:
    case kHIDUsage_GD_Slider:
    case kHIDUsage_GD_Dial:
    case kHIDUsage_GD_Wheel:
      return true;
    default:
      return false;
  }
}

CFMutableDictionaryRef match_dictionary(uint32_t usage) {
  CFMutableDictionaryRef dictionary = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  int32_t page = kHIDPage_GenericDesktop;
  int32_t requested_usage = static_cast<int32_t>(usage);
  CFNumberRef page_number =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &page);
  CFNumberRef usage_number =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                     &requested_usage);
  CFDictionarySetValue(dictionary, CFSTR(kIOHIDDeviceUsagePageKey),
                       page_number);
  CFDictionarySetValue(dictionary, CFSTR(kIOHIDDeviceUsageKey),
                       usage_number);
  CFRelease(page_number);
  CFRelease(usage_number);
  return dictionary;
}

class MacPhysicalHidSource final : public PhysicalHidSource {
 public:
  MacPhysicalHidSource(AddHandler add_handler, InputHandler input_handler,
                       RemoveHandler remove_handler)
      : add_handler_(std::move(add_handler)),
        input_handler_(std::move(input_handler)),
        remove_handler_(std::move(remove_handler)) {}

  ~MacPhysicalHidSource() override {
    if (!manager_) return;
    IOHIDManagerCancel(manager_);
    dispatch_semaphore_wait(
        cancelled_,
        dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    clear_devices();
    CFRelease(manager_);
  }

  bool start(bool seize, std::string& error) {
    manager_ = IOHIDManagerCreate(kCFAllocatorDefault,
                                  kIOHIDOptionsTypeNone);
    if (!manager_) {
      error = "IOHIDManagerCreate failed";
      return false;
    }
    CFMutableArrayRef matches = CFArrayCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    const uint32_t usages[] = {
        kHIDUsage_GD_Joystick,
        kHIDUsage_GD_GamePad,
        kHIDUsage_GD_MultiAxisController,
    };
    for (uint32_t usage : usages) {
      CFMutableDictionaryRef match = match_dictionary(usage);
      CFArrayAppendValue(matches, match);
      CFRelease(match);
    }
    IOHIDManagerSetDeviceMatchingMultiple(manager_, matches);
    CFRelease(matches);
    IOHIDManagerRegisterDeviceMatchingCallback(manager_, device_added,
                                                this);
    IOHIDManagerRegisterDeviceRemovalCallback(manager_, device_removed,
                                               this);
    IOHIDManagerRegisterInputValueCallback(manager_, input_value, this);
    queue_ = dispatch_queue_create("org.virtualhidbridge.physical-input",
                                   DISPATCH_QUEUE_SERIAL);
    cancelled_ = dispatch_semaphore_create(0);
    IOHIDManagerSetDispatchQueue(manager_, queue_);
    IOHIDManagerSetCancelHandler(manager_, ^{
      dispatch_semaphore_signal(cancelled_);
    });
    const IOReturn opened = IOHIDManagerOpen(
        manager_, seize ? kIOHIDOptionsTypeSeizeDevice
                        : kIOHIDOptionsTypeNone);
    if (opened != kIOReturnSuccess) {
      error = "IOHIDManagerOpen failed: " + std::to_string(opened);
      return false;
    }
    IOHIDManagerActivate(manager_);
    return true;
  }

 private:
  static void device_added(void* context, IOReturn, void*,
                           IOHIDDeviceRef device) {
    static_cast<MacPhysicalHidSource*>(context)->add_device(device);
  }

  static void device_removed(void* context, IOReturn, void*,
                             IOHIDDeviceRef device) {
    static_cast<MacPhysicalHidSource*>(context)->remove_device(device);
  }

  static void input_value(void* context, IOReturn, void*,
                          IOHIDValueRef value) {
    static_cast<MacPhysicalHidSource*>(context)->handle_value(value);
  }

  bool is_virtual(IOHIDDeviceRef device) const {
    CFTypeRef virtual_property =
        IOHIDDeviceGetProperty(device, CFSTR("HIDVirtualDevice"));
    if (virtual_property == kCFBooleanTrue) return true;
    return string_property(device, CFSTR(kIOHIDTransportKey), "") ==
           "Virtual";
  }

  uint32_t device_id(IOHIDDeviceRef device) const {
    uint64_t registry_id = 0;
    IORegistryEntryGetRegistryEntryID(IOHIDDeviceGetService(device),
                                      &registry_id);
    return 0x80000000u |
           static_cast<uint32_t>((registry_id ^ (registry_id >> 32)) &
                                 0x7fffffffu);
  }

  void add_device(IOHIDDeviceRef device) {
    if (is_virtual(device) || devices_.contains(device)) return;
    CFArrayRef element_array =
        IOHIDDeviceCopyMatchingElements(device, nullptr,
                                        kIOHIDOptionsTypeNone);
    if (!element_array) return;
    std::vector<Binding> buttons, hats, axes;
    const CFIndex count = CFArrayGetCount(element_array);
    for (CFIndex i = 0; i < count; ++i) {
      auto element = static_cast<IOHIDElementRef>(
          const_cast<void*>(CFArrayGetValueAtIndex(element_array, i)));
      const IOHIDElementType type = IOHIDElementGetType(element);
      if (type != kIOHIDElementTypeInput_Misc &&
          type != kIOHIDElementTypeInput_Button &&
          type != kIOHIDElementTypeInput_Axis) {
        continue;
      }
      Binding binding;
      binding.element = element;
      binding.usage_page = IOHIDElementGetUsagePage(element);
      binding.usage = IOHIDElementGetUsage(element);
      binding.logical_min = IOHIDElementGetLogicalMin(element);
      binding.logical_max = IOHIDElementGetLogicalMax(element);
      if (binding.usage_page == kHIDPage_Button) {
        binding.kind = BindingKind::button;
        buttons.push_back(binding);
      } else if (binding.usage_page == kHIDPage_GenericDesktop &&
                 binding.usage == kHIDUsage_GD_Hatswitch) {
        binding.kind = BindingKind::hat;
        hats.push_back(binding);
      } else if (is_axis_usage(binding.usage_page, binding.usage)) {
        binding.kind = BindingKind::axis;
        axes.push_back(binding);
      }
    }
    CFRelease(element_array);
    auto order = [](const Binding& a, const Binding& b) {
      if (a.usage_page != b.usage_page)
        return a.usage_page < b.usage_page;
      return a.usage < b.usage;
    };
    std::sort(buttons.begin(), buttons.end(), order);
    std::sort(hats.begin(), hats.end(), order);
    std::sort(axes.begin(), axes.end(), order);

    SourceDevice source;
    source.id = device_id(device);
    std::fill(std::begin(source.state.hats),
              std::end(source.state.hats), uint8_t{8});
    DeviceDescription description{};
    description.requested_profile =
        static_cast<uint8_t>(DeviceProfile::generic);
    description.button_count =
        static_cast<uint8_t>(std::min(buttons.size(), kMaxButtons));
    description.hat_count =
        static_cast<uint8_t>(std::min(hats.size(), kMaxHats));
    description.axis_count =
        static_cast<uint8_t>(std::min(axes.size(), kMaxAxes));
    description.vendor_id = static_cast<uint16_t>(
        number_property(device, CFSTR(kIOHIDVendorIDKey)));
    description.product_id = static_cast<uint16_t>(
        number_property(device, CFSTR(kIOHIDProductIDKey)));
    description.version_number = static_cast<uint16_t>(
        number_property(device, CFSTR(kIOHIDVersionNumberKey), 1));
    copy_text(description.product, sizeof(description.product),
              string_property(device, CFSTR(kIOHIDProductKey),
                              "Physical HID Controller"));
    copy_text(description.manufacturer, sizeof(description.manufacturer),
              string_property(device, CFSTR(kIOHIDManufacturerKey),
                              "Unknown"));
    copy_text(description.serial, sizeof(description.serial),
              string_property(device, CFSTR(kIOHIDSerialNumberKey), ""));

    auto install = [&](std::vector<Binding>& list, size_t maximum) {
      const size_t install_count = std::min(list.size(), maximum);
      for (size_t i = 0; i < install_count; ++i) {
        list[i].index = static_cast<uint8_t>(i);
        CFRetain(list[i].element);
        source.bindings.emplace(list[i].element, list[i]);
      }
    };
    install(buttons, kMaxButtons);
    install(hats, kMaxHats);
    install(axes, kMaxAxes);
    for (size_t i = 0; i < description.axis_count; ++i) {
      description.axes[i].usage_page =
          static_cast<uint16_t>(axes[i].usage_page);
      description.axes[i].usage = static_cast<uint16_t>(axes[i].usage);
      description.axes[i].logical_min = INT16_MIN;
      description.axes[i].logical_max = INT16_MAX;
    }
    const uint32_t id = source.id;
    devices_.emplace(device, std::move(source));
    if (add_handler_) add_handler_(id, description);
  }

  void remove_device(IOHIDDeviceRef device) {
    auto found = devices_.find(device);
    if (found == devices_.end()) return;
    const uint32_t id = found->second.id;
    for (const auto& [element, binding] : found->second.bindings)
      CFRelease(element);
    devices_.erase(found);
    if (remove_handler_) remove_handler_(id);
  }

  void handle_value(IOHIDValueRef value) {
    IOHIDElementRef element = IOHIDValueGetElement(value);
    IOHIDDeviceRef device = IOHIDElementGetDevice(element);
    auto device_found = devices_.find(device);
    if (device_found == devices_.end()) return;
    SourceDevice& source = device_found->second;
    auto binding_found = source.bindings.find(element);
    if (binding_found == source.bindings.end()) return;
    const Binding& binding = binding_found->second;
    const CFIndex raw = IOHIDValueGetIntegerValue(value);
    if (binding.kind == BindingKind::button) {
      const uint64_t mask = uint64_t{1} << binding.index;
      if (raw)
        source.state.buttons |= mask;
      else
        source.state.buttons &= ~mask;
    } else if (binding.kind == BindingKind::hat) {
      source.state.hats[binding.index] =
          raw >= binding.logical_min && raw <= binding.logical_max
              ? static_cast<uint8_t>(
                    std::clamp<CFIndex>(raw - binding.logical_min, 0, 7))
              : 8;
    } else if (binding.logical_max > binding.logical_min) {
      const double normalized =
          static_cast<double>(raw - binding.logical_min) /
          static_cast<double>(binding.logical_max - binding.logical_min);
      const long scaled =
          std::lround(normalized * 65535.0 - 32768.0);
      source.state.axes[binding.index] = static_cast<int16_t>(
          std::clamp(scaled, -32768l, 32767l));
    }
    if (input_handler_)
      input_handler_(source.id, ++source.sequence, source.state);
  }

  void clear_devices() {
    for (auto& [device, source] : devices_)
      for (const auto& [element, binding] : source.bindings)
        CFRelease(element);
    devices_.clear();
  }

  AddHandler add_handler_;
  InputHandler input_handler_;
  RemoveHandler remove_handler_;
  IOHIDManagerRef manager_ = nullptr;
  dispatch_queue_t queue_ = nullptr;
  dispatch_semaphore_t cancelled_ = nullptr;
  std::unordered_map<IOHIDDeviceRef, SourceDevice> devices_;
};

}  // namespace

std::unique_ptr<PhysicalHidSource> PhysicalHidSource::create(
    AddHandler add_handler, InputHandler input_handler,
    RemoveHandler remove_handler, bool seize, std::string& error) {
  auto source = std::make_unique<MacPhysicalHidSource>(
      std::move(add_handler), std::move(input_handler),
      std::move(remove_handler));
  if (!source->start(seize, error)) return nullptr;
  return source;
}

}  // namespace vhid
