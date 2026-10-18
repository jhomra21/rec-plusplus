// The macOS app: one AppKit window of standard controls, a menu bar item, and the Shift-
// Command-R global hotkey. Recording runs in rec::app::Session; this file only presents it.
// REC_APP_HOME keeps settings and recordings in one folder, REC_APP_TEST_PATTERN=1 records
// the synthetic flash/beep sources, and REC_APP_SELF_TEST=<report> drives the window's own
// controls through a recording session (see scripts/app-e2e.sh).
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Security/Security.h>

#include "app/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using rec::app::choice_index;
using rec::app::kCodecs;
using rec::app::kFormats;
using rec::app::kFrameRates;
using rec::app::kResolutions;
using rec::app::kServices;
using rec::app::Session;
using rec::app::Settings;
using rec::app::State;
using rec::app::Status;

NSString* ns(const std::string& text) { return [NSString stringWithUTF8String:text.c_str()] ?: @""; }
std::string utf8(NSString* text) { return text.UTF8String ? std::string(text.UTF8String) : std::string(); }

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

NSString* clock_text(double seconds) {
    const auto whole = static_cast<long long>(seconds);
    if (whole >= 3600) return [NSString stringWithFormat:@"%lld:%02lld:%02lld", whole / 3600, whole / 60 % 60, whole % 60];
    return [NSString stringWithFormat:@"%02lld:%02lld", whole / 60, whole % 60];
}

// Stream keys live in the login keychain, one generic password per server URL, never in the
// settings file.
NSMutableDictionary* key_query(const std::string& server) {
    return [@{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
              (__bridge id)kSecAttrService: @"rec stream key",
              (__bridge id)kSecAttrAccount: ns(server)} mutableCopy];
}

std::string load_key(const std::string& server) {
    if (server.empty()) return {};
    NSMutableDictionary* query = key_query(server);
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef found = nullptr;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &found) != errSecSuccess || !found) return {};
    NSData* data = CFBridgingRelease(found);
    return std::string(static_cast<const char*>(data.bytes), data.length);
}

OSStatus store_key(const std::string& server, const std::string& key) {
    if (server.empty() || key.empty() || load_key(server) == key) return errSecSuccess;
    NSData* data = [NSData dataWithBytes:key.data() length:key.size()];
    OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)key_query(server),
                                    (__bridge CFDictionaryRef)@{(__bridge id)kSecValueData: data});
    if (status == errSecItemNotFound) {
        NSMutableDictionary* item = key_query(server);
        item[(__bridge id)kSecValueData] = data;
        item[(__bridge id)kSecAttrLabel] = [NSString stringWithFormat:@"Rec stream key (%@)", ns(server)];
        status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
    }
    return status;
}

void delete_key(const std::string& server) { SecItemDelete((__bridge CFDictionaryRef)key_query(server)); }

NSTextField* label(NSString* text) {
    NSTextField* field = [NSTextField labelWithString:text];
    field.lineBreakMode = NSLineBreakByTruncatingTail;
    return field;
}

NSTextField* header(NSString* text) {
    NSTextField* field = label(text);
    field.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return field;
}

OSStatus on_hotkey(EventHandlerCallRef, EventRef, void* context);

// One entry of the Source menu: a display, the saved region, "Select a Region…", or a window.
struct Source {
    enum class Kind { display, region, pick, window } kind = Kind::display;
    std::uint32_t display = 1;
    std::string app, title;
};

NSString* window_label(const std::string& app, const std::string& title) {
    NSString* text = ns(title.empty() ? app : app.empty() ? title : app + " - " + title);
    if (text.length > 70) text = [[text substringToIndex:69] stringByAppendingString:@"…"];
    return [@"Window: " stringByAppendingString:text];
}
} // namespace

// A dimmed, borderless window over one screen. Dragging selects a rectangle; Esc, a right
// click, or leaving Rec cancels.
@interface RegionWindow : NSWindow
@end

@implementation RegionWindow
- (BOOL)canBecomeKeyWindow { return YES; }
@end

@interface RegionView : NSView
@property(nonatomic, copy) void (^done)(NSRect rect);  // An empty rect cancels.
@end

@implementation RegionView {
    NSPoint _start;
    NSRect _rect;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
- (void)resetCursorRects { [self addCursorRect:self.bounds cursor:NSCursor.crosshairCursor]; }

- (void)mouseDown:(NSEvent*)event {
    _start = [self convertPoint:event.locationInWindow fromView:nil];
    _rect = NSZeroRect;
    [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent*)event {
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    _rect = NSIntersectionRect(NSMakeRect(std::min(_start.x, p.x), std::min(_start.y, p.y), std::fabs(p.x - _start.x),
                                          std::fabs(p.y - _start.y)), self.bounds);
    [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent*)event {
    [self mouseDragged:event];
    if (_rect.size.width >= 8 && _rect.size.height >= 8) {
        if (self.done) self.done(_rect);
    } else {
        _rect = NSZeroRect;
        [self setNeedsDisplay:YES];
    }
}

- (void)rightMouseDown:(NSEvent*)event {
    if (self.done) self.done(NSZeroRect);
}

- (void)keyDown:(NSEvent*)event {
    if (event.keyCode == kVK_Escape) {
        if (self.done) self.done(NSZeroRect);
    } else {
        [super keyDown:event];
    }
}

- (void)drawRect:(NSRect)dirty {
    NSBezierPath* shade = [NSBezierPath bezierPathWithRect:self.bounds];
    const bool selected = !NSIsEmptyRect(_rect);
    if (selected) {
        [shade appendBezierPathWithRect:_rect];
        shade.windingRule = NSWindingRuleEvenOdd;
    }
    [[NSColor colorWithWhite:0 alpha:0.35] setFill];
    [shade fill];
    if (selected) {
        [NSColor.whiteColor setStroke];
        [[NSBezierPath bezierPathWithRect:NSInsetRect(_rect, 0.5, 0.5)] stroke];
        return;
    }
    NSDictionary* style = @{NSFontAttributeName: [NSFont systemFontOfSize:20 weight:NSFontWeightMedium],
                            NSForegroundColorAttributeName: NSColor.whiteColor};
    NSString* hint = @"Drag to select the part of the screen to record. Press Esc to cancel.";
    const NSSize size = [hint sizeWithAttributes:style];
    [hint drawAtPoint:NSMakePoint(NSMidX(self.bounds) - size.width / 2, NSMidY(self.bounds) - size.height / 2) withAttributes:style];
}
@end

@interface RecController : NSObject <NSApplicationDelegate, NSMenuDelegate, NSTextFieldDelegate>
- (void)toggleRecord:(id)sender;
- (void)refresh;
@end

@implementation RecController {
    std::unique_ptr<Session> _session;
    Settings _settings;
    std::string _settingsPath;
    std::vector<rec::Display> _displays;
    std::vector<rec::AudioDevice> _microphones;
    std::vector<rec::Window> _windows;
    std::vector<Source> _sources;
    NSArray<NSWindow*>* _pickers;  // The region picker while it is open.
    bool _testPattern;
    bool _hotkey;
    bool _quitting;
    bool _attention;
    bool _saved;
    State _state;
    float _systemShown, _micShown;
    EventHotKeyRef _hotkeyRef;
    NSWindow* _window;
    NSTextField *_status, *_detail, *_live, *_folder;
    NSLevelIndicator *_systemMeter, *_micMeter;
    NSButton *_record, *_pause, *_systemAudio, *_microphone, *_cursor, *_stream, *_changeFolder, *_openFolder, *_showLast;
    NSPopUpButton *_source, *_micDevice, *_resolution, *_fps, *_codec, *_format, *_service;
    NSTextField *_server, *_bitrate;
    NSSecureTextField* _key;
    NSStatusItem* _statusItem;
    NSTimer* _timer;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    const std::string home = environment("REC_APP_HOME");
    _testPattern = environment("REC_APP_TEST_PATTERN") == "1";
    NSString* support = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject
        stringByAppendingPathComponent:@"rec"];
    _settingsPath = (home.empty() ? utf8(support) : home) + "/settings.conf";
    _settings = rec::app::load_settings(_settingsPath);
    if (_settings.folder.empty()) {
        NSString* movies = NSSearchPathForDirectoriesInDomains(NSMoviesDirectory, NSUserDomainMask, YES).firstObject;
        _settings.folder = home.empty() ? utf8(movies) : home + "/recordings";
    }
    __weak RecController* weakSelf = self;
    _session = std::make_unique<Session>([weakSelf] {
        dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf refresh]; });
    });

    [self buildMenus];
    [self buildWindow];
    [self buildStatusItem];
    [self showSettings];
    [self registerHotkey];
    [self refresh];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self listDevices];

    const std::string report = environment("REC_APP_SELF_TEST");
    if (!report.empty()) {
        NSString* path = ns(report);
        [NSThread detachNewThreadWithBlock:^{ [weakSelf selfTest:path]; }];
    }
}

// Display and window names need Screen Recording permission. Without it, list the displays
// CoreGraphics reports, in the same main-first order, so launching never triggers the
// permission prompt.
- (void)listDevices {
    const bool listScreen = !_testPattern && CGPreflightScreenCaptureAccess();
    __weak RecController* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::string error;
        auto displays = listScreen ? rec::list_displays(error) : std::vector<rec::Display>{};
        auto windows = listScreen ? rec::list_windows(error) : std::vector<rec::Window>{};
        auto microphones = rec::list_microphones(error);
        dispatch_async(dispatch_get_main_queue(), ^{
            RecController* strongSelf = weakSelf;
            if (!strongSelf) return;
            [strongSelf showDisplays:displays windows:windows microphones:microphones];
        });
    });
}

- (void)showDisplays:(const std::vector<rec::Display>&)displays windows:(const std::vector<rec::Window>&)windows
         microphones:(const std::vector<rec::AudioDevice>&)microphones {
    _displays = displays;
    _windows = windows;
    _microphones = microphones;
    if (_displays.empty()) {
        CGDirectDisplayID ids[16];
        uint32_t count = 0;
        if (CGGetActiveDisplayList(16, ids, &count) == kCGErrorSuccess) {
            for (uint32_t i = 0; i < count; ++i) {
                CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i]);
                const auto width = mode ? static_cast<std::uint32_t>(CGDisplayModeGetPixelWidth(mode)) : 0u;
                const auto height = mode ? static_cast<std::uint32_t>(CGDisplayModeGetPixelHeight(mode)) : 0u;
                if (mode) CGDisplayModeRelease(mode);
                _displays.push_back({ids[i], width, height, "Display " + std::to_string(i + 1)});
            }
        }
    }
    [self showSources];

    [_micDevice removeAllItems];
    [_micDevice addItemWithTitle:@"Default microphone"];
    NSInteger mic = 0;
    for (std::size_t i = 0; i < _microphones.size(); ++i) {
        // Through the menu: addItemWithTitle: would drop a device whose name repeats.
        [_micDevice.menu addItem:[[NSMenuItem alloc] initWithTitle:ns(_microphones[i].name) action:nil keyEquivalent:@""]];
        if (_microphones[i].id == _settings.microphone_device) mic = static_cast<NSInteger>(i) + 1;
    }
    [_micDevice selectItemAtIndex:mic];
    [self refresh];
}

// Displays, the saved region, "Select a Region…", then open windows. Each item's tag is its
// index in _sources.
- (void)showSources {
    const auto& s = _settings;
    _sources.clear();
    NSMenu* menu = [[NSMenu alloc] init];
    const auto add = [&](Source source, NSString* title) {
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
        item.tag = static_cast<NSInteger>(_sources.size());
        _sources.push_back(std::move(source));
        [menu addItem:item];
    };
    for (std::size_t i = 0; i < std::max<std::size_t>(1, _displays.size()); ++i) {
        const auto* d = i < _displays.size() ? &_displays[i] : nullptr;
        NSString* name = d && d->width ? [NSString stringWithFormat:@"Display %zu (%ux%u)%@", i + 1, d->width, d->height, i == 0 ? @", main" : @""]
                                       : [NSString stringWithFormat:@"Display %zu", i + 1];
        add({Source::Kind::display, static_cast<std::uint32_t>(i + 1)}, name);
    }
    [menu addItem:NSMenuItem.separatorItem];
    if (const auto r = rec::app::parse_region(s.region))
        add({Source::Kind::region, s.display},
            [NSString stringWithFormat:@"Region %ux%u at %d,%d on Display %u", r->width, r->height, r->x, r->y, s.display]);
    add({Source::Kind::pick}, @"Select a Region…");
    const bool open = std::any_of(_windows.begin(), _windows.end(), [&](const rec::Window& w) {
        return w.app == s.window_app && w.title == s.window_title;
    });
    const bool missing = s.source == "window" && !open && !(s.window_app.empty() && s.window_title.empty());
    if (missing || !_windows.empty()) [menu addItem:NSMenuItem.separatorItem];
    if (missing)
        add({Source::Kind::window, 0, s.window_app, s.window_title},
            [window_label(s.window_app, s.window_title) stringByAppendingString:@" (not open)"]);
    for (const auto& w : _windows) add({Source::Kind::window, 0, w.app, w.title}, window_label(w.app, w.title));
    _source.menu = menu;

    NSInteger chosen = 0;
    for (std::size_t i = 0; i < _sources.size(); ++i) {
        const auto& source = _sources[i];
        const bool match = s.source == "window"
                               ? source.kind == Source::Kind::window && source.app == s.window_app && source.title == s.window_title
                           : s.source == "region" ? source.kind == Source::Kind::region
                                                  : source.kind == Source::Kind::display && source.display == s.display;
        if (match) { chosen = static_cast<NSInteger>(i); break; }
    }
    [_source selectItemWithTag:chosen];
}

// Hides Rec, lets the person drag a rectangle on any screen, and records that part of the
// display under it.
- (void)chooseRegion {
    if (_pickers) return;
    [_window orderOut:nil];
    [NSApp activateIgnoringOtherApps:YES];
    NSMutableArray<NSWindow*>* pickers = [NSMutableArray array];
    const NSPoint mouse = NSEvent.mouseLocation;
    __weak RecController* weakSelf = self;
    for (NSScreen* screen in NSScreen.screens) {
        RegionWindow* picker = [[RegionWindow alloc] initWithContentRect:screen.frame styleMask:NSWindowStyleMaskBorderless
                                                                 backing:NSBackingStoreBuffered defer:NO];
        picker.releasedWhenClosed = NO;
        picker.opaque = NO;
        picker.hasShadow = NO;
        picker.backgroundColor = NSColor.clearColor;
        picker.level = NSScreenSaverWindowLevel;
        picker.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
        [picker setFrame:screen.frame display:NO];
        RegionView* view = [[RegionView alloc] initWithFrame:NSMakeRect(0, 0, screen.frame.size.width, screen.frame.size.height)];
        view.done = ^(NSRect rect) { [weakSelf pickedRegion:rect screen:screen]; };
        picker.contentView = view;
        [pickers addObject:picker];
        if (NSPointInRect(mouse, screen.frame)) {
            [picker makeKeyAndOrderFront:nil];
            [picker makeFirstResponder:view];
        } else {
            [picker orderFront:nil];
        }
    }
    _pickers = pickers;
}

- (void)pickedRegion:(NSRect)rect screen:(NSScreen*)screen {
    if (!_pickers) return;
    for (NSWindow* picker in _pickers) [picker orderOut:nil];
    _pickers = nil;
    [self showWindow:nil];
    if (!NSIsEmptyRect(rect) && screen) {
        // Regions are in the display's pixels from its top-left corner; the view is in points
        // from the bottom-left.
        const auto screenID = static_cast<CGDirectDisplayID>([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]);
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(screenID);
        const double pixels = mode ? static_cast<double>(CGDisplayModeGetPixelWidth(mode)) : screen.frame.size.width * screen.backingScaleFactor;
        if (mode) CGDisplayModeRelease(mode);
        const double scale = pixels / screen.frame.size.width;
        std::uint32_t display = 1;
        for (std::size_t i = 0; i < _displays.size(); ++i)
            if (_displays[i].id == screenID) display = static_cast<std::uint32_t>(i + 1);
        const rec::Region region{static_cast<std::int32_t>(std::lround(rect.origin.x * scale)),
                                 static_cast<std::int32_t>(std::lround((screen.frame.size.height - NSMaxY(rect)) * scale)),
                                 static_cast<std::uint32_t>(std::lround(rect.size.width * scale)) & ~1u,
                                 static_cast<std::uint32_t>(std::lround(rect.size.height * scale)) & ~1u};
        if (region.width >= 16 && region.height >= 16) {
            _settings.source = "region";
            _settings.display = display;
            _settings.region = rec::app::region_text(region);
        }
    }
    [self showSources];
    [self remember];
    [self refresh];
}

- (void)applicationDidResignActive:(NSNotification*)notification {
    if (_pickers) [self pickedRegion:NSZeroRect screen:nil];
}

// Windows open and close while Rec is in the background; list them again when it returns.
- (void)applicationDidBecomeActive:(NSNotification*)notification {
    if (!_pickers && _session && _session->status().state == State::idle) [self listDevices];
}

- (void)registerHotkey {
    EventTypeSpec spec{kEventClassKeyboard, kEventHotKeyPressed};
    InstallApplicationEventHandler(&on_hotkey, 1, &spec, (__bridge void*)self, nullptr);
    const EventHotKeyID id{'recR', 1};
    _hotkey = RegisterEventHotKey(kVK_ANSI_R, cmdKey | shiftKey, id, GetApplicationEventTarget(), 0, &_hotkeyRef) == noErr;
}

// MARK: - Interface

- (NSMenuItem*)item:(NSString*)title action:(SEL)action key:(NSString*)key {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    item.target = self;
    return item;
}

- (void)buildMenus {
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenu* app = [[NSMenu alloc] initWithTitle:@"Rec"];
    [app addItemWithTitle:@"About Rec" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:@"Hide Rec" action:@selector(hide:) keyEquivalent:@"h"];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:@"Quit Rec" action:@selector(terminate:) keyEquivalent:@"q"];
    // Text fields need the Edit menu's key equivalents to paste a stream key.
    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    [edit addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
    [edit addItem:NSMenuItem.separatorItem];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    NSMenu* recording = [[NSMenu alloc] initWithTitle:@"Recording"];
    recording.delegate = self;
    NSMenu* window = [[NSMenu alloc] initWithTitle:@"Window"];
    [window addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [window addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    for (NSMenu* menu in @[app, edit, recording, window]) {
        NSMenuItem* top = [[NSMenuItem alloc] initWithTitle:menu.title action:nil keyEquivalent:@""];
        top.submenu = menu;
        [bar addItem:top];
    }
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = window;
}

// The Recording menu and the menu bar item's menu are rebuilt each time they open.
- (void)menuNeedsUpdate:(NSMenu*)menu {
    [menu removeAllItems];
    const auto state = _session->status().state;
    const bool active = state == State::recording || state == State::paused;
    NSMenuItem* record = [self item:active ? @"Stop Recording" : @"Start Recording" action:@selector(toggleRecord:) key:@"r"];
    record.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    record.enabled = state == State::idle || active;
    [menu addItem:record];
    NSMenuItem* pause = [self item:state == State::paused ? @"Resume" : @"Pause" action:@selector(togglePause:) key:@""];
    pause.enabled = active;
    [menu addItem:pause];
    if (menu == _statusItem.menu) {
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self item:@"Show Rec" action:@selector(showWindow:) key:@""]];
        [menu addItem:[self item:@"Open Recordings Folder" action:@selector(openFolder:) key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:@"Quit Rec" action:@selector(terminate:) keyEquivalent:@""];
        [menu addItem:quit];
    }
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(toggleRecord:)) {
        const auto state = _session->status().state;
        return state == State::idle || state == State::recording || state == State::paused;
    }
    if (item.action == @selector(togglePause:)) {
        const auto state = _session->status().state;
        return state == State::recording || state == State::paused;
    }
    return YES;
}

- (void)buildStatusItem {
    _statusItem = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    _statusItem.button.imagePosition = NSImageLeft;
    _statusItem.button.toolTip = @"Rec";
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Rec"];
    menu.delegate = self;
    menu.autoenablesItems = NO;
    _statusItem.menu = menu;
}

- (NSPopUpButton*)popup:(NSString*)name {
    NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    popup.target = self;
    popup.action = @selector(changed:);
    popup.accessibilityLabel = name;
    return popup;
}

- (NSButton*)check:(NSString*)title {
    return [NSButton checkboxWithTitle:title target:self action:@selector(changed:)];
}

- (NSButton*)button:(NSString*)title action:(SEL)action {
    NSButton* button = [NSButton buttonWithTitle:title target:self action:action];
    button.bezelStyle = NSBezelStyleRounded;
    return button;
}

- (NSTextField*)field:(NSString*)name secure:(BOOL)secure {
    NSTextField* field = secure ? [[NSSecureTextField alloc] init] : [[NSTextField alloc] init];
    field.accessibilityLabel = name;
    field.delegate = self;
    field.lineBreakMode = NSLineBreakByTruncatingTail;
    field.cell.scrollable = YES;
    return field;
}

// Two columns: a label (or a checkbox spanning both) and its control.
- (NSGridView*)grid:(NSArray<NSArray<NSView*>*>*)rows {
    NSMutableArray* cells = [NSMutableArray array];
    for (NSArray<NSView*>* row in rows)
        [cells addObject:row.count == 2 ? row : @[row[0], NSGridCell.emptyContentView]];
    NSGridView* grid = [NSGridView gridViewWithViews:cells];
    grid.rowSpacing = 8;
    grid.columnSpacing = 10;
    [grid columnAtIndex:0].width = 130;
    [grid columnAtIndex:1].xPlacement = NSGridCellPlacementFill;
    for (NSInteger i = 0; i < grid.numberOfRows; ++i) {
        [grid rowAtIndex:i].yPlacement = NSGridCellPlacementCenter;
        if (rows[static_cast<NSUInteger>(i)].count == 1) [[grid rowAtIndex:i] mergeCellsInRange:NSMakeRange(0, 2)];
    }
    return grid;
}

- (NSLevelIndicator*)meter:(NSString*)name {
    NSLevelIndicator* meter = [[NSLevelIndicator alloc] init];
    meter.levelIndicatorStyle = NSLevelIndicatorStyleContinuousCapacity;
    meter.minValue = 0;
    meter.maxValue = 1;
    meter.warningValue = 0.9;
    meter.criticalValue = 0.97;
    meter.accessibilityLabel = name;
    return meter;
}

- (void)buildWindow {
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 760)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"Rec";
    // Closing the window keeps Rec, and any recording, running in the menu bar.
    _window.releasedWhenClosed = NO;

    _status = label(@"Ready to record");
    _status.font = [NSFont systemFontOfSize:24 weight:NSFontWeightSemibold];
    _status.accessibilityLabel = @"Status";
    _detail = [NSTextField wrappingLabelWithString:@""];
    _detail.textColor = NSColor.secondaryLabelColor;
    _detail.maximumNumberOfLines = 2;
    _detail.preferredMaxLayoutWidth = 360;
    [_detail setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _live = label(@"");
    _live.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    _live.textColor = NSColor.systemPurpleColor;
    [_live setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* detailRow = [NSStackView stackViewWithViews:@[_detail, _live]];
    detailRow.distribution = NSStackViewDistributionFill;
    detailRow.alignment = NSLayoutAttributeFirstBaseline;

    _systemMeter = [self meter:@"System audio level"];
    _micMeter = [self meter:@"Microphone level"];
    NSGridView* meters = [self grid:@[@[label(@"System audio"), _systemMeter], @[label(@"Microphone"), _micMeter]]];

    _record = [self button:@"Start Recording" action:@selector(toggleRecord:)];
    _record.controlSize = NSControlSizeLarge;
    _record.keyEquivalent = @"\r";
    _pause = [self button:@"Pause" action:@selector(togglePause:)];
    _pause.controlSize = NSControlSizeLarge;
    [_pause.widthAnchor constraintEqualToConstant:120].active = YES;
    NSStackView* buttons = [NSStackView stackViewWithViews:@[_record, _pause]];
    buttons.distribution = NSStackViewDistributionFill;
    [_record setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    _source = [self popup:@"Source"];
    _systemAudio = [self check:@"System audio"];
    _microphone = [self check:@"Microphone"];
    _micDevice = [self popup:@"Microphone device"];
    _cursor = [self check:@"Show cursor"];
    NSGridView* capture = [self grid:@[@[label(@"Source"), _source], @[_systemAudio], @[_microphone, _micDevice], @[_cursor]]];

    _resolution = [self popup:@"Resolution"];
    for (const auto& choice : kResolutions) [_resolution addItemWithTitle:ns(choice.label)];
    _fps = [self popup:@"Frame rate"];
    for (auto rate : kFrameRates) [_fps addItemWithTitle:[NSString stringWithFormat:@"%u fps", rate]];
    _codec = [self popup:@"Codec"];
    for (const auto& choice : kCodecs) [_codec addItemWithTitle:ns(choice.label)];
    _format = [self popup:@"Format"];
    for (const auto& choice : kFormats) [_format addItemWithTitle:ns(choice.label)];
    NSGridView* quality = [self grid:@[@[label(@"Resolution"), _resolution], @[label(@"Frame rate"), _fps],
                                       @[label(@"Codec"), _codec], @[label(@"Format"), _format]]];

    _stream = [self check:@"Go live while recording"];
    _service = [self popup:@"Service"];
    for (const auto& choice : kServices) [_service addItemWithTitle:ns(choice.label)];
    _server = [self field:@"Server URL" secure:NO];
    _key = (NSSecureTextField*)[self field:@"Stream key" secure:YES];
    _bitrate = [self field:@"Bitrate (kb/s)" secure:NO];
    NSGridView* live = [self grid:@[@[_stream], @[label(@"Service"), _service], @[label(@"Server URL"), _server],
                                    @[label(@"Stream key"), _key], @[label(@"Bitrate (kb/s)"), _bitrate]]];

    _folder = label(@"");
    _folder.textColor = NSColor.secondaryLabelColor;
    _folder.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _folder.selectable = YES;
    _folder.accessibilityLabel = @"Recordings folder";
    _changeFolder = [self button:@"Change Folder…" action:@selector(chooseFolder:)];
    _openFolder = [self button:@"Open Folder" action:@selector(openFolder:)];
    _showLast = [self button:@"Show Last Recording" action:@selector(showLast:)];
    NSStackView* folderButtons = [NSStackView stackViewWithViews:@[_changeFolder, _openFolder, _showLast]];
    folderButtons.distribution = NSStackViewDistributionFillEqually;

    NSArray<NSView*>* views = @[_status, detailRow, meters, buttons,
                                header(@"Capture"), capture, header(@"Quality"), quality,
                                header(@"Live stream"), live, header(@"Recordings"), _folder, folderButtons];
    NSStackView* stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 8;
    stack.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
    for (NSView* view in @[buttons, capture, quality, live]) [stack setCustomSpacing:18 afterView:view];
    for (NSView* view in views) [view.widthAnchor constraintEqualToConstant:440].active = YES;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    NSView* content = _window.contentView;
    [content addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:content.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
    [_window setContentSize:stack.fittingSize];
}

- (void)showSettings {
    const auto& s = _settings;
    _systemAudio.state = s.system_audio ? NSControlStateValueOn : NSControlStateValueOff;
    _microphone.state = s.microphone ? NSControlStateValueOn : NSControlStateValueOff;
    _cursor.state = s.cursor ? NSControlStateValueOn : NSControlStateValueOff;
    [_resolution selectItemAtIndex:choice_index(kResolutions, s.resolution)];
    const auto rate = std::find(kFrameRates.begin(), kFrameRates.end(), s.fps);
    [_fps selectItemAtIndex:rate == kFrameRates.end() ? 4 : rate - kFrameRates.begin()];
    [_codec selectItemAtIndex:choice_index(kCodecs, s.codec)];
    [_format selectItemAtIndex:choice_index(kFormats, s.format)];
    _stream.state = s.stream ? NSControlStateValueOn : NSControlStateValueOff;
    [_service selectItemAtIndex:choice_index(kServices, s.destination)];
    _bitrate.stringValue = [NSString stringWithFormat:@"%u", s.stream_kbps];
    _folder.stringValue = ns(s.folder);
    _folder.toolTip = ns(s.folder);
    [self showServer];
    [self showDisplays:_displays windows:_windows microphones:_microphones];
}

// Presets show their fixed server; a custom server is typed in.
- (void)showServer {
    const bool custom = _settings.destination == "custom";
    _server.stringValue = ns(custom ? _settings.server : rec::app::server_url(_settings));
    _server.editable = custom;
    _server.selectable = YES;
    _key.stringValue = ns(load_key(rec::app::server_url(_settings)));
}

- (void)readSettings {
    auto& s = _settings;
    const NSInteger source = _source.selectedItem ? _source.selectedItem.tag : -1;
    if (source >= 0 && source < static_cast<NSInteger>(_sources.size())) {
        const auto& chosen = _sources[static_cast<std::size_t>(source)];
        if (chosen.kind == Source::Kind::display || chosen.kind == Source::Kind::region) {
            s.source = chosen.kind == Source::Kind::display ? "display" : "region";
            s.display = chosen.display;
        } else if (chosen.kind == Source::Kind::window) {
            s.source = "window";
            s.window_app = chosen.app;
            s.window_title = chosen.title;
        }
    }
    s.system_audio = _systemAudio.state == NSControlStateValueOn;
    s.microphone = _microphone.state == NSControlStateValueOn;
    const NSInteger mic = _micDevice.indexOfSelectedItem;
    s.microphone_device = mic > 0 && mic <= static_cast<NSInteger>(_microphones.size()) ? _microphones[mic - 1].id : "default";
    s.cursor = _cursor.state == NSControlStateValueOn;
    const auto at = [](NSPopUpButton* popup) { return static_cast<std::size_t>(std::max<NSInteger>(0, popup.indexOfSelectedItem)); };
    s.resolution = kResolutions[at(_resolution)].value;
    s.fps = kFrameRates[at(_fps)];
    s.codec = kCodecs[at(_codec)].value;
    s.format = kFormats[at(_format)].value;
    s.stream = _stream.state == NSControlStateValueOn;
    s.destination = kServices[at(_service)].value;
    if (s.destination == "custom") s.server = utf8(_server.stringValue);
    const int kbps = _bitrate.intValue;
    s.stream_kbps = static_cast<std::uint32_t>(std::clamp(kbps > 0 ? kbps : 6000, 500, 50000));
}

- (void)remember {
    [self readSettings];
    rec::app::save_settings(_settingsPath, _settings);
}

// MARK: - Actions

- (void)changed:(id)sender {
    if (sender == _source) {
        const NSInteger at = _source.selectedItem ? _source.selectedItem.tag : -1;
        if (at >= 0 && at < static_cast<NSInteger>(_sources.size()) && _sources[static_cast<std::size_t>(at)].kind == Source::Kind::pick) {
            // After the menu closes, so the picker is not dismissed by the menu's own events.
            dispatch_async(dispatch_get_main_queue(), ^{ [self chooseRegion]; });
            return;
        }
        [self readSettings];
    }
    if (sender == _service) {
        [self readSettings];
        [self showServer];
    }
    [self refresh];
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    if (notification.object == _server && _settings.destination == "custom") {
        [self readSettings];
        _key.stringValue = ns(load_key(rec::app::server_url(_settings)));
    }
}

- (void)toggleRecord:(id)sender {
    const auto state = _session->status().state;
    if (state == State::recording || state == State::paused) {
        _session->stop();
    } else if (state == State::idle) {
        if (!_testPattern && !CGPreflightScreenCaptureAccess()) {
            [self askForScreenRecording];
            return;
        }
        [self remember];
        const std::string key = utf8(_key.stringValue);
        if (auto started = _session->start(_settings, key, _testPattern); !started) {
            [self warn:ns(started.error)];
            return;
        }
        if (_settings.stream) store_key(rec::app::server_url(_settings), key);
    }
    [self refresh];
}

- (void)togglePause:(id)sender {
    const auto state = _session->status().state;
    if (state == State::recording) _session->pause();
    else if (state == State::paused) _session->resume();
    [self refresh];
}

// macOS shows its own prompt once per app; after that only System Settings can grant access,
// and the change applies when Rec reopens.
- (void)askForScreenRecording {
    [self showWindow:nil];
    CGRequestScreenCaptureAccess();
    NSString* text = @"Turn on Rec in System Settings > Privacy & Security > Screen & System Audio Recording, "
                     @"then quit and reopen Rec.";
    _detail.stringValue = text;
    _detail.textColor = NSColor.systemOrangeColor;
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Rec needs Screen Recording permission";
    alert.informativeText = text;
    [alert addButtonWithTitle:@"Open System Settings"];
    [alert addButtonWithTitle:@"Cancel"];
    [alert beginSheetModalForWindow:_window completionHandler:^(NSModalResponse response) {
        if (response != NSAlertFirstButtonReturn) return;
        [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:
            @"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"]];
    }];
}

- (void)warn:(NSString*)message {
    _detail.stringValue = message;
    _detail.textColor = NSColor.systemOrangeColor;
    if (!environment("REC_APP_SELF_TEST").empty()) return;
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Rec could not start recording";
    alert.informativeText = message;
    if (_window.visible) [alert beginSheetModalForWindow:_window completionHandler:nil];
    else [alert runModal];
}

- (void)showWindow:(id)sender {
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)openFolder:(id)sender {
    NSURL* url = [NSURL fileURLWithPath:ns(_settings.folder) isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:nil];
    [NSWorkspace.sharedWorkspace openURL:url];
}

- (void)showLast:(id)sender {
    if (_settings.last_recording.empty()) return;
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:ns(_settings.last_recording)]]];
}

- (void)chooseFolder:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.canCreateDirectories = YES;
    panel.prompt = @"Choose";
    panel.message = @"Choose where Rec saves recordings";
    panel.directoryURL = [NSURL fileURLWithPath:ns(_settings.folder) isDirectory:YES];
    __weak RecController* weakSelf = self;
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse response) {
        RecController* strongSelf = weakSelf;
        if (!strongSelf || response != NSModalResponseOK || !panel.URL.path) return;
        strongSelf->_settings.folder = utf8(panel.URL.path);
        strongSelf->_folder.stringValue = panel.URL.path;
        strongSelf->_folder.toolTip = panel.URL.path;
        [strongSelf remember];
    }];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)visible {
    [self showWindow:nil];
    return YES;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return NO; }

// Quitting during a recording saves the movie first.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    if (_session->status().state == State::idle) {
        [self remember];
        _saved = true;
        return NSTerminateNow;
    }
    _quitting = true;
    _session->stop();
    [self refresh];
    return NSTerminateLater;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    if (!_saved) [self remember];
    if (_hotkeyRef) UnregisterEventHotKey(_hotkeyRef);
}

// MARK: - Status

- (void)tick:(NSTimer*)timer { [self refresh]; }

- (void)meter:(NSLevelIndicator*)meter peak:(float)peak shown:(float&)shown {
    // dB scale over the last 60 dB, falling back at about 20 dB a second.
    const float level = peak > 0 ? std::clamp((20.0f * std::log10(peak) + 60.0f) / 60.0f, 0.0f, 1.0f) : 0.0f;
    shown = std::max(level, shown - 0.017f);
    meter.doubleValue = shown;
}

- (void)refresh {
    if (!_session) return;
    const Status s = _session->status();
    const bool active = s.state == State::recording || s.state == State::paused;
    _state = s.state;
    if (!s.saved.empty() && s.saved != _settings.last_recording) {
        _settings.last_recording = s.saved;
        rec::app::save_settings(_settingsPath, _settings);
    }

    NSString* status = @"";
    NSString* detail = @"";
    _attention = false;
    switch (s.state) {
    case State::idle:
        status = @"Ready to record";
        if (!s.error.empty()) { detail = [@"Recording failed: " stringByAppendingString:ns(s.error)]; _attention = true; }
        else if (!s.stream_error.empty()) { detail = [@"Stream stopped early: " stringByAppendingString:ns(s.stream_error)]; _attention = true; }
        else if (!s.saved.empty()) detail = [@"Saved " stringByAppendingString:ns(s.saved).lastPathComponent];
        else detail = _hotkey ? @"⇧⌘R starts and stops recording from anywhere." : @"⇧⌘R is used by another app.";
        break;
    case State::starting: status = @"Starting…"; break;
    case State::recording: status = [@"● Recording  " stringByAppendingString:clock_text(s.seconds)]; break;
    case State::paused: status = [@"Paused  " stringByAppendingString:clock_text(s.seconds)]; break;
    case State::finishing: status = @"Saving…"; break;
    }
    if (active) {
        NSMutableString* text = [NSMutableString stringWithFormat:@"%llu frames", static_cast<unsigned long long>(s.frames)];
        if (s.dropped) { [text appendFormat:@", %llu dropped", static_cast<unsigned long long>(s.dropped)]; _attention = true; }
        if (s.audio_dropped) { [text appendFormat:@", audio drops %llu", static_cast<unsigned long long>(s.audio_dropped)]; _attention = true; }
        if (s.streaming && !s.stream_error.empty()) { [text appendFormat:@". Stream lost: %@", ns(s.stream_error)]; _attention = true; }
        else if (s.stream_dropped) { [text appendFormat:@", stream skipped %llu", static_cast<unsigned long long>(s.stream_dropped)]; _attention = true; }
        if (s.reconnects) [text appendFormat:@", stream reconnected %u %@", s.reconnects, s.reconnects == 1 ? @"time" : @"times"];
        detail = text;
    }
    if (![_status.stringValue isEqualToString:status]) _status.stringValue = status;
    if (![_detail.stringValue isEqualToString:detail]) _detail.stringValue = detail;
    _status.textColor = s.state == State::recording ? NSColor.systemRedColor
                      : s.state == State::paused    ? NSColor.systemOrangeColor
                                                    : NSColor.labelColor;
    _detail.textColor = _attention ? NSColor.systemOrangeColor : NSColor.secondaryLabelColor;
    NSString* live = !active ? @"" : s.live ? @"● LIVE" : s.reconnecting ? @"Reconnecting…" : @"";
    if (![_live.stringValue isEqualToString:live]) _live.stringValue = live;
    // Hide the last level once recording ends; polling stops with it.
    if (!active) _systemShown = _micShown = 0;
    [self meter:_systemMeter peak:s.system_peak shown:_systemShown];
    [self meter:_micMeter peak:s.mic_peak shown:_micShown];

    _record.title = active ? @"Stop Recording" : @"Start Recording";
    _pause.title = s.state == State::paused ? @"Resume" : @"Pause";
    _record.enabled = s.state == State::idle || active;
    _pause.enabled = active;
    const bool editable = s.state == State::idle;
    for (NSControl* control in @[_source, _systemAudio, _microphone, _cursor, _resolution, _fps, _codec, _format,
                                 _stream, _service, _server, _key, _bitrate, _changeFolder])
        control.enabled = editable;
    _micDevice.enabled = editable && _microphone.state == NSControlStateValueOn;
    _showLast.enabled = !_settings.last_recording.empty();

    const bool poll = s.state != State::idle;
    if (poll && !_timer) {
        _timer = [NSTimer timerWithTimeInterval:0.05 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
        [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
    } else if (!poll && _timer) {
        [_timer invalidate];
        _timer = nil;
    }

    NSStatusBarButton* button = _statusItem.button;
    if (@available(macOS 11.0, *)) {
        NSImage* image = [NSImage imageWithSystemSymbolName:active ? @"record.circle.fill" : @"record.circle"
                                   accessibilityDescription:active ? @"Rec, recording" : @"Rec"];
        [image setTemplate:!active];
        button.image = image;
        button.contentTintColor = active && s.state == State::recording ? NSColor.systemRedColor : nil;
        button.title = active ? [@" " stringByAppendingString:clock_text(s.seconds)] : @"";
    } else {
        button.title = active ? [@"● " stringByAppendingString:clock_text(s.seconds)] : @"○";
    }
    button.toolTip = active ? [NSString stringWithFormat:@"Rec: %@ %@%@", s.state == State::paused ? @"paused" : @"recording",
                                                         clock_text(s.seconds), s.live ? @", live" : @""]
                            : @"Rec";

    if (_quitting && s.state == State::idle) {
        _quitting = false;
        [self remember];
        _saved = true;
        [NSApp replyToApplicationShouldTerminate:YES];
    }
}

// MARK: - Self-test

// Runs on its own thread and touches the interface only through the main queue, as a user
// would: every step presses the window's real controls or the menu bar item's menu.
- (void)selfTest:(NSString*)reportPath {
    NSMutableString* report = [NSMutableString string];
    const auto note = [&](NSString* line) {
        [report appendFormat:@"%@\n", line];
        [report writeToFile:reportPath atomically:YES encoding:NSUTF8StringEncoding error:nil];
        std::fprintf(stderr, "self-test: %s\n", line.UTF8String);
    };
    const auto fail = [&](NSString* why) {
        note([@"failed=" stringByAppendingString:why]);
        std::_Exit(1);
    };
    const auto ui = [](void (^block)(void)) { dispatch_sync(dispatch_get_main_queue(), block); };
    const auto status = [&] {
        __block NSString* text;
        ui(^{ text = self->_status.stringValue; });
        return text;
    };
    const auto detail = [&] {
        __block NSString* text;
        ui(^{ text = self->_detail.stringValue; });
        return text;
    };
    const auto wait_for = [&](NSString* prefix, double seconds) {
        for (double waited = 0; waited < seconds; waited += 0.1) {
            if ([status() hasPrefix:prefix]) return;
            usleep(100000);
        }
        fail([NSString stringWithFormat:@"status is '%@' (%@), expected '%@'", status(), detail(), prefix]);
    };
    const auto press = [&](NSButton* button) { ui(^{ [button performClick:nil]; }); };
    const auto set_check = [&](NSButton* box, bool on) {
        ui(^{ if ((box.state == NSControlStateValueOn) != on) [box performClick:nil]; });
    };
    const auto saved = [&](NSString* step) {
        NSString* text = detail();
        if (![text hasPrefix:@"Saved "]) fail([NSString stringWithFormat:@"%@: %@", step, text]);
        __block NSString* path;
        ui(^{ path = ns(self->_settings.last_recording); });
        note([NSString stringWithFormat:@"%@=%@", step, path]);
    };
    NSString* const recording = @"● Recording";

    // Every control has an accessible name.
    __block NSString* unnamed = nil;
    ui(^{
        for (NSControl* control in @[self->_record, self->_pause, self->_source, self->_systemAudio, self->_microphone,
                                     self->_micDevice, self->_cursor, self->_resolution, self->_fps, self->_codec,
                                     self->_format, self->_stream, self->_service, self->_server, self->_key,
                                     self->_bitrate, self->_changeFolder, self->_openFolder, self->_showLast,
                                     self->_systemMeter, self->_micMeter]) {
            NSString* name = control.accessibilityLabel.length ? control.accessibilityLabel : control.accessibilityTitle;
            // Buttons are named by their title.
            if (!name.length && [control isKindOfClass:NSButton.class]) name = ((NSButton*)control).title;
            if (!name.length) unnamed = control.className;
        }
    });
    if (unnamed) fail([@"a control has no accessible name: " stringByAppendingString:unnamed]);
    note(@"accessibility=ok");
    wait_for(@"Ready to record", 20);
    // 30 fps keeps the check light on virtual machines.
    ui(^{ [self->_fps selectItemAtIndex:std::find(kFrameRates.begin(), kFrameRates.end(), 30u) - kFrameRates.begin()]; });

    // 1. Record, pause, resume, stop.
    set_check(_systemAudio, true);
    set_check(_microphone, true);
    press(_record);
    wait_for(recording, 20);
    sleep(2);
    press(_pause);
    wait_for(@"Paused", 5);
    usleep(1500000);
    press(_pause);
    wait_for(recording, 5);
    sleep(2);
    press(_record);
    wait_for(@"Ready to record", 30);
    saved(@"pause");

    // 2. The global hotkey, delivered as the system delivers it to the registered handler.
    if (!_hotkey) fail(@"Shift-Command-R could not be registered");
    const auto hotkey = [&] {
        ui(^{
            EventRef event = nullptr;
            CreateEvent(nullptr, kEventClassKeyboard, kEventHotKeyPressed, 0, kEventAttributeNone, &event);
            const EventHotKeyID id{'recR', 1};
            SetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, sizeof id, &id);
            SendEventToEventTarget(event, GetApplicationEventTarget());
            ReleaseEvent(event);
        });
    };
    hotkey();
    wait_for(recording, 20);
    sleep(2);
    hotkey();
    wait_for(@"Ready to record", 30);
    saved(@"hotkey");

    // 3. Go live to a local server while recording.
    const std::string server = environment("REC_APP_SELF_TEST_STREAM");
    if (!server.empty()) {
        set_check(_microphone, false);
        set_check(_stream, true);
        ui(^{
            [self->_service selectItemAtIndex:static_cast<NSInteger>(kServices.size()) - 1];
            [self->_service sendAction:self->_service.action to:self->_service.target];
            self->_server.stringValue = ns(server);
            self->_key.stringValue = @"appkey";
            self->_bitrate.stringValue = @"3000";
        });
        press(_record);
        wait_for(recording, 20);
        bool live = false;
        for (int i = 0; i < 100 && !live; ++i) {
            __block BOOL on = NO;
            ui(^{ on = [self->_live.stringValue isEqualToString:@"● LIVE"]; });
            live = on;
            if (!live) usleep(100000);
        }
        if (!live) fail([@"no LIVE badge: " stringByAppendingString:detail()]);
        sleep(4);
        press(_record);
        wait_for(@"Ready to record", 30);
        if ([detail() hasPrefix:@"Stream stopped early"]) fail(detail());
        saved(@"stream");
        const std::string stored = load_key(server);
        delete_key(server);
        if (stored != "appkey") fail(@"the stream key was not stored in the keychain");
        note(@"keychain=ok");
        set_check(_stream, false);
    }

    // 4. Closing the window keeps recording; the menu bar item's menu stops it.
    press(_record);
    wait_for(recording, 20);
    ui(^{ [self->_window performClose:nil]; });
    sleep(2);
    __block BOOL visible = YES;
    ui(^{ visible = self->_window.visible; });
    if (visible) fail(@"the window stayed open");
    wait_for(recording, 1);
    ui(^{
        NSMenu* menu = self->_statusItem.menu;
        [self menuNeedsUpdate:menu];
        [menu performActionForItemAtIndex:0];
    });
    wait_for(@"Ready to record", 30);
    saved(@"hidden");

    // 5. Quitting during a recording saves the movie, then the app exits.
    ui(^{ [self showWindow:nil]; });
    press(_record);
    wait_for(recording, 20);
    sleep(2);
    note(@"quit=started");
    dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
}
@end

namespace {
OSStatus on_hotkey(EventHandlerCallRef, EventRef, void* context) {
    [(__bridge RecController*)context toggleRecord:nil];
    return noErr;
}
} // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        RecController* controller = [[RecController alloc] init];
        app.delegate = controller;
        [app run];
    }
    return 0;
}
