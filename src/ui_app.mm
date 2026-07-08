#import <AppKit/AppKit.h>

@interface DeviceRow : NSObject
@property(nonatomic, copy) NSString* deviceId;
@property(nonatomic, copy) NSString* status;
@property(nonatomic, copy) NSString* product;
@property(nonatomic, copy) NSString* lastEvent;
@property(nonatomic, copy) NSString* inputCount;
@end

@implementation DeviceRow
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSTableViewDataSource>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSTextField* executableField;
@property(nonatomic, strong) NSTextField* bindField;
@property(nonatomic, strong) NSTextField* portField;
@property(nonatomic, strong) NSTextField* sourceField;
@property(nonatomic, strong) NSTextField* vendorIdField;
@property(nonatomic, strong) NSTextField* productIdField;
@property(nonatomic, strong) NSTextField* versionField;
@property(nonatomic, strong) NSTextField* productNameField;
@property(nonatomic, strong) NSTextField* manufacturerField;
@property(nonatomic, strong) NSTextField* serialField;
@property(nonatomic, strong) NSPopUpButton* transportPopup;
@property(nonatomic, strong) NSPopUpButton* profilePopup;
@property(nonatomic, strong) NSTextField* statusLabel;
@property(nonatomic, strong) NSButton* startStopButton;
@property(nonatomic, strong) NSButton* noPhysicalCheck;
@property(nonatomic, strong) NSButton* dryRunCheck;
@property(nonatomic, strong) NSTextView* logView;
@property(nonatomic, strong) NSTableView* tableView;
@property(nonatomic, strong) NSMutableArray<DeviceRow*>* devices;
@property(nonatomic, strong) NSMutableString* lineBuffer;
@property(nonatomic, strong) NSTask* task;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  (void)notification;
  self.devices = [NSMutableArray array];
  self.lineBuffer = [NSMutableString string];
  [self buildMenu];
  [self buildWindow];
  [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  (void)sender;
  return YES;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
  (void)notification;
  [self stopBridge];
}

- (void)buildMenu {
  NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@""];
  NSMenuItem* appItem = [[NSMenuItem alloc] initWithTitle:@""
                                                   action:nil
                                            keyEquivalent:@""];
  [mainMenu addItem:appItem];

  NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"Virtual HID Bridge"];
  NSString* quitTitle = @"Quit Virtual HID Bridge";
  NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:quitTitle
                                                    action:@selector(terminate:)
                                             keyEquivalent:@"q"];
  [appMenu addItem:quitItem];
  [appItem setSubmenu:appMenu];
  [NSApp setMainMenu:mainMenu];
}

- (NSTextField*)label:(NSString*)text frame:(NSRect)frame {
  NSTextField* field = [[NSTextField alloc] initWithFrame:frame];
  field.stringValue = text;
  field.editable = NO;
  field.bezeled = NO;
  field.drawsBackground = NO;
  field.selectable = NO;
  return field;
}

- (NSTextField*)textField:(NSString*)value frame:(NSRect)frame {
  NSTextField* field = [[NSTextField alloc] initWithFrame:frame];
  field.stringValue = value;
  return field;
}

- (NSString*)defaultBridgePath {
  NSString* executableDir =
      NSBundle.mainBundle.executablePath.stringByDeletingLastPathComponent;
  NSString* bundled =
      [executableDir stringByAppendingPathComponent:@"vhid-bridge"];
  if ([NSFileManager.defaultManager isExecutableFileAtPath:bundled]) {
    return bundled;
  }
  NSString* bundlePath = NSBundle.mainBundle.bundlePath;
  NSString* buildDir = bundlePath.stringByDeletingLastPathComponent;
  NSString* sibling = [buildDir stringByAppendingPathComponent:@"vhid-bridge"];
  if ([NSFileManager.defaultManager isExecutableFileAtPath:sibling]) {
    return sibling;
  }
  NSString* cwd = NSFileManager.defaultManager.currentDirectoryPath;
  return [cwd stringByAppendingPathComponent:@"vhid-bridge"];
}

- (void)buildWindow {
  self.window = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(0, 0, 980, 760)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskResizable |
                          NSWindowStyleMaskMiniaturizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  self.window.title = @"Virtual HID Bridge";
  [self.window center];

  NSView* content = self.window.contentView;

  CGFloat y = 715;
  [content addSubview:[self label:@"Bridge executable"
                            frame:NSMakeRect(20, y, 130, 22)]];
  self.executableField =
      [self textField:[self defaultBridgePath] frame:NSMakeRect(155, y, 650, 22)];
  [content addSubview:self.executableField];

  self.startStopButton = [[NSButton alloc] initWithFrame:NSMakeRect(825, y - 1, 130, 26)];
  self.startStopButton.title = @"Start";
  self.startStopButton.bezelStyle = NSBezelStyleRounded;
  self.startStopButton.target = self;
  self.startStopButton.action = @selector(toggleBridge:);
  [content addSubview:self.startStopButton];

  y -= 38;
  [content addSubview:[self label:@"Bind"
                            frame:NSMakeRect(20, y, 45, 22)]];
  self.bindField = [self textField:@"0.0.0.0" frame:NSMakeRect(65, y, 110, 22)];
  [content addSubview:self.bindField];

  [content addSubview:[self label:@"Port" frame:NSMakeRect(190, y, 45, 22)]];
  self.portField = [self textField:@"48660" frame:NSMakeRect(235, y, 80, 22)];
  [content addSubview:self.portField];

  [content addSubview:[self label:@"UDP source"
                            frame:NSMakeRect(335, y, 90, 22)]];
  self.sourceField = [self textField:@"" frame:NSMakeRect(425, y, 200, 22)];
  self.sourceField.placeholderString = @"host[:port]";
  [content addSubview:self.sourceField];

  self.noPhysicalCheck = [[NSButton alloc] initWithFrame:NSMakeRect(645, y - 2, 120, 24)];
  self.noPhysicalCheck.title = @"No physical";
  self.noPhysicalCheck.buttonType = NSButtonTypeSwitch;
  self.noPhysicalCheck.state = NSControlStateValueOn;
  [content addSubview:self.noPhysicalCheck];

  self.dryRunCheck = [[NSButton alloc] initWithFrame:NSMakeRect(765, y - 2, 90, 24)];
  self.dryRunCheck.title = @"Dry run";
  self.dryRunCheck.buttonType = NSButtonTypeSwitch;
  [content addSubview:self.dryRunCheck];

  y -= 40;
  NSTextField* identityTitle =
      [self label:@"Virtual device identity overrides (blank = source/default)"
            frame:NSMakeRect(20, y, 460, 22)];
  identityTitle.font = [NSFont boldSystemFontOfSize:13];
  [content addSubview:identityTitle];

  [content addSubview:[self label:@"Output profile"
                            frame:NSMakeRect(610, y, 95, 22)]];
  self.profilePopup =
      [[NSPopUpButton alloc] initWithFrame:NSMakeRect(710, y - 2, 220, 26)
                                 pullsDown:NO];
  [self.profilePopup addItemsWithTitles:@[
    @"Source/default", @"Generic HID", @"Standard Gamepad",
    @"Switch 1 Pro Controller"
  ]];
  [content addSubview:self.profilePopup];

  y -= 30;
  [content addSubview:[self label:@"VID"
                            frame:NSMakeRect(20, y, 35, 22)]];
  self.vendorIdField = [self textField:@"" frame:NSMakeRect(55, y, 80, 22)];
  self.vendorIdField.placeholderString = @"0x1209";
  [content addSubview:self.vendorIdField];

  [content addSubview:[self label:@"PID"
                            frame:NSMakeRect(150, y, 35, 22)]];
  self.productIdField = [self textField:@"" frame:NSMakeRect(185, y, 80, 22)];
  self.productIdField.placeholderString = @"0x0001";
  [content addSubview:self.productIdField];

  [content addSubview:[self label:@"Version"
                            frame:NSMakeRect(280, y, 60, 22)]];
  self.versionField = [self textField:@"" frame:NSMakeRect(340, y, 70, 22)];
  self.versionField.placeholderString = @"1";
  [content addSubview:self.versionField];

  [content addSubview:[self label:@"Transport"
                            frame:NSMakeRect(430, y, 75, 22)]];
  self.transportPopup =
      [[NSPopUpButton alloc] initWithFrame:NSMakeRect(505, y - 2, 165, 26)
                                 pullsDown:NO];
  [self.transportPopup addItemsWithTitles:@[
    @"Source/default", @"Virtual", @"USB", @"Bluetooth",
    @"Bluetooth Low Energy", @"Network"
  ]];
  [content addSubview:self.transportPopup];

  y -= 34;
  [content addSubview:[self label:@"Product"
                            frame:NSMakeRect(20, y, 75, 22)]];
  self.productNameField =
      [self textField:@"" frame:NSMakeRect(95, y, 230, 22)];
  self.productNameField.placeholderString = @"Virtual HID Gamepad";
  [content addSubview:self.productNameField];

  [content addSubview:[self label:@"Manufacturer"
                            frame:NSMakeRect(345, y, 105, 22)]];
  self.manufacturerField =
      [self textField:@"" frame:NSMakeRect(450, y, 210, 22)];
  self.manufacturerField.placeholderString = @"Virtual HID Bridge";
  [content addSubview:self.manufacturerField];

  [content addSubview:[self label:@"Serial"
                            frame:NSMakeRect(680, y, 55, 22)]];
  self.serialField = [self textField:@"" frame:NSMakeRect(735, y, 190, 22)];
  self.serialField.placeholderString = @"stable-per-controller";
  [content addSubview:self.serialField];

  self.statusLabel = [self label:@"Stopped"
                           frame:NSMakeRect(20, 530, 940, 22)];
  [content addSubview:self.statusLabel];

  NSTextField* hint = [self label:@"Virtual HID publishing requires signing the bundled helper with the entitlement; the local signing target handles the helper and app."
                            frame:NSMakeRect(20, 506, 940, 22)];
  hint.textColor = NSColor.secondaryLabelColor;
  [content addSubview:hint];

  NSScrollView* tableScroll =
      [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 300, 940, 190)];
  tableScroll.hasVerticalScroller = YES;
  self.tableView = [[NSTableView alloc] initWithFrame:tableScroll.bounds];
  self.tableView.dataSource = self;
  [self addColumn:@"id" title:@"ID" width:50];
  [self addColumn:@"status" title:@"Status" width:90];
  [self addColumn:@"product" title:@"Product" width:330];
  [self addColumn:@"input" title:@"Input reports" width:110];
  [self addColumn:@"event" title:@"Last event" width:340];
  tableScroll.documentView = self.tableView;
  [content addSubview:tableScroll];

  NSScrollView* logScroll =
      [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 20, 940, 265)];
  logScroll.hasVerticalScroller = YES;
  self.logView = [[NSTextView alloc] initWithFrame:logScroll.bounds];
  self.logView.editable = NO;
  self.logView.font = [NSFont monospacedSystemFontOfSize:12
                                                  weight:NSFontWeightRegular];
  logScroll.documentView = self.logView;
  [content addSubview:logScroll];

  [self.window makeKeyAndOrderFront:nil];
}

- (void)addColumn:(NSString*)identifier title:(NSString*)title width:(CGFloat)width {
  NSTableColumn* column =
      [[NSTableColumn alloc] initWithIdentifier:identifier];
  column.title = title;
  column.width = width;
  [self.tableView addTableColumn:column];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
  (void)tableView;
  return (NSInteger)self.devices.count;
}

- (id)tableView:(NSTableView*)tableView
    objectValueForTableColumn:(NSTableColumn*)tableColumn
                          row:(NSInteger)row {
  (void)tableView;
  DeviceRow* device = self.devices[(NSUInteger)row];
  NSString* identifier = tableColumn.identifier;
  if ([identifier isEqualToString:@"id"]) return device.deviceId;
  if ([identifier isEqualToString:@"status"]) return device.status;
  if ([identifier isEqualToString:@"product"]) return device.product;
  if ([identifier isEqualToString:@"input"]) return device.inputCount;
  if ([identifier isEqualToString:@"event"]) return device.lastEvent;
  return @"";
}

- (void)toggleBridge:(id)sender {
  (void)sender;
  if (self.task && self.task.isRunning) {
    [self stopBridge];
  } else {
    [self startBridge];
  }
}

- (NSString*)trimmedField:(NSTextField*)field {
  return [field.stringValue
      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

- (void)addArgument:(NSString*)flag
          fromField:(NSTextField*)field
          arguments:(NSMutableArray<NSString*>*)arguments {
  NSString* value = [self trimmedField:field];
  if (!value.length) return;
  [arguments addObject:flag];
  [arguments addObject:value];
}

- (void)startBridge {
  NSString* executable = self.executableField.stringValue;
  if (![NSFileManager.defaultManager isExecutableFileAtPath:executable]) {
    [self appendLine:[NSString stringWithFormat:@"error: bridge executable is not executable: %@",
                                                executable]];
    return;
  }

  NSMutableArray<NSString*>* arguments = [NSMutableArray array];
  if ([self trimmedField:self.bindField].length) {
    [arguments addObject:@"--bind"];
    [arguments addObject:[self trimmedField:self.bindField]];
  }
  if ([self trimmedField:self.portField].length) {
    [arguments addObject:@"--listen-port"];
    [arguments addObject:[self trimmedField:self.portField]];
  }
  if ([self trimmedField:self.sourceField].length) {
    [arguments addObject:@"--udp-source"];
    [arguments addObject:[self trimmedField:self.sourceField]];
  }
  if (self.noPhysicalCheck.state == NSControlStateValueOn) {
    [arguments addObject:@"--no-physical"];
  }
  if (self.dryRunCheck.state == NSControlStateValueOn) {
    [arguments addObject:@"--dry-run"];
  }
  NSString* profile = self.profilePopup.titleOfSelectedItem;
  if ([profile isEqualToString:@"Generic HID"]) {
    [arguments addObjectsFromArray:@[ @"--output-profile", @"generic" ]];
  } else if ([profile isEqualToString:@"Standard Gamepad"]) {
    [arguments addObjectsFromArray:@[
      @"--output-profile", @"standard-gamepad"
    ]];
  } else if ([profile isEqualToString:@"Switch 1 Pro Controller"]) {
    [arguments addObjectsFromArray:@[ @"--output-profile", @"switch-pro" ]];
  }
  [self addArgument:@"--override-vendor-id"
          fromField:self.vendorIdField
          arguments:arguments];
  [self addArgument:@"--override-product-id"
          fromField:self.productIdField
          arguments:arguments];
  [self addArgument:@"--override-version"
          fromField:self.versionField
          arguments:arguments];
  [self addArgument:@"--override-product"
          fromField:self.productNameField
          arguments:arguments];
  [self addArgument:@"--override-manufacturer"
          fromField:self.manufacturerField
          arguments:arguments];
  [self addArgument:@"--override-serial"
          fromField:self.serialField
          arguments:arguments];

  NSString* transport = self.transportPopup.titleOfSelectedItem;
  if ([transport isEqualToString:@"Virtual"]) {
    [arguments addObjectsFromArray:@[ @"--override-transport", @"virtual" ]];
  } else if ([transport isEqualToString:@"USB"]) {
    [arguments addObjectsFromArray:@[ @"--override-transport", @"usb" ]];
  } else if ([transport isEqualToString:@"Bluetooth"]) {
    [arguments addObjectsFromArray:@[ @"--override-transport", @"bluetooth" ]];
  } else if ([transport isEqualToString:@"Bluetooth Low Energy"]) {
    [arguments addObjectsFromArray:@[ @"--override-transport", @"ble" ]];
  } else if ([transport isEqualToString:@"Network"]) {
    [arguments addObjectsFromArray:@[ @"--override-transport", @"network" ]];
  }

  self.task = [[NSTask alloc] init];
  self.task.executableURL = [NSURL fileURLWithPath:executable];
  self.task.arguments = arguments;
  NSPipe* pipe = [NSPipe pipe];
  self.task.standardOutput = pipe;
  self.task.standardError = pipe;

  __weak AppDelegate* weakSelf = self;
  pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle* handle) {
    NSData* data = handle.availableData;
    if (!data.length) return;
    NSString* text = [[NSString alloc] initWithData:data
                                           encoding:NSUTF8StringEncoding];
    if (!text.length) return;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf appendText:text];
    });
  };
  self.task.terminationHandler = ^(NSTask* finishedTask) {
    (void)finishedTask;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf bridgeExited];
    });
  };

  NSError* error = nil;
  if (![self.task launchAndReturnError:&error]) {
    [self appendLine:[NSString stringWithFormat:@"error: failed to launch: %@",
                                                error.localizedDescription]];
    self.task = nil;
    return;
  }

  [self.devices removeAllObjects];
  [self.tableView reloadData];
  self.statusLabel.stringValue =
      [NSString stringWithFormat:@"Running: %@ %@", executable,
                                 [arguments componentsJoinedByString:@" "]];
  self.startStopButton.title = @"Stop";
  [self appendLine:self.statusLabel.stringValue];
}

- (void)stopBridge {
  if (self.task && self.task.isRunning) {
    [self.task terminate];
  }
}

- (void)bridgeExited {
  self.statusLabel.stringValue = @"Stopped";
  self.startStopButton.title = @"Start";
  self.task = nil;
  [self appendLine:@"bridge process exited"];
}

- (void)appendText:(NSString*)text {
  NSTextStorage* storage = self.logView.textStorage;
  [storage appendAttributedString:[[NSAttributedString alloc] initWithString:text]];
  [self.logView scrollRangeToVisible:NSMakeRange(storage.length, 0)];

  [self.lineBuffer appendString:text];
  while (true) {
    NSRange newline = [self.lineBuffer rangeOfString:@"\n"];
    if (newline.location == NSNotFound) break;
    NSString* line = [self.lineBuffer substringToIndex:newline.location];
    [self.lineBuffer deleteCharactersInRange:NSMakeRange(0, newline.location + 1)];
    [self processLine:line];
  }
}

- (void)appendLine:(NSString*)line {
  [self appendText:[line stringByAppendingString:@"\n"]];
}

- (DeviceRow*)deviceForId:(NSString*)deviceId create:(BOOL)create {
  for (DeviceRow* device in self.devices) {
    if ([device.deviceId isEqualToString:deviceId]) return device;
  }
  if (!create) return nil;
  DeviceRow* device = [[DeviceRow alloc] init];
  device.deviceId = deviceId;
  device.status = @"new";
  device.product = @"";
  device.lastEvent = @"";
  device.inputCount = @"0";
  [self.devices addObject:device];
  return device;
}

- (NSArray<NSTextCheckingResult*>*)matches:(NSString*)pattern line:(NSString*)line {
  NSRegularExpression* regex =
      [NSRegularExpression regularExpressionWithPattern:pattern
                                                options:0
                                                  error:nil];
  return [regex matchesInString:line options:0 range:NSMakeRange(0, line.length)];
}

- (void)processLine:(NSString*)line {
  NSArray<NSTextCheckingResult*>* addMatches =
      [self matches:@"(?:raw HID )?device ([0-9]+) added: ([^(]+)"
                line:line];
  if (addMatches.count) {
    NSTextCheckingResult* match = addMatches.firstObject;
    NSString* deviceId = [line substringWithRange:[match rangeAtIndex:1]];
    NSString* product = [[line substringWithRange:[match rangeAtIndex:2]]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    DeviceRow* device = [self deviceForId:deviceId create:YES];
    device.status = @"active";
    device.product = product;
    device.lastEvent = @"added";
    [self.tableView reloadData];
    return;
  }

  NSArray<NSTextCheckingResult*>* removeMatches =
      [self matches:@"device ([0-9]+) removed" line:line];
  if (removeMatches.count) {
    NSTextCheckingResult* match = removeMatches.firstObject;
    NSString* deviceId = [line substringWithRange:[match rangeAtIndex:1]];
    DeviceRow* device = [self deviceForId:deviceId create:YES];
    device.status = @"removed";
    device.lastEvent = @"removed";
    [self.tableView reloadData];
    return;
  }

  NSArray<NSTextCheckingResult*>* reportMatches =
      [self matches:@"device ([0-9]+): .*report ([0-9]+)" line:line];
  if (reportMatches.count) {
    NSTextCheckingResult* match = reportMatches.firstObject;
    NSString* deviceId = [line substringWithRange:[match rangeAtIndex:1]];
    DeviceRow* device = [self deviceForId:deviceId create:YES];
    device.inputCount = [line substringWithRange:[match rangeAtIndex:2]];
    device.lastEvent = line;
    [self.tableView reloadData];
  }
}

@end

int main(int argc, const char* argv[]) {
  (void)argc;
  (void)argv;
  @autoreleasepool {
    NSApplication* app = [NSApplication sharedApplication];
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    AppDelegate* delegate = [[AppDelegate alloc] init];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
