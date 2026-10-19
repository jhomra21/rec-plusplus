// The macOS app: one AppKit window of standard controls, a menu bar item, and the Shift-
// Command-R global hotkey. Recording runs in rec::app::Session; this file only presents it.
// REC_APP_HOME keeps settings, recordings and stream keys apart, REC_APP_TEST_PATTERN=1 records
// the synthetic flash/beep sources, and REC_APP_SELF_TEST=<report> drives the window's own
// controls through a recording session (see scripts/app-e2e.sh).
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>
#import <Security/Security.h>

#include "app/session.hpp"
#include "cli/engine.hpp"
#include "core/shape.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using rec::app::choice_index;
using rec::app::kCameraSizes;
using rec::app::kCodecs;
using rec::app::kCorners;
using rec::app::kFormats;
using rec::app::kFrameRates;
using rec::app::kResolutions;
using rec::app::kMaxDestinations;
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
// settings file. A separate home gets its own service: a freshly built test copy reading the
// user's real keys would wait on a keychain prompt.
NSMutableDictionary* key_query(const std::string& server) {
    static NSString* const service = environment("REC_APP_HOME").empty() ? @"rec stream key" : @"rec stream key (test home)";
    return [@{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
              (__bridge id)kSecAttrService: service,
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

// The preview passes the pointer to its delegate, which moves and resizes the camera in it.
enum class Pointer { down, dragged, up, moved, exited };
@protocol PreviewDelegate
- (void)preview:(NSView*)view pointer:(Pointer)pointer event:(NSEvent*)event;
@end

@interface PreviewView : NSView
@property(nonatomic, weak) id<PreviewDelegate> delegate;
@end

@implementation PreviewView
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* area in self.trackingAreas) [self removeTrackingArea:area];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                       options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                               NSTrackingActiveInActiveApp | NSTrackingInVisibleRect
                                                         owner:self
                                                      userInfo:nil]];
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (void)mouseDown:(NSEvent*)event { [self.delegate preview:self pointer:Pointer::down event:event]; }
- (void)mouseDragged:(NSEvent*)event { [self.delegate preview:self pointer:Pointer::dragged event:event]; }
- (void)mouseUp:(NSEvent*)event { [self.delegate preview:self pointer:Pointer::up event:event]; }
- (void)mouseMoved:(NSEvent*)event { [self.delegate preview:self pointer:Pointer::moved event:event]; }
- (void)mouseExited:(NSEvent*)event { [self.delegate preview:self pointer:Pointer::exited event:event]; }
@end

// The source a press in the preview takes hold of, and how.
struct PreviewHit {
    rec::app::SourceRef source;
    rec::app::AreaDrag drag;
};

@interface RecController
    : NSObject <NSApplicationDelegate, NSMenuDelegate, NSTextFieldDelegate, NSTableViewDataSource, NSTableViewDelegate, PreviewDelegate>
- (void)toggleRecord:(id)sender;
- (void)saveReplay:(id)sender;
- (void)refresh;
@end

@implementation RecController {
    std::unique_ptr<Session> _session;
    Settings _settings;
    std::string _settingsPath;
    std::vector<rec::Display> _displays;
    std::vector<rec::AudioDevice> _microphones;
    std::vector<rec::Camera> _connected;  // Cameras found when Rec opened.
    std::vector<rec::Camera> _cameras;    // The Camera list: "default" first.
    std::vector<rec::Window> _windows;
    std::vector<Source> _sources;
    NSArray<NSWindow*>* _pickers;  // The region picker while it is open.
    bool _testPattern;
    bool _hotkey, _replayHotkey;
    NSString* _note;  // A message that replaces the detail line for a few seconds.
    NSDate* _noteUntil;
    bool _noteAttention;
    bool _quitting;
    bool _attention;
    bool _saved;
    State _state;
    float _systemShown, _micShown;
    EventHotKeyRef _hotkeyRef, _replayHotkeyRef;
    NSWindow* _window;
    NSTextField *_status, *_detail, *_live, *_folder;
    NSLevelIndicator *_systemMeter, *_micMeter;
    NSSlider *_systemVolume, *_micVolume;
    NSTextField *_systemVolumeText, *_micVolumeText;
    NSButton *_systemMute, *_micMute;
    NSButton *_record, *_pause, *_saveReplay, *_replay, *_systemAudio, *_microphone, *_cursor, *_clicks, *_stream, *_changeFolder, *_openFolder, *_showLast;
    NSButton *_camera, *_image, *_chooseImage, *_showPreview;
    NSButton *_newScene, *_deleteScene, *_sourceShown, *_addDisplay, *_addWindow, *_removeSource;
    NSButton* _verticalMovie;
    // Each destination's row: on, service, Vertical and Remove, then its server and key.
    NSStackView* _destRow[kMaxDestinations];
    NSButton *_destOn[kMaxDestinations], *_destVertical[kMaxDestinations], *_destRemove[kMaxDestinations];
    NSPopUpButton* _destService[kMaxDestinations];
    NSTextField* _destServer[kMaxDestinations];
    NSSecureTextField* _destKey[kMaxDestinations];
    NSButton* _addDestination;
    NSPopUpButton* _verticalLayout;
    NSTextField* _sceneName;
    NSTableView* _sourcesTable;
    rec::app::SourceRef _selected;   // The source the Sources list selects.
    bool _filling;                   // The app, not the person, is setting the Sources list.
    std::vector<rec::Window> _menuWindows;  // What the Add Window menu offered.
    NSPopUpButton *_source, *_micDevice, *_resolution, *_fps, *_codec, *_format, *_replayLength;
    NSPopUpButton *_cameraDevice, *_corner, *_cameraSize, *_scene;
    NSPopUpButton *_cameraShape, *_cameraBorder;
    NSSlider* _roundness;
    NSButton* _shadow;
    NSTextField *_bitrate, *_imageFile;
    NSStatusItem* _statusItem;
    NSTimer* _timer;
    PreviewView* _preview;
    NSTextField* _previewHint;
    CALayer* _outline;               // Around the source the pointer holds or is over.
    rec::app::AreaDrag _drag;
    rec::app::SourceRef _dragged;    // The source _drag holds.
    rec::Area _outlineArea;          // Where it is going.
    std::optional<rec::app::SourceRef> _hoverSource;
    State _shownState;
    rec::PreviewFrame _frame;
    NSView* _preview2;               // The vertical video's, beside the other.
    rec::PreviewFrame _frame2;
    bool _verticalShown;
    NSTimer* _previewTimer;
    bool _previewing;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    const std::string home = environment("REC_APP_HOME");
    _testPattern = environment("REC_APP_TEST_PATTERN") == "1";
    NSString* support = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject
        stringByAppendingPathComponent:@"rec"];
    _settingsPath = (home.empty() ? utf8(support) : home) + "/settings.conf";
    // A separate home (the app E2E) has its own engine; the engine started from here inherits it.
    if (!home.empty() && environment("REC_ENGINE_SOCKET").empty()) setenv("REC_ENGINE_SOCKET", (home + "/engine.sock").c_str(), 1);
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
    // The preview runs only while the window is on screen.
    for (NSNotificationName name in @[NSWindowDidChangeOcclusionStateNotification, NSWindowDidMiniaturizeNotification,
                                      NSWindowDidDeminiaturizeNotification])
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(windowChanged:) name:name object:_window];
    [self buildStatusItem];
    [self showSettings];
    [self registerHotkey];
    // A recording left running when Rec last quit (or started with `rec start`) shows here.
    std::string engineError;
    if (!_session->attach(_settings, engineError) && !engineError.empty()) {
        _note = [@"Could not reach the background engine: " stringByAppendingString:ns(engineError)];
        _noteAttention = true;
        _noteUntil = [NSDate dateWithTimeIntervalSinceNow:8];
    }
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
        auto cameras = rec::list_cameras(error);
        dispatch_async(dispatch_get_main_queue(), ^{
            RecController* strongSelf = weakSelf;
            if (!strongSelf) return;
            [strongSelf showCameras:cameras];
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

// The default camera, the connected ones, the test camera for self-tests, and the saved
// camera if it is not connected.
- (void)showCameras:(const std::vector<rec::Camera>&)cameras {
    _connected = cameras;
    _cameras = {{"default", "Default camera"}};
    _cameras.insert(_cameras.end(), _connected.begin(), _connected.end());
    if (_testPattern) _cameras.push_back({"test", "Test camera"});
    const auto& saved = _settings.camera_device;
    if (std::none_of(_cameras.begin(), _cameras.end(), [&](const rec::Camera& c) { return c.id == saved; }))
        _cameras.push_back({saved, saved + " (not connected)"});
    [_cameraDevice removeAllItems];
    NSInteger chosen = 0;
    for (std::size_t i = 0; i < _cameras.size(); ++i) {
        [_cameraDevice.menu addItem:[[NSMenuItem alloc] initWithTitle:ns(_cameras[i].name) action:nil keyEquivalent:@""]];
        if (_cameras[i].id == saved) chosen = static_cast<NSInteger>(i);
    }
    [_cameraDevice selectItemAtIndex:chosen];
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
    const EventHotKeyID id{'recR', 1}, replay{'recR', 2};
    _hotkey = RegisterEventHotKey(kVK_ANSI_R, cmdKey | shiftKey, id, GetApplicationEventTarget(), 0, &_hotkeyRef) == noErr;
    _replayHotkey = RegisterEventHotKey(kVK_ANSI_R, cmdKey | shiftKey | optionKey, replay, GetApplicationEventTarget(), 0,
                                        &_replayHotkeyRef) == noErr;
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
    NSMenuItem* replay = [self item:@"Save Replay" action:@selector(saveReplay:) key:@"r"];
    replay.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift | NSEventModifierFlagOption;
    replay.enabled = active && _session->status().replay;
    [menu addItem:replay];
    if (_settings.scenes.size() > 1) {
        [menu addItem:NSMenuItem.separatorItem];
        for (std::size_t i = 0; i < _settings.scenes.size(); ++i) {
            NSMenuItem* scene = [self item:ns(_settings.scenes[i].name) action:@selector(chooseScene:) key:@""];
            scene.tag = static_cast<NSInteger>(i);
            scene.state = _settings.scene == i ? NSControlStateValueOn : NSControlStateValueOff;
            [menu addItem:scene];
        }
    }
    if (const auto status = _session->status(); active && (status.system_audio || status.microphone)) {
        [menu addItem:NSMenuItem.separatorItem];
        if (status.system_audio) {
            NSMenuItem* mute = [self item:@"Mute System Audio" action:@selector(toggleMute:) key:@""];
            mute.tag = 0;
            mute.state = _systemMute.state;
            [menu addItem:mute];
        }
        if (status.microphone) {
            NSMenuItem* mute = [self item:@"Mute Microphone" action:@selector(toggleMute:) key:@""];
            mute.tag = 1;
            mute.state = _micMute.state;
            [menu addItem:mute];
        }
    }
    if (menu == _statusItem.menu) {
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self item:@"Show Rec" action:@selector(showWindow:) key:@""]];
        [menu addItem:[self item:@"Open Recordings Folder" action:@selector(openFolder:) key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        NSMenuItem* keep = [self item:@"Keep Recording After Quit" action:@selector(toggleKeepRecording:) key:@""];
        keep.state = _settings.keep_recording == "yes" ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:keep];
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
    // A grid taller than its rows spreads them apart.
    [grid setContentHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationVertical];
    [grid columnAtIndex:0].width = 130;
    [grid columnAtIndex:1].xPlacement = NSGridCellPlacementFill;
    for (NSInteger i = 0; i < grid.numberOfRows; ++i) {
        [grid rowAtIndex:i].yPlacement = NSGridCellPlacementCenter;
        if (rows[static_cast<NSUInteger>(i)].count == 1) {
            [[grid rowAtIndex:i] mergeCellsInRange:NSMakeRange(0, 2)];
            // A merged cell takes the first column's placement, which would leave a row of
            // controls at its narrowest.
            [grid cellAtColumnIndex:0 rowIndex:i].xPlacement = NSGridCellPlacementFill;
        }
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

- (NSSlider*)volumeSlider:(NSString*)name {
    NSSlider* slider = [NSSlider sliderWithValue:0 minValue:rec::app::kVolumeMin maxValue:rec::app::kVolumeMax
                                          target:self action:@selector(volumeChanged:)];
    slider.continuous = YES;
    slider.accessibilityLabel = name;
    return slider;
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
    _detail.preferredMaxLayoutWidth = 440;
    [_detail setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _live = label(@"");
    _live.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    _live.textColor = NSColor.systemPurpleColor;

    // Each source: its level before volume, the volume, and mute.
    _systemMeter = [self meter:@"System audio level"];
    _micMeter = [self meter:@"Microphone level"];
    _systemVolume = [self volumeSlider:@"System audio volume"];
    _micVolume = [self volumeSlider:@"Microphone volume"];
    _systemVolumeText = label(@"0 dB");
    _micVolumeText = label(@"0 dB");
    _systemMute = [self check:@"Mute system"];
    _micMute = [self check:@"Mute mic"];
    // A source's name and meter, then its volume, value and mute below.
    const auto volumeRow = ^NSStackView*(NSSlider* slider, NSTextField* value, NSButton* mute) {
        [value.widthAnchor constraintEqualToConstant:50].active = YES;
        [mute.widthAnchor constraintEqualToConstant:110].active = YES;
        NSStackView* row = [NSStackView stackViewWithViews:@[slider, value, mute]];
        row.distribution = NSStackViewDistributionFill;
        row.spacing = 10;
        [slider setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
        [slider.widthAnchor constraintGreaterThanOrEqualToConstant:120].active = YES;
        return row;
    };
    NSGridView* meters = [self grid:@[@[label(@"System audio"), _systemMeter], @[volumeRow(_systemVolume, _systemVolumeText, _systemMute)],
                                      @[label(@"Microphone"), _micMeter], @[volumeRow(_micVolume, _micVolumeText, _micMute)]]];

    // The newest frame as encoded, in the output's shape on dark gray.
    _preview = [[PreviewView alloc] init];
    _preview.delegate = self;
    _preview.wantsLayer = YES;
    _preview.layer.backgroundColor = [NSColor colorWithWhite:0.11 alpha:1].CGColor;
    _preview.layer.contentsGravity = kCAGravityResizeAspect;
    _outline = [CALayer layer];
    _outline.borderColor = NSColor.whiteColor.CGColor;
    _outline.borderWidth = 2;
    _outline.hidden = YES;
    [_preview.layer addSublayer:_outline];
    _preview.accessibilityElement = YES;
    _preview.accessibilityRole = NSAccessibilityImageRole;
    _preview.accessibilityLabel = @"Preview";
    _previewHint = label(@"Starting the preview…");
    _previewHint.textColor = [NSColor colorWithWhite:0.65 alpha:1];
    _previewHint.translatesAutoresizingMaskIntoConstraints = NO;
    [_preview addSubview:_previewHint];
    [NSLayoutConstraint activateConstraints:@[
        [_previewHint.centerXAnchor constraintEqualToAnchor:_preview.centerXAnchor],
        [_previewHint.centerYAnchor constraintEqualToAnchor:_preview.centerYAnchor],
        [_preview.heightAnchor constraintEqualToConstant:440 * 9.0 / 16],
    ]];
    _preview2 = [[NSView alloc] init];
    _preview2.wantsLayer = YES;
    _preview2.layer.backgroundColor = [NSColor colorWithWhite:0.11 alpha:1].CGColor;
    _preview2.layer.contentsGravity = kCAGravityResizeAspect;
    _preview2.accessibilityElement = YES;
    _preview2.accessibilityRole = NSAccessibilityImageRole;
    _preview2.accessibilityLabel = @"Vertical video preview";
    _preview2.hidden = YES;
    [NSLayoutConstraint activateConstraints:@[
        [_preview2.heightAnchor constraintEqualToConstant:440 * 9.0 / 16],
        [_preview2.widthAnchor constraintEqualToConstant:440 * 9.0 / 16 * 9 / 16],
    ]];

    _record = [self button:@"Start Recording" action:@selector(toggleRecord:)];
    _record.controlSize = NSControlSizeLarge;
    _record.keyEquivalent = @"\r";
    _pause = [self button:@"Pause" action:@selector(togglePause:)];
    _pause.controlSize = NSControlSizeLarge;
    [_pause.widthAnchor constraintEqualToConstant:110].active = YES;
    _saveReplay = [self button:@"Save Replay" action:@selector(saveReplay:)];
    _saveReplay.controlSize = NSControlSizeLarge;
    [_saveReplay.widthAnchor constraintEqualToConstant:110].active = YES;
    NSStackView* buttons = [NSStackView stackViewWithViews:@[_record, _pause, _saveReplay]];
    buttons.distribution = NSStackViewDistributionFill;
    [_record setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    _source = [self popup:@"Source"];
    _systemAudio = [self check:@"System audio"];
    _microphone = [self check:@"Microphone"];
    _micDevice = [self popup:@"Microphone device"];
    _cursor = [self check:@"Show cursor"];
    _clicks = [self check:@"Show clicks"];
    NSGridView* capture = [self grid:@[@[label(@"Source"), _source], @[_systemAudio], @[_microphone, _micDevice], @[_cursor, _clicks]]];

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
    _bitrate = [self field:@"Bitrate (kb/s)" secure:NO];
    NSMutableArray<NSView*>* liveViews = [NSMutableArray arrayWithObject:[self grid:@[@[_stream], @[label(@"Bitrate (kb/s)"), _bitrate]]]];
    // Each destination gets the landscape video or, ticked, the vertical one.
    for (std::size_t i = 0; i < kMaxDestinations; ++i) {
        _destOn[i] = [self check:@""];
        _destService[i] = [self popup:@""];
        for (const auto& choice : kServices) [_destService[i] addItemWithTitle:ns(choice.label)];
        _destVertical[i] = [self check:@"Vertical"];
        _destRemove[i] = [self button:@"Remove" action:@selector(removeDestination:)];
        _destRemove[i].tag = static_cast<NSInteger>(i);
        _destServer[i] = [self field:@"" secure:NO];
        _destKey[i] = (NSSecureTextField*)[self field:@"" secure:YES];
        _destKey[i].placeholderString = @"Stream key";
        NSStackView* top = [NSStackView stackViewWithViews:@[_destOn[i], _destService[i], _destVertical[i], _destRemove[i]]];
        top.distribution = NSStackViewDistributionFill;
        for (NSView* view in @[_destOn[i], _destVertical[i], _destRemove[i]])
            [view setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
        NSStackView* bottom = [NSStackView stackViewWithViews:@[_destServer[i], _destKey[i]]];
        bottom.distribution = NSStackViewDistributionFillEqually;
        bottom.edgeInsets = NSEdgeInsetsMake(0, 22, 0, 0);
        _destRow[i] = [NSStackView stackViewWithViews:@[top, bottom]];
        _destRow[i].orientation = NSUserInterfaceLayoutOrientationVertical;
        _destRow[i].spacing = 6;
        for (NSView* view in @[top, bottom]) [view.widthAnchor constraintEqualToAnchor:_destRow[i].widthAnchor].active = YES;
        [liveViews addObject:_destRow[i]];
    }
    _addDestination = [self button:@"Add Destination" action:@selector(addDestination:)];
    [liveViews addObject:_addDestination];
    NSStackView* live = [NSStackView stackViewWithViews:liveViews];
    live.orientation = NSUserInterfaceLayoutOrientationVertical;
    live.alignment = NSLayoutAttributeLeading;
    live.spacing = 12;
    for (NSView* view in liveViews)
        if (view != _addDestination) [view.widthAnchor constraintEqualToAnchor:live.widthAnchor].active = YES;

    _replay = [self check:@"Keep the last"];
    _replayLength = [self popup:@"Replay length"];
    for (auto seconds : rec::app::kReplayLengths)
        [_replayLength addItemWithTitle:seconds < 60 ? [NSString stringWithFormat:@"%u seconds", seconds]
                                       : [NSString stringWithFormat:@"%u %@", seconds / 60, seconds == 60 ? @"minute" : @"minutes"]];
    NSGridView* replay = [self grid:@[@[_replay, _replayLength]]];

    _camera = [self check:@"Camera"];
    _cameraDevice = [self popup:@"Camera device"];
    _corner = [self popup:@"Camera position"];
    for (const auto& choice : kCorners) [_corner addItemWithTitle:ns(choice.label)];
    _cameraSize = [self popup:@"Camera size"];
    for (const auto& choice : kCameraSizes) [_cameraSize addItemWithTitle:ns(choice.label)];
    _cameraShape = [self popup:@"Camera shape"];
    for (const auto& choice : rec::app::kCameraShapes) [_cameraShape addItemWithTitle:ns(choice.label)];
    _roundness = [NSSlider sliderWithValue:0.2 minValue:0 maxValue:1 target:self action:@selector(changed:)];
    _roundness.continuous = YES;
    _roundness.accessibilityLabel = @"Camera roundness";
    _cameraBorder = [self popup:@"Camera border"];
    for (const auto& choice : rec::app::kCameraBorders) [_cameraBorder addItemWithTitle:ns(choice.label)];
    _shadow = [self check:@"Drop shadow"];
    _image = [self check:@"Image overlay"];
    _imageFile = label(@"");
    _imageFile.textColor = NSColor.secondaryLabelColor;
    _imageFile.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _imageFile.accessibilityLabel = @"Image file";
    [_imageFile setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _chooseImage = [self button:@"Choose…" action:@selector(chooseImage:)];
    NSStackView* imageRow = [NSStackView stackViewWithViews:@[_imageFile, _chooseImage]];
    imageRow.distribution = NSStackViewDistributionFill;
    NSGridView* camera = [self grid:@[@[_camera, _cameraDevice], @[label(@"Position"), _corner], @[label(@"Size"), _cameraSize],
                                      @[label(@"Shape"), _cameraShape], @[label(@"Roundness"), _roundness],
                                      @[label(@"Border"), _cameraBorder], @[NSGridCell.emptyContentView, _shadow],
                                      @[_image, imageRow]]];
    [camera columnAtIndex:0].width = 120;

    // The scenes and the sources they arrange.
    _scene = [self popup:@"Scene"];
    _newScene = [self button:@"New Scene" action:@selector(newScene:)];
    _deleteScene = [self button:@"Delete Scene" action:@selector(deleteScene:)];
    NSStackView* sceneButtons = [NSStackView stackViewWithViews:@[_newScene, _deleteScene]];
    sceneButtons.distribution = NSStackViewDistributionFillEqually;
    _sceneName = [self field:@"Scene name" secure:NO];
    _verticalLayout = [self popup:@"Vertical video layout"];
    for (const auto& choice : rec::app::kVerticalLayouts) [_verticalLayout addItemWithTitle:ns(choice.label)];
    NSGridView* scenes = [self grid:@[@[label(@"Scene"), _scene], @[sceneButtons], @[label(@"Name"), _sceneName],
                                      @[label(@"Vertical"), _verticalLayout]]];
    [scenes columnAtIndex:0].width = 50;
    _sourcesTable = [[NSTableView alloc] init];
    NSTableColumn* sourceColumn = [[NSTableColumn alloc] initWithIdentifier:@"source"];
    sourceColumn.editable = NO;
    sourceColumn.resizingMask = NSTableColumnAutoresizingMask;
    [_sourcesTable addTableColumn:sourceColumn];
    _sourcesTable.headerView = nil;
    _sourcesTable.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    _sourcesTable.dataSource = self;
    _sourcesTable.delegate = self;
    _sourcesTable.accessibilityLabel = @"Sources";
    NSScrollView* sourcesScroll = [[NSScrollView alloc] init];
    sourcesScroll.documentView = _sourcesTable;
    sourcesScroll.hasVerticalScroller = YES;
    sourcesScroll.borderType = NSBezelBorder;
    [sourcesScroll.heightAnchor constraintEqualToConstant:120].active = YES;
    _sourceShown = [self check:@"Show in this scene"];
    _addDisplay = [self button:@"Add Display…" action:@selector(addSourceMenu:)];
    _addWindow = [self button:@"Add Window…" action:@selector(addSourceMenu:)];
    _removeSource = [self button:@"Remove" action:@selector(removeSource:)];
    NSStackView* sourceButtons = [NSStackView stackViewWithViews:@[_addDisplay, _addWindow, _removeSource]];
    sourceButtons.distribution = NSStackViewDistributionFillEqually;
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
    _verticalMovie = [self check:@"Also save a vertical video (9:16, MP4)"];

    // Status and preview side by side over two columns: what and how to record on the left,
    // where it goes on the right, and the scenes in a narrower third column. One column would
    // be taller than a 13-inch screen.
    NSArray<NSView*>* status = @[_status, _detail, _live, meters, buttons];
    NSArray<NSView*>* left = @[header(@"Capture"), capture, header(@"Quality"), quality, header(@"Recordings"), _folder, folderButtons,
                               _verticalMovie];
    NSArray<NSView*>* right = @[header(@"Live stream"), live, header(@"Replay"), replay];
    NSArray<NSView*>* third = @[header(@"Scenes"), scenes, label(@"Sources"), sourcesScroll, _sourceShown, sourceButtons,
                                header(@"Camera and image"), camera];
    const auto column = [](NSArray<NSView*>* views, CGFloat width) {
        NSStackView* stack = [NSStackView stackViewWithViews:views];
        stack.orientation = NSUserInterfaceLayoutOrientationVertical;
        stack.alignment = NSLayoutAttributeLeading;
        stack.spacing = 8;
        // Side by side, the shorter column would otherwise stretch to the taller one's height.
        [stack setHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationVertical];
        for (NSView* view in views) [view.widthAnchor constraintEqualToConstant:width].active = YES;
        return stack;
    };
    NSStackView* statusColumn = column(status, 440);
    _showPreview = [self check:@"Show preview"];
    // With the vertical video, its preview stands beside the landscape one.
    NSStackView* previews = [NSStackView stackViewWithViews:@[_preview, _preview2]];
    previews.distribution = NSStackViewDistributionFill;
    previews.spacing = 8;
    NSStackView* top = [NSStackView stackViewWithViews:@[statusColumn, column(@[previews, _showPreview], 440)]];
    top.alignment = NSLayoutAttributeTop;
    top.spacing = 30;
    NSStackView* leftColumn = column(left, 440);
    NSStackView* rightColumn = column(right, 440);
    NSStackView* scenesColumn = column(third, 330);
    for (NSView* view in @[capture, quality]) [leftColumn setCustomSpacing:18 afterView:view];
    for (NSView* view in @[live]) [rightColumn setCustomSpacing:18 afterView:view];
    [scenesColumn setCustomSpacing:18 afterView:sourceButtons];
    NSStackView* columns = [NSStackView stackViewWithViews:@[leftColumn, rightColumn]];
    columns.alignment = NSLayoutAttributeTop;
    columns.spacing = 30;
    NSStackView* body = [NSStackView stackViewWithViews:@[top, columns]];
    body.orientation = NSUserInterfaceLayoutOrientationVertical;
    body.alignment = NSLayoutAttributeLeading;
    body.spacing = 8;
    [body setCustomSpacing:18 afterView:top];
    NSStackView* stack = [NSStackView stackViewWithViews:@[body, scenesColumn]];
    stack.alignment = NSLayoutAttributeTop;
    stack.spacing = 30;
    stack.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
    // A top-aligned stack keeps the top inset but not the bottom one: the tallest column ended
    // at the window's bottom edge.
    for (NSView* view in stack.arrangedSubviews)
        [view.bottomAnchor constraintLessThanOrEqualToAnchor:stack.bottomAnchor constant:-20].active = YES;
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
    _clicks.state = s.clicks ? NSControlStateValueOn : NSControlStateValueOff;
    [_resolution selectItemAtIndex:choice_index(kResolutions, s.resolution)];
    const auto rate = std::find(kFrameRates.begin(), kFrameRates.end(), s.fps);
    [_fps selectItemAtIndex:rate == kFrameRates.end() ? 4 : rate - kFrameRates.begin()];
    [_codec selectItemAtIndex:choice_index(kCodecs, s.codec)];
    [_format selectItemAtIndex:choice_index(kFormats, s.format)];
    _stream.state = s.stream ? NSControlStateValueOn : NSControlStateValueOff;
    _verticalMovie.state = s.vertical_movie ? NSControlStateValueOn : NSControlStateValueOff;
    _bitrate.stringValue = [NSString stringWithFormat:@"%u", s.stream_kbps];
    _replay.state = s.replay ? NSControlStateValueOn : NSControlStateValueOff;
    const auto length = std::find(rec::app::kReplayLengths.begin(), rec::app::kReplayLengths.end(), s.replay_seconds);
    [_replayLength selectItemAtIndex:length == rec::app::kReplayLengths.end() ? 1 : length - rec::app::kReplayLengths.begin()];
    _camera.state = s.camera ? NSControlStateValueOn : NSControlStateValueOff;
    [self showCameraStyle];
    [self showVolume:rec::AudioInput::system];
    [self showVolume:rec::AudioInput::microphone];
    _image.state = s.image ? NSControlStateValueOn : NSControlStateValueOff;
    _showPreview.state = s.preview ? NSControlStateValueOn : NSControlStateValueOff;
    [self showImage];
    [self showCameras:_connected];
    _folder.stringValue = ns(s.folder);
    _folder.toolTip = ns(s.folder);
    [self showDestinations:std::vector<std::string>()];
    [self showDisplays:_displays windows:_windows microphones:_microphones];
    [self showScenes];
}

- (void)showVolume:(rec::AudioInput)input {
    const bool system = input == rec::AudioInput::system;
    (system ? _systemVolume : _micVolume).integerValue = system ? _settings.system_volume : _settings.mic_volume;
    (system ? _systemMute : _micMute).state = (system ? _settings.system_muted : _settings.mic_muted) ? NSControlStateValueOn : NSControlStateValueOff;
    [self showDecibels:system];
}

- (void)showDecibels:(bool)system {
    const int volume = system ? _settings.system_volume : _settings.mic_volume;
    NSString* text = [NSString stringWithFormat:@"%@%d dB", volume > 0 ? @"+" : @"", volume];
    (system ? _systemVolumeText : _micVolumeText).stringValue = text;
    // VoiceOver reads the slider's value in dB rather than as a bare number.
    (system ? _systemVolume : _micVolume).accessibilityValueDescription = text;
}

// Reads a source's slider and mute box, and applies them at once while recording.
- (void)changeVolume:(rec::AudioInput)input {
    const bool system = input == rec::AudioInput::system;
    int& volume = system ? _settings.system_volume : _settings.mic_volume;
    bool& muted = system ? _settings.system_muted : _settings.mic_muted;
    volume = static_cast<int>(std::lround((system ? _systemVolume : _micVolume).doubleValue));
    muted = (system ? _systemMute : _micMute).state == NSControlStateValueOn;
    [self showDecibels:system];
    const auto s = _session->status();
    if ((s.state == State::recording || s.state == State::paused) && (system ? s.system_audio : s.microphone)) {
        if (auto changed = _session->set_volume(input, volume, muted); !changed) {
            _note = [@"Volume not changed: " stringByAppendingString:ns(changed.error)];
            _noteAttention = true;
            _noteUntil = [NSDate dateWithTimeIntervalSinceNow:5];
        }
    }
    // A drag saves once, when it ends.
    if (NSApp.currentEvent.type != NSEventTypeLeftMouseDragged) rec::app::save_settings(_settingsPath, _settings);
    [self refresh];
}

- (void)volumeChanged:(NSSlider*)slider {
    [self changeVolume:slider == _systemVolume ? rec::AudioInput::system : rec::AudioInput::microphone];
}

- (void)toggleMute:(NSMenuItem*)item {
    NSButton* box = item.tag == 0 ? _systemMute : _micMute;
    box.state = box.state == NSControlStateValueOn ? NSControlStateValueOff : NSControlStateValueOn;
    [self changeVolume:item.tag == 0 ? rec::AudioInput::system : rec::AudioInput::microphone];
}

- (void)showImage {
    const auto& path = _settings.image_path;
    _imageFile.stringValue = path.empty() ? @"No image chosen" : ns(path).lastPathComponent;
    _imageFile.toolTip = path.empty() ? nil : ns(path);
}

// Presets show their fixed server; X, TikTok and custom servers are pasted in, with a hint
// where to find them.
// keys holds what each row's key field shows, in order: a key moves with its destination
// when rows are added or removed. An empty one shows the key kept for the row, if any; the
// others are kept under the row's key name, which can change with the rows above it.
- (void)showDestinations:(std::vector<std::string>)keys {
    const auto& list = _settings.destinations;
    keys.resize(list.size());
    for (std::size_t i = 0; i < kMaxDestinations; ++i) {
        _destRow[i].hidden = i >= list.size();
        if (i >= list.size()) continue;
        const auto& d = list[i];
        _destOn[i].state = d.on ? NSControlStateValueOn : NSControlStateValueOff;
        [_destService[i] selectItemAtIndex:choice_index(kServices, d.service)];
        _destVertical[i].state = d.vertical ? NSControlStateValueOn : NSControlStateValueOff;
        [self showServer:i];
        _destKey[i].stringValue = ns(keys[i]);
        if (keys[i].empty()) [self loadKey:i];
        else store_key(rec::app::key_name(_settings, i), keys[i]);
    }
    _addDestination.hidden = list.size() >= kMaxDestinations;
    // The rows have no labels of their own; VoiceOver says which destination each control is for.
    for (std::size_t i = 0; i < list.size(); ++i) {
        NSString* name = ns(rec::app::destination_name(_settings, i));
        _destOn[i].accessibilityLabel = [@"Stream to " stringByAppendingString:name];
        _destService[i].accessibilityLabel = [NSString stringWithFormat:@"Destination %zu service", i + 1];
        _destVertical[i].accessibilityLabel = [@"Send the vertical video to " stringByAppendingString:name];
        _destRemove[i].accessibilityLabel = [@"Remove " stringByAppendingString:name];
        _destServer[i].accessibilityLabel = [name stringByAppendingString:@" server URL"];
        _destKey[i].accessibilityLabel = [name stringByAppendingString:@" stream key"];
    }
}

// Presets show their fixed server; X, TikTok and custom servers are pasted in, with a hint
// where to find them.
- (void)showServer:(std::size_t)i {
    const auto& d = _settings.destinations[i];
    const bool typed = rec::app::typed_server(d.service);
    NSTextField* server = _destServer[i];
    server.stringValue = ns(typed ? d.server : rec::app::server_url(d));
    server.editable = typed;
    server.selectable = YES;
    server.placeholderString = ns(rec::app::server_hint(d.service));
}

// Off the main thread: a keychain prompt (a rebuilt app no longer matches the item's access
// list) would otherwise keep the window from appearing at launch until it is answered.
- (void)loadKey:(std::size_t)i {
    const std::string name = rec::app::key_name(_settings, i);
    if (name.empty()) return;
    NSSecureTextField* field = _destKey[i];
    __weak RecController* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString* key = ns(load_key(name));
        dispatch_async(dispatch_get_main_queue(), ^{
            RecController* me = weakSelf;
            // A key typed meanwhile, or another server chosen, wins.
            if (!me || field.stringValue.length || rec::app::key_name(me->_settings, i) != name) return;
            field.stringValue = key;
        });
    });
}

- (std::vector<std::string>)streamKeys {
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < _settings.destinations.size(); ++i) keys.push_back(utf8(_destKey[i].stringValue));
    return keys;
}

// A key is kept as soon as it is entered, and forgotten when its field is emptied.
- (void)keepKey:(std::size_t)i {
    const auto name = rec::app::key_name(_settings, i);
    const auto key = utf8(_destKey[i].stringValue);
    if (key.empty()) delete_key(name);
    else store_key(name, key);
}

- (void)addDestination:(id)sender {
    [_window makeFirstResponder:nil];
    auto keys = [self streamKeys];
    [self readSettings];
    if (!rec::app::add_destination(_settings)) return;
    [self showDestinations:keys];
    rec::app::save_settings(_settingsPath, _settings);
    [self refresh];
}

- (void)removeDestination:(NSButton*)sender {
    const auto i = static_cast<std::size_t>(sender.tag);
    if (i >= _settings.destinations.size() || _settings.destinations.size() < 2) return;
    [_window makeFirstResponder:nil];
    auto keys = [self streamKeys];
    [self readSettings];
    keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(i));
    rec::app::remove_destination(_settings, i);
    [self showDestinations:keys];
    rec::app::save_settings(_settingsPath, _settings);
    [self refresh];
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
    s.clicks = _clicks.state == NSControlStateValueOn;
    const auto at = [](NSPopUpButton* popup) { return static_cast<std::size_t>(std::max<NSInteger>(0, popup.indexOfSelectedItem)); };
    s.resolution = kResolutions[at(_resolution)].value;
    s.fps = kFrameRates[at(_fps)];
    s.codec = kCodecs[at(_codec)].value;
    s.format = kFormats[at(_format)].value;
    s.stream = _stream.state == NSControlStateValueOn;
    for (std::size_t i = 0; i < s.destinations.size(); ++i) {
        auto& d = s.destinations[i];
        d.on = _destOn[i].state == NSControlStateValueOn;
        d.vertical = _destVertical[i].state == NSControlStateValueOn;
        // The typed server belongs to the service it was typed for.
        if (rec::app::typed_server(d.service)) d.server = utf8(_destServer[i].stringValue);
        const std::string service = kServices[at(_destService[i])].value;
        if (service != d.service) {
            d.service = service;
            d.server.clear();
        }
    }
    s.vertical_movie = _verticalMovie.state == NSControlStateValueOn;
    s.replay = _replay.state == NSControlStateValueOn;
    s.replay_seconds = rec::app::kReplayLengths[std::min(at(_replayLength), rec::app::kReplayLengths.size() - 1)];
    s.camera = _camera.state == NSControlStateValueOn;
    if (!_cameras.empty()) s.camera_device = _cameras[std::min(at(_cameraDevice), _cameras.size() - 1)].id;
    s.image = _image.state == NSControlStateValueOn;
    s.preview = _showPreview.state == NSControlStateValueOn;
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
    for (std::size_t i = 0; i < _settings.destinations.size(); ++i) {
        if (sender == _destService[i]) {
            // TikTok is watched on phones; its stream starts out vertical.
            if (std::string(kServices[static_cast<std::size_t>(std::max<NSInteger>(0, _destService[i].indexOfSelectedItem))].value) == "tiktok")
                _destVertical[i].state = NSControlStateValueOn;
            [_window makeFirstResponder:nil];
            auto keys = [self streamKeys];
            keys[i].clear();  // Another service: the key kept for it.
            [self remember];
            [self showDestinations:keys];
        }
        if (sender == _destOn[i] || sender == _destVertical[i]) [self remember];
    }
    if (sender == _verticalMovie) [self remember];
    if (sender == _verticalLayout) {
        rec::app::current_scene(_settings).vertical =
            rec::app::kVerticalLayouts[static_cast<std::size_t>(std::max<NSInteger>(0, _verticalLayout.indexOfSelectedItem))].value;
        [self applyScene];
        return;
    }
    if (sender == _systemMute || sender == _micMute) {
        [self changeVolume:sender == _systemMute ? rec::AudioInput::system : rec::AudioInput::microphone];
        return;
    }
    if (sender == _scene) {
        [self changeScene:static_cast<std::size_t>(std::max<NSInteger>(0, _scene.indexOfSelectedItem))];
        return;
    }
    if (sender == _corner || sender == _cameraSize) {
        [self placeCamera];
        return;
    }
    if (sender == _cameraShape || sender == _roundness || sender == _cameraBorder || sender == _shadow) {
        [self changeCameraStyle];
        return;
    }
    if (sender == _sourceShown) {
        rec::app::scene_item(rec::app::current_scene(_settings), _selected).visible = _sourceShown.state == NSControlStateValueOn;
        [self showSceneSources];
        [self applyScene];
        return;
    }
    if (sender == _camera || sender == _image) {
        [self readSettings];
        [self showSceneSources];
    }
    if (sender == _showPreview) [self remember];
    [self refresh];
}

- (void)windowChanged:(NSNotification*)notification { [self refresh]; }

- (void)chooseScene:(NSMenuItem*)item {
    if (item.tag >= 0) [self changeScene:static_cast<std::size_t>(item.tag)];
}

// While recording the scene shows at once; otherwise it is the scene to start with, shown
// in the preview.
- (void)changeScene:(std::size_t)index {
    if (index >= _settings.scenes.size()) return;
    _settings.scene = static_cast<std::uint32_t>(index);
    [self showScenes];
    [self applyScene];
}

// Shows the current scene in the recording or the preview, and saves it.
- (void)applyScene {
    if (auto shown = _session->show_scene(_settings); !shown) {
        _note = [@"Scene not changed: " stringByAppendingString:ns(shown.error)];
        _noteAttention = true;
        _noteUntil = [NSDate dateWithTimeIntervalSinceNow:5];
    }
    rec::app::save_settings(_settingsPath, _settings);
    [self refresh];
}

// The Scene list, the scene's name, its sources and the camera's place.
- (void)showScenes {
    [_scene removeAllItems];
    // Through the menu: addItemWithTitle: would drop a scene whose name repeats.
    for (const auto& scene : _settings.scenes)
        [_scene.menu addItem:[[NSMenuItem alloc] initWithTitle:ns(scene.name) action:nil keyEquivalent:@""]];
    const auto& scene = rec::app::current_scene(_settings);
    [_scene selectItemAtIndex:static_cast<NSInteger>(_settings.scene)];
    if (![_sceneName.stringValue isEqualToString:ns(scene.name)]) _sceneName.stringValue = ns(scene.name);
    [_verticalLayout selectItemAtIndex:choice_index(rec::app::kVerticalLayouts, scene.vertical)];
    [self showSceneSources];
    [self showCameraPlace];
}

// Every source, as the scene shows it; the selected one stays selected.
- (void)showSceneSources {
    const auto sources = rec::app::scene_sources(_settings);
    std::size_t chosen = 0;
    for (std::size_t i = 0; i < sources.size(); ++i)
        if (sources[i] == _selected) chosen = i;
    _selected = sources[chosen];
    _filling = true;
    [_sourcesTable reloadData];
    [_sourcesTable selectRowIndexes:[NSIndexSet indexSetWithIndex:chosen] byExtendingSelection:NO];
    _filling = false;
    _sourceShown.state = rec::app::scene_item(rec::app::current_scene(_settings), _selected).visible ? NSControlStateValueOn
                                                                                                     : NSControlStateValueOff;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)table {
    return static_cast<NSInteger>(rec::app::scene_sources(_settings).size());
}

- (id)tableView:(NSTableView*)table objectValueForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
    const auto sources = rec::app::scene_sources(_settings);
    if (row < 0 || row >= static_cast<NSInteger>(sources.size())) return @"";
    const auto& source = sources[static_cast<std::size_t>(row)];
    NSString* name = ns(rec::app::source_name(_settings, source));
    return rec::app::scene_item(rec::app::current_scene(_settings), source).visible ? name
                                                                                     : [name stringByAppendingString:@" (hidden)"];
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    if (_filling) return;
    const auto sources = rec::app::scene_sources(_settings);
    const NSInteger row = _sourcesTable.selectedRow;
    if (row < 0 || row >= static_cast<NSInteger>(sources.size())) return;
    _selected = sources[static_cast<std::size_t>(row)];
    [self showSceneSources];
    [self refresh];
}

// The Position and Size lists name the camera's place in this scene when it is a preset.
- (void)showCameraPlace {
    const auto& area = rec::app::current_scene(_settings).camera.area;
    [_corner selectItemAtIndex:choice_index(kCorners, rec::app::corner_of(area))];
    if (const auto size = rec::app::size_of(area); !size.empty()) [_cameraSize selectItemAtIndex:choice_index(kCameraSizes, size)];
}

- (void)showCameraStyle {
    const auto& style = _settings.camera_style;
    [_cameraShape selectItemAtIndex:static_cast<NSInteger>(style.shape)];
    _roundness.doubleValue = style.roundness;
    [_cameraBorder selectItemAtIndex:rec::app::border_index(style)];
    _shadow.state = style.shadow ? NSControlStateValueOn : NSControlStateValueOff;
}

// A new shape, roundness, border or shadow shows at once, also while recording.
- (void)changeCameraStyle {
    auto& style = _settings.camera_style;
    const auto shapes = static_cast<NSInteger>(rec::app::kCameraShapes.size());
    style.shape = static_cast<rec::Style::Shape>(std::clamp<NSInteger>(_cameraShape.indexOfSelectedItem, 0, shapes - 1));
    style.roundness = std::clamp(_roundness.doubleValue, 0.0, 1.0);
    if (const NSInteger b = _cameraBorder.indexOfSelectedItem; b >= 0) {
        style.border = rec::app::kCameraBorders[static_cast<std::size_t>(b)].width;
        style.border_color = rec::app::kCameraBorders[static_cast<std::size_t>(b)].color;
    }
    style.shadow = _shadow.state == NSControlStateValueOn;
    [self applyScene];
}

// A preset Position or Size puts the camera there in this scene.
- (void)placeCamera {
    const std::string corner = kCorners[static_cast<std::size_t>(std::max<NSInteger>(0, _corner.indexOfSelectedItem))].value;
    if (corner == "custom") return;
    const std::string size = kCameraSizes[static_cast<std::size_t>(std::max<NSInteger>(0, _cameraSize.indexOfSelectedItem))].value;
    rec::app::current_scene(_settings).camera.area = rec::app::corner_area(corner, size);
    [self showCameraPlace];
    [self applyScene];
}

- (void)newScene:(id)sender {
    if (!rec::app::add_scene(_settings)) return;
    [self showScenes];
    [self applyScene];
}

- (void)deleteScene:(id)sender {
    rec::app::remove_scene(_settings);
    [self showScenes];
    [self applyScene];
}

- (void)sceneNameChanged {
    auto& scene = rec::app::current_scene(_settings);
    scene.name = utf8(_sceneName.stringValue);
    [_scene itemAtIndex:static_cast<NSInteger>(_settings.scene)].title = ns(scene.name);
}

- (void)addSourceMenu:(NSButton*)sender {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;
    if (sender == _addWindow) {
        _menuWindows = _windows;
        for (std::size_t i = 0; i < _menuWindows.size(); ++i) {
            const auto& w = _menuWindows[i];
            NSString* title = ns(w.title.empty() ? w.app : w.app.empty() ? w.title : w.app + " – " + w.title);
            NSMenuItem* item = [self item:title action:@selector(addWindowLayer:) key:@""];
            item.tag = static_cast<NSInteger>(i);
            [menu addItem:item];
        }
        if (_menuWindows.empty()) {
            NSMenuItem* none = [self item:@"No windows are open" action:nil key:@""];
            none.enabled = NO;
            [menu addItem:none];
        }
    } else {
        for (std::size_t i = 0; i < std::max<std::size_t>(1, _displays.size()); ++i) {
            NSString* title = i < _displays.size()
                ? [NSString stringWithFormat:@"Display %zu (%ux%u)", i + 1, _displays[i].width, _displays[i].height]
                : @"Display 1";
            NSMenuItem* item = [self item:title action:@selector(addDisplayLayer:) key:@""];
            item.tag = static_cast<NSInteger>(i);
            [menu addItem:item];
        }
    }
    [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, NSHeight(sender.bounds) + 4) inView:sender];
}

- (void)addDisplayLayer:(NSMenuItem*)item {
    rec::app::Layer layer;
    layer.display = static_cast<std::uint32_t>(std::max<NSInteger>(0, item.tag) + 1);
    [self addLayer:layer];
}

- (void)addWindowLayer:(NSMenuItem*)item {
    if (item.tag < 0 || item.tag >= static_cast<NSInteger>(_menuWindows.size())) return;
    const auto& w = _menuWindows[static_cast<std::size_t>(item.tag)];
    rec::app::Layer layer;
    layer.kind = "window";
    layer.window_app = w.app;
    layer.window_title = w.title;
    [self addLayer:layer];
}

// A new display or window, shown over the whole frame in this scene and hidden in the others.
- (void)addLayer:(const rec::app::Layer&)layer {
    if (_session->status().state != State::idle) return;
    if (!rec::app::add_layer(_settings, layer)) {
        _note = [NSString stringWithFormat:@"Scenes hold at most %zu displays and windows", rec::app::kMaxLayers];
        _noteAttention = true;
        _noteUntil = [NSDate dateWithTimeIntervalSinceNow:5];
        [self refresh];
        return;
    }
    _selected = {rec::app::SourceRef::Kind::layer, _settings.layers.size() - 1};
    [self showSceneSources];
    rec::app::save_settings(_settingsPath, _settings);
    [self refresh];
}

- (void)removeSource:(id)sender {
    if (_session->status().state != State::idle || _selected.kind != rec::app::SourceRef::Kind::layer) return;
    rec::app::remove_layer(_settings, _selected.layer);
    _selected = {};
    [self showSceneSources];
    rec::app::save_settings(_settingsPath, _settings);
    [self refresh];
}
- (void)chooseImage:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = NO;
    panel.canChooseFiles = YES;
    panel.allowedFileTypes = @[@"png", @"jpg", @"jpeg", @"heic", @"tiff", @"tif", @"gif", @"bmp"];
    panel.prompt = @"Choose";
    panel.message = @"Choose an image to show over the recording";
    __weak RecController* weakSelf = self;
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse response) {
        RecController* strongSelf = weakSelf;
        if (!strongSelf || response != NSModalResponseOK || !panel.URL.path) return;
        strongSelf->_settings.image_path = utf8(panel.URL.path);
        strongSelf->_image.state = NSControlStateValueOn;
        [strongSelf showImage];
        [strongSelf remember];
        [strongSelf refresh];
    }];
}

- (void)controlTextDidChange:(NSNotification*)notification {
    if (notification.object == _sceneName) [self sceneNameChanged];
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    if (notification.object == _sceneName) rec::app::save_settings(_settingsPath, _settings);
    for (std::size_t i = 0; i < _settings.destinations.size(); ++i) {
        // A pasted server finds the key kept for it.
        if (notification.object == _destServer[i] && rec::app::typed_server(_settings.destinations[i].service)) {
            [self remember];
            [self loadKey:i];
        }
        if (notification.object == _destKey[i]) [self keepKey:i];
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
        if (auto started = _session->start(_settings, [self streamKeys], _testPattern); !started) {
            [self warn:ns(started.error)];
            return;
        }
        for (std::size_t i = 0; i < _settings.destinations.size(); ++i)
            if (_settings.stream && _settings.destinations[i].on) [self keepKey:i];
    }
    [self refresh];
}

- (void)saveReplay:(id)sender {
    std::string path;
    double seconds = 0;
    if (auto saved = _session->save_replay(_settings.folder, path, seconds); !saved) {
        _note = [@"Replay not saved: " stringByAppendingString:ns(saved.error)];
        _noteAttention = true;
    } else {
        _note = [NSString stringWithFormat:@"Saved replay (%.1f s): %@", seconds, ns(path).lastPathComponent];
        _noteAttention = false;
        _settings.last_recording = path;
        rec::app::save_settings(_settingsPath, _settings);
    }
    _noteUntil = [NSDate dateWithTimeIntervalSinceNow:5];
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

// Quitting during a recording saves the movie first, unless the background engine is to keep
// recording.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    const auto state = _session->status().state;
    if (state == State::idle) {
        [self remember];
        _saved = true;
        return NSTerminateNow;
    }
    if (state == State::recording || state == State::paused) {
        const auto keep = [self keepRecording];
        if (!keep) return NSTerminateCancel;
        if (*keep) {
            // The engine records on; opening Rec again (or `rec stop`) stops it.
            _session->detach();
            [self remember];
            _saved = true;
            return NSTerminateNow;
        }
    }
    _quitting = true;
    _session->stop();
    [self refresh];
    return NSTerminateLater;
}

// Whether quitting leaves the recording running: the setting, or the first time, the
// person's answer, which becomes the setting. Empty when they cancel.
- (std::optional<bool>)keepRecording {
    if (_settings.keep_recording != "ask") return _settings.keep_recording == "yes";
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Keep recording after Rec quits?";
    alert.informativeText = @"Rec can keep recording in the background. Open Rec again to see it and stop it.\n\n"
                            @"Rec remembers your answer. Change it in the menu bar item's menu: Keep Recording After Quit.";
    [alert addButtonWithTitle:@"Keep Recording"];
    [alert addButtonWithTitle:@"Stop and Save"];
    [alert addButtonWithTitle:@"Cancel"];
    const NSModalResponse answer = [alert runModal];
    if (answer != NSAlertFirstButtonReturn && answer != NSAlertSecondButtonReturn) return std::nullopt;
    _settings.keep_recording = answer == NSAlertFirstButtonReturn ? "yes" : "no";
    rec::app::save_settings(_settingsPath, _settings);
    return answer == NSAlertFirstButtonReturn;
}

- (void)toggleKeepRecording:(id)sender {
    _settings.keep_recording = _settings.keep_recording == "yes" ? "no" : "yes";
    rec::app::save_settings(_settingsPath, _settings);
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    if (!_saved) [self remember];
    if (_hotkeyRef) UnregisterEventHotKey(_hotkeyRef);
    if (_replayHotkeyRef) UnregisterEventHotKey(_replayHotkeyRef);
    // Joins the idle preview before exit() can kill it mid-start: Metal's shader-cache database
    // (archiveUsage.db), shared with the engine, keeps a lock held by a killed thread, and the
    // next launch hangs in Starting… for as long as the engine has the database open.
    _session.reset();
}

// MARK: - Status

- (void)tick:(NSTimer*)timer { [self refresh]; }

- (void)showPreview:(NSTimer*)timer {
    if (_session->preview(_frame)) {
        [self draw:_frame in:_preview];
        _previewHint.hidden = YES;
    }
    if (_verticalShown && _session->preview(_frame2, rec::kSecondCanvas)) [self draw:_frame2 in:_preview2];
}

- (void)draw:(const rec::PreviewFrame&)frame in:(NSView*)view {
    NSData* data = [NSData dataWithBytes:frame.pixels.data() length:frame.pixels.size()];
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = CGImageCreate(frame.width, frame.height, 8, 32, frame.width * 4, space,
                                     kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, provider, nullptr, false,
                                     kCGRenderingIntentDefault);
    view.layer.contents = (__bridge id)image;
    CGImageRelease(image);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
}

// Where the frame is drawn in the preview: the output's shape, centered (view coordinates,
// origin bottom left).
- (NSRect)previewFrameRect {
    const NSRect b = _preview.bounds;
    if (!_frame.number || !_frame.width || !_frame.height) return NSZeroRect;
    const double aspect = static_cast<double>(_frame.width) / _frame.height;
    double w = b.size.width, h = w / aspect;
    if (h > b.size.height) {
        h = b.size.height;
        w = h * aspect;
    }
    return NSMakeRect(b.origin.x + (b.size.width - w) / 2, b.origin.y + (b.size.height - h) / 2, w, h);
}

// The source a press at point takes hold of in the preview: the topmost one shown there, in
// this scene, before Start or in this recording.
- (std::optional<PreviewHit>)hitAt:(NSPoint)point {
    if (!_frame.number || (_shownState != State::idle && _shownState != State::recording && _shownState != State::paused))
        return std::nullopt;
    const NSRect r = [self previewFrameRect];
    if (r.size.width <= 0) return std::nullopt;
    const double x = (point.x - r.origin.x) / r.size.width, y = 1 - (point.y - r.origin.y) / r.size.height;
    const double handle_x = 12 / r.size.width, handle_y = 12 / r.size.height;
    const auto source = rec::app::source_at(_settings, x, y, handle_x, handle_y);
    if (!source) return std::nullopt;
    const auto& item = rec::app::scene_item(rec::app::current_scene(_settings), *source);
    const auto drag = rec::app::grab_area(item.area, x, y, handle_x, handle_y, source->kind == rec::app::SourceRef::Kind::camera);
    if (drag.grab == rec::app::AreaDrag::Grab::none) return std::nullopt;
    return PreviewHit{*source, drag};
}

- (void)showOutline {
    std::optional<rec::Area> area;
    if (_drag.grab != rec::app::AreaDrag::Grab::none) area = _outlineArea;
    else if (_hoverSource) area = rec::app::scene_item(rec::app::current_scene(_settings), *_hoverSource).area;
    const NSRect r = [self previewFrameRect];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _outline.hidden = !area || r.size.width <= 0;
    if (!_outline.hidden)
        _outline.frame = CGRectMake(r.origin.x + area->x * r.size.width, r.origin.y + (1 - area->y - area->height) * r.size.height,
                                    area->width * r.size.width, area->height * r.size.height);
    [CATransaction commit];
}

// Dragging a source in the preview moves it, dragging a corner resizes it; the recording or
// the preview before Start follows at once.
- (void)preview:(NSView*)view pointer:(Pointer)pointer event:(NSEvent*)event {
    using Grab = rec::app::AreaDrag::Grab;
    const NSPoint point = [view convertPoint:event.locationInWindow fromView:nil];
    const auto cursor = [](const rec::app::AreaDrag& grab, bool held) {
        [(grab.grab == Grab::none     ? NSCursor.arrowCursor
          : grab.grab == Grab::resize ? NSCursor.crosshairCursor
          : held                      ? NSCursor.closedHandCursor
                                      : NSCursor.openHandCursor) set];
    };
    switch (pointer) {
    case Pointer::down: {
        const auto hit = [self hitAt:point];
        if (!hit) return;
        _drag = hit->drag;
        _dragged = hit->source;
        _selected = hit->source;
        [self showSceneSources];
        _outlineArea = _drag.from;
        cursor(_drag, true);
        break;
    }
    case Pointer::dragged: {
        const NSRect r = [self previewFrameRect];
        if (_drag.grab == Grab::none || r.size.width <= 0) return;
        _outlineArea = rec::app::drag_area(_drag, (point.x - r.origin.x) / r.size.width, 1 - (point.y - r.origin.y) / r.size.height);
        rec::app::scene_item(rec::app::current_scene(_settings), _dragged).area = _outlineArea;
        if (_dragged.kind == rec::app::SourceRef::Kind::camera) [self showCameraPlace];
        _session->show_scene(_settings);
        break;
    }
    case Pointer::up: {
        if (_drag.grab == Grab::none) return;
        _drag = {};
        rec::app::save_settings(_settingsPath, _settings);
        [self refresh];
        const auto hit = [self hitAt:point];
        cursor(hit ? hit->drag : rec::app::AreaDrag{}, false);
        break;
    }
    case Pointer::moved: {
        const auto hit = [self hitAt:point];
        _hoverSource = hit ? std::optional(hit->source) : std::nullopt;
        cursor(hit ? hit->drag : rec::app::AreaDrag{}, false);
        break;
    }
    case Pointer::exited:
        _hoverSource.reset();
        if (_drag.grab == Grab::none) [NSCursor.arrowCursor set];
        break;
    }
    [self showOutline];
}

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
        _note = nil;
    }

    NSString* status = @"";
    NSString* detail = @"";
    _attention = false;
    switch (s.state) {
    case State::idle:
        status = @"Ready to record";
        if (!s.error.empty()) { detail = [@"Recording failed: " stringByAppendingString:ns(s.error)]; _attention = true; }
        else if (!s.stream_error.empty()) { detail = [@"Stream stopped early: " stringByAppendingString:ns(s.stream_error)]; _attention = true; }
        else if (!s.saved.empty())
            detail = [[(s.window_closed ? @"The window closed. Saved " : @"Saved ") stringByAppendingString:ns(s.saved).lastPathComponent]
                stringByAppendingString:s.saved_vertical.empty() ? @"" : @" and its vertical video"];
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
        if (s.live && s.stream_queued_ms > 1000) {
            [text appendFormat:@", stream %.1f s behind", s.stream_queued_ms / 1000.0];
            _attention = true;
        }
        if (s.reconnects) [text appendFormat:@", stream reconnected %u %@", s.reconnects, s.reconnects == 1 ? @"time" : @"times"];
        if (s.replay) [text appendFormat:@", replay %d s", static_cast<int>(s.replay_seconds)];
        detail = text;
    }
    if (_note && _noteUntil.timeIntervalSinceNow > 0) {
        detail = _note;
        _attention = _noteAttention;
    }
    if (![_status.stringValue isEqualToString:status]) _status.stringValue = status;
    if (![_detail.stringValue isEqualToString:detail]) _detail.stringValue = detail;
    _status.textColor = s.state == State::recording ? NSColor.systemRedColor
                      : s.state == State::paused    ? NSColor.systemOrangeColor
                                                    : NSColor.labelColor;
    _detail.textColor = _attention ? NSColor.systemOrangeColor : NSColor.secondaryLabelColor;
    NSString* live = !active ? @""
                   : s.live && s.servers > 1 ? [NSString stringWithFormat:@"● LIVE %u/%u  %.1f Mb/s", s.servers_live, s.servers, s.stream_kbps / 1000.0]
                   : s.live ? [NSString stringWithFormat:@"● LIVE  %.1f Mb/s", s.stream_kbps / 1000.0]
                   : s.reconnecting ? @"Reconnecting…" : @"";
    if (![_live.stringValue isEqualToString:live]) _live.stringValue = live;
    // Hide the last level once recording ends; polling stops with it.
    if (!active) _systemShown = _micShown = 0;
    [self meter:_systemMeter peak:s.system_peak shown:_systemShown];
    [self meter:_micMeter peak:s.mic_peak shown:_micShown];

    _record.title = active ? @"Stop Recording" : @"Start Recording";
    _pause.title = s.state == State::paused ? @"Resume" : @"Pause";
    _record.enabled = s.state == State::idle || active;
    _pause.enabled = active;
    _saveReplay.enabled = active && s.replay;
    const bool editable = s.state == State::idle;
    for (NSControl* control in @[_source, _systemAudio, _microphone, _cursor, _clicks, _resolution, _fps, _codec, _format,
                                 _stream, _bitrate, _replay, _replayLength, _changeFolder, _addDestination, _verticalMovie])
        control.enabled = editable;
    for (std::size_t i = 0; i < kMaxDestinations; ++i) {
        for (NSControl* control in @[_destOn[i], _destService[i], _destVertical[i], _destServer[i], _destKey[i]]) control.enabled = editable;
        _destRemove[i].enabled = editable && _settings.destinations.size() > 1;
    }
    _micDevice.enabled = editable && _microphone.state == NSControlStateValueOn;
    // Volume and mute change during a recording that has the source.
    for (NSControl* control in @[_systemVolume, _systemMute])
        control.enabled = editable ? _systemAudio.state == NSControlStateValueOn : active && s.system_audio;
    for (NSControl* control in @[_micVolume, _micMute])
        control.enabled = editable ? _microphone.state == NSControlStateValueOn : active && s.microphone;
    // Sources are fixed while recording; scenes and where they put the sources are not.
    for (NSControl* control in @[_camera, _image, _chooseImage, _addDisplay, _addWindow]) control.enabled = editable;
    _cameraDevice.enabled = editable && _camera.state == NSControlStateValueOn;
    const bool arranging = editable || active;
    for (NSControl* control in @[_scene, _newScene, _sceneName, _sourceShown]) control.enabled = arranging;
    _deleteScene.enabled = arranging && _settings.scenes.size() > 1;
    _removeSource.enabled = editable && _selected.kind == rec::app::SourceRef::Kind::layer;
    _corner.enabled = arranging && _settings.camera;
    // A dragged camera is sized by its corners.
    _cameraSize.enabled = arranging && _settings.camera && _corner.indexOfSelectedItem != choice_index(kCorners, "custom");
    // The camera's look changes while recording, in every scene.
    for (NSControl* control in @[_cameraShape, _cameraBorder, _shadow]) control.enabled = arranging && _settings.camera;
    _roundness.enabled = arranging && _settings.camera && rec::core::uses_roundness(_settings.camera_style.shape);
    // The vertical video's layout changes with the scene, also while recording.
    const bool vertical = active ? s.vertical : rec::app::uses_vertical(_settings);
    _verticalLayout.enabled = arranging && vertical;
    if (vertical != _verticalShown) {
        _verticalShown = vertical;
        _frame2 = {};
        _preview2.layer.contents = nil;
        _preview2.hidden = !vertical;
    }
    _shownState = s.state;
    _showLast.enabled = !_settings.last_recording.empty();

    // Preview frames are made only while the window is on screen: the recording's, or before
    // recording the idle preview's when Show preview is on. Capturing the screen before
    // Screen Recording is allowed would ask for it, so the preview waits for the permission.
    const bool shown = _window.visible && !_window.miniaturized && (_window.occlusionState & NSWindowOcclusionStateVisible);
    const bool wanted = _showPreview.state == NSControlStateValueOn;
    const bool allowed = s.state != State::idle || _testPattern || CGPreflightScreenCaptureAccess();
    const bool before = s.state == State::idle && !_quitting && shown && wanted && allowed;
    if (before) {
        [self readSettings];
        _session->idle_preview(&_settings, _testPattern);
    } else {
        _session->idle_preview(nullptr);
    }
    const bool previewing = shown && (active || (before && s.previewing));
    if (previewing != _previewing) {
        _session->show_preview(previewing);
        if (previewing) {
            _previewTimer = [NSTimer timerWithTimeInterval:1.0 / 30 target:self selector:@selector(showPreview:) userInfo:nil repeats:YES];
            [NSRunLoop.mainRunLoop addTimer:_previewTimer forMode:NSRunLoopCommonModes];
        } else {
            [_previewTimer invalidate];
            _previewTimer = nil;
        }
        _previewing = previewing;
    }
    if (!previewing && (_frame.number || _frame2.number)) {
        _frame = {};
        _frame2 = {};
        _preview.layer.contents = nil;
        _preview2.layer.contents = nil;
        _previewHint.hidden = NO;
    }
    NSString* hint = s.state != State::idle ? @"Starting the preview…"
                   : !wanted ? @"The preview is off"
                   : !allowed ? @"Allow Screen Recording to see the preview"
                   : !s.preview_error.empty() ? [@"No preview: " stringByAppendingString:ns(s.preview_error)]
                   : @"Starting the preview…";
    if (![_previewHint.stringValue isEqualToString:hint]) _previewHint.stringValue = hint;

    [self showOutline];

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
        // A stack of every thread shows where a wait that never ended was stuck.
        const std::string sample = "/usr/bin/sample " + std::to_string(getpid()) + " 2 -file '" +
                                   std::string(reportPath.UTF8String) + ".sample.txt' >/dev/null 2>&1";
        std::system(sample.c_str());
        std::_Exit(1);
    };    const auto ui = [](void (^block)(void)) { dispatch_sync(dispatch_get_main_queue(), block); };
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

    // The keep-recording step spans launches (app-e2e.sh): "keep" starts a recording and quits
    // with Keep Recording After Quit on; "attach" finds it still running, its preview coming
    // from the background engine (the test camera top right), and stops it.
    const std::string phase = environment("REC_APP_SELF_TEST_PHASE");
    if (!phase.empty()) {
        const auto seconds = [&](double at) {
            for (int i = 0; i < 2000; ++i) {
                if (self->_session->status().seconds >= at) return;
                usleep(10000);
            }
            fail([NSString stringWithFormat:@"the recording did not reach %.1f s", at]);
        };
        if (phase == "keep") {
            wait_for(@"Ready to record", 20);
            press(_record);
            wait_for(recording, 20);
            seconds(2);
            note(@"keep=started");
        } else {
            wait_for(recording, 20);
            bool green = false;
            for (int i = 0; i < 150 && !green; ++i) {
                __block rec::PreviewFrame shot;
                ui(^{ shot = self->_frame; });
                if (shot.number) {
                    long r = 0, g = 0, b = 0;
                    for (int x = 0; x < 10; ++x)
                        for (int y = 0; y < 10; ++y) {
                            const auto px = static_cast<std::size_t>((0.72 + 0.22 * (x + 0.5) / 10) * shot.width);
                            const auto py = static_cast<std::size_t>((0.08 + 0.22 * (y + 0.5) / 10) * shot.height);
                            const std::uint8_t* p = shot.pixels.data() + (py * shot.width + px) * 4;
                            b += p[0]; g += p[1]; r += p[2];
                        }
                    green = g / 100 > 120 && g > r + 6000 && g > b + 6000;
                }
                if (!green) usleep(100000);
            }
            if (!green) fail(@"keep: the preview never showed the engine's camera");
            note(@"keep-preview=ok");
            seconds(6);
            press(_record);
            wait_for(@"Ready to record", 30);
            saved(@"keep");
        }
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
        return;
    }
    // Every control has an accessible name.
    __block NSString* unnamed = nil;
    ui(^{
        for (NSControl* control in @[self->_record, self->_pause, self->_saveReplay, self->_replay, self->_replayLength,
                                     self->_camera, self->_cameraDevice, self->_corner, self->_cameraSize, self->_scene,
                                     self->_newScene, self->_deleteScene, self->_sceneName, self->_sourcesTable,
                                     self->_sourceShown, self->_addDisplay, self->_addWindow, self->_removeSource,
                                     self->_image, self->_imageFile, self->_chooseImage, self->_source, self->_systemAudio, self->_microphone,
                                     self->_micDevice, self->_cursor, self->_clicks, self->_resolution, self->_fps, self->_codec,
                                     self->_format, self->_stream, self->_destOn[0], self->_destService[0], self->_destServer[0],
                                     self->_destKey[0], self->_destVertical[0], self->_destRemove[0], self->_addDestination,
                                     self->_verticalMovie, self->_verticalLayout,
                                     self->_bitrate, self->_changeFolder, self->_openFolder, self->_showLast,
                                     self->_systemMeter, self->_micMeter, self->_systemVolume, self->_micVolume,
                                     self->_systemMute, self->_micMute]) {
            NSString* name = control.accessibilityLabel.length ? control.accessibilityLabel : control.accessibilityTitle;
            // Buttons are named by their title.
            if (!name.length && [control isKindOfClass:NSButton.class]) name = ((NSButton*)control).title;
            if (!name.length) unnamed = control.className;
        }
    });
    if (unnamed) fail([@"a control has no accessible name: " stringByAppendingString:unnamed]);
    note(@"accessibility=ok");
    wait_for(@"Ready to record", 20);
    // The space between the controls and the window's edges, reported for the layout check.
    __block NSString* margins = nil;
    __block bool tight = false;
    ui(^{
        NSView* content = self->_window.contentView;
        NSRect used = NSZeroRect;
        NSMutableArray<NSString*>* edges = [NSMutableArray array];
        NSMutableArray<NSView*>* pending = [content.subviews mutableCopy];
        while (pending.count) {
            NSView* view = pending.lastObject;
            [pending removeLastObject];
            if (view.hiddenOrHasHiddenAncestor) continue;
            // A table scrolls inside its clip view; the scroll view's own frame is what shows.
            if ([view isKindOfClass:NSClipView.class]) continue;
            if (![view isKindOfClass:NSControl.class]) { [pending addObjectsFromArray:view.subviews]; continue; }
            if (NSIsEmptyRect(view.frame)) continue;
            const NSRect at = [view.superview convertRect:view.frame toView:content];
            used = NSIsEmptyRect(used) ? at : NSUnionRect(used, at);
            const CGFloat low = content.isFlipped ? NSMaxY(content.bounds) - NSMaxY(at) : NSMinY(at);
            if (edges.count < 4 && (NSMinX(at) < 12 || NSMaxX(content.bounds) - NSMaxX(at) < 12 || low < 12))
                [edges addObject:[NSString stringWithFormat:@"%@ '%@' in %@ %.0f,%.0f %.0fx%.0f", view.className,
                                  ((NSControl*)view).accessibilityLabel ?: @"", view.superview.className,
                                  at.origin.x, at.origin.y, at.size.width, at.size.height]];
        }
        const NSRect box = content.bounds;
        const CGFloat left = NSMinX(used), right = NSMaxX(box) - NSMaxX(used);
        const CGFloat bottom = content.isFlipped ? NSMaxY(box) - NSMaxY(used) : NSMinY(used);
        const NSSize fitting = content.subviews.firstObject.fittingSize;
        const NSRect screen = self->_window.screen.visibleFrame;
        margins = [NSString stringWithFormat:@"left %.1f right %.1f bottom %.1f; content %.0fx%.0f, fitting %.0fx%.0f, screen %.0fx%.0f",
                   left, right, bottom, box.size.width, box.size.height, fitting.width, fitting.height,
                   screen.size.width, screen.size.height];
        if (edges.count) margins = [margins stringByAppendingFormat:@"; at the edge: %@", [edges componentsJoinedByString:@", "]];
        // Where each layout stack sits (y from the bottom, height, fitting height), to find
        // which one outgrows the window.
        NSStackView* stack = (NSStackView*)content.subviews.firstObject;
        NSStackView* body = (NSStackView*)stack.arrangedSubviews.firstObject;
        NSStackView* columns = (NSStackView*)body.arrangedSubviews.lastObject;
        NSMutableArray<NSString*>* frames = [NSMutableArray array];
        const auto frame = [&](NSString* name, NSView* view) {
            const NSRect at = [view.superview convertRect:view.frame toView:content];
            const CGFloat y = content.isFlipped ? NSMaxY(box) - NSMaxY(at) : NSMinY(at);
            [frames addObject:[NSString stringWithFormat:@"%@ y %.0f h %.0f fit %.0f%@", name, y, at.size.height,
                               view.fittingSize.height, view.hasAmbiguousLayout ? @" ambiguous" : @""]];
        };
        frame(@"stack", stack);
        frame(@"body", body);
        frame(@"top", body.arrangedSubviews.firstObject);
        frame(@"left", columns.arrangedSubviews.firstObject);
        frame(@"right", columns.arrangedSubviews.lastObject);
        frame(@"scenes", stack.arrangedSubviews.lastObject);
        frame(@"folder-buttons", self->_changeFolder.superview);
        frame(@"detail", self->_detail);
        margins = [margins stringByAppendingFormat:@"; frames: %@; window %.0fx%.0f", [frames componentsJoinedByString:@", "],
                   self->_window.frame.size.width, self->_window.frame.size.height];
        tight = left < 12 || right < 12 || bottom < 12;
    });
    note([@"margins=" stringByAppendingString:margins]);
    // Push buttons' frames include about 6 pt of bezel around the visible button.
    if (tight) fail([@"controls are too close to the window's edge: " stringByAppendingString:margins]);
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
        // The same stream also goes to a second server with its own key.
        const std::string server2 = server + "2";
        const auto custom = [&](std::size_t i, const std::string& url, NSString* key) {
            ui(^{
                [self->_destService[i] selectItemAtIndex:static_cast<NSInteger>(kServices.size()) - 1];
                [self->_destService[i] sendAction:self->_destService[i].action to:self->_destService[i].target];
                self->_destServer[i].stringValue = ns(url);
                self->_destKey[i].stringValue = key;
            });
        };
        custom(0, server, @"appkey");
        ui(^{ self->_bitrate.stringValue = @"3000"; });
        press(_addDestination);
        custom(1, server2, @"appkey2");
        // A third destination, turned off, needs no key and gets nothing.
        press(_addDestination);
        __block std::string third;
        __block NSString* names;
        ui(^{
            third = self->_settings.destinations.size() == 3 ? self->_settings.destinations[2].service : "";
            names = [NSString stringWithFormat:@"%@|%@", self->_destVertical[1].accessibilityLabel, self->_destKey[0].accessibilityLabel];
        });
        if (third != "twitch") fail(@"Add Destination did not add Twitch third");
        if (![names isEqualToString:@"Send the vertical video to Custom 2|Custom stream key"])
            fail([@"destination names: " stringByAppendingString:names]);
        set_check(_destOn[2], false);
        // The second server gets the vertical video, which is also saved beside the movie.
        set_check(_destVertical[1], true);
        set_check(_verticalMovie, true);
        press(_record);
        wait_for(recording, 20);
        bool live = false;
        for (int i = 0; i < 100 && !live; ++i) {
            __block BOOL on = NO;
            ui(^{ on = [self->_live.stringValue hasPrefix:@"● LIVE"]; });
            live = on;
            if (!live) usleep(100000);
        }
        if (!live) fail([@"no LIVE badge: " stringByAppendingString:detail()]);
        sleep(3);
        // The badge shows the measured send rate.
        __block NSString* badge;
        ui(^{ badge = self->_live.stringValue; });
        NSScanner* scanner = [NSScanner scannerWithString:badge];
        double rate = 0;
        if (![scanner scanString:@"● LIVE 2/2" intoString:nil] || ![scanner scanDouble:&rate] || rate <= 0)
            fail([@"no stream rate for both servers in the LIVE badge: " stringByAppendingString:badge]);
        note([@"live=" stringByAppendingString:badge]);
        __block rec::PreviewFrame vertical;
        ui(^{ vertical = self->_frame2; });
        if (!vertical.number || vertical.width >= vertical.height)
            fail([NSString stringWithFormat:@"no vertical preview (%ux%u)", vertical.width, vertical.height]);
        sleep(1);
        press(_record);
        wait_for(@"Ready to record", 30);
        if ([detail() hasPrefix:@"Stream stopped early"]) fail(detail());
        if (![detail() hasSuffix:@"and its vertical video"]) fail([@"the vertical video was not reported: " stringByAppendingString:detail()]);
        saved(@"stream");
        __block NSString* verticalPath;
        ui(^{ verticalPath = ns(self->_session->status().saved_vertical); });
        note([@"stream-vertical=" stringByAppendingString:verticalPath]);
        set_check(_destVertical[1], false);
        set_check(_verticalMovie, false);
        press(_destRemove[2]);
        __block std::size_t left = 0;
        ui(^{ left = self->_settings.destinations.size(); });
        if (left != 2) fail(@"Remove did not remove the third destination");
        const std::string stored = load_key(server), stored2 = load_key(server2);
        delete_key(server);
        delete_key(server2);
        if (stored != "appkey" || stored2 != "appkey2") fail(@"the stream keys were not stored in the keychain");
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

    // 5. With the replay buffer on, Save Replay writes the last seconds while recording goes on.
    ui(^{ [self showWindow:nil]; });
    set_check(_replay, true);
    press(_record);
    wait_for(recording, 20);
    sleep(5);
    press(_saveReplay);
    if (![detail() hasPrefix:@"Saved replay"]) fail([@"Save Replay: " stringByAppendingString:detail()]);
    {
        __block NSString* path;
        ui(^{ path = ns(self->_settings.last_recording); });
        note([@"replay=" stringByAppendingString:path]);
    }
    sleep(1);
    press(_record);
    wait_for(@"Ready to record", 30);
    saved(@"replay-recording");

    // 6. A camera and an image: the test camera in the corner, then full frame from the Scene
    // list, then hidden from the menu bar item's menu, all during one recording. The image is a
    // transparent 1080p PNG with a red block in its top-left corner, chosen as Choose… would.
    NSString* imagePath = [ns(environment("REC_APP_HOME")) stringByAppendingPathComponent:@"overlay.png"];
    {
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:1920 pixelsHigh:1080
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
            bytesPerRow:1920 * 4 bitsPerPixel:32];
        std::memset(rep.bitmapData, 0, 1920 * 4 * 1080);
        for (int y = 0; y < 216; ++y)
            for (int x = 0; x < 384; ++x) {
                unsigned char* p = rep.bitmapData + y * 1920 * 4 + x * 4;
                p[0] = 220; p[1] = 30; p[2] = 30; p[3] = 255;
            }
        if (![[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:imagePath atomically:YES])
            fail(@"could not write the overlay image");
    }
    set_check(_replay, false);
    ui(^{
        self->_settings.image_path = utf8(imagePath);
        [self showImage];
        for (std::size_t i = 0; i < self->_cameras.size(); ++i)
            if (self->_cameras[i].id == "test") [self->_cameraDevice selectItemAtIndex:static_cast<NSInteger>(i)];
    });
    set_check(_camera, true);
    set_check(_image, true);
    const auto scene = [&](NSInteger index) {
        ui(^{
            [self->_scene selectItemAtIndex:index];
            [self->_scene sendAction:self->_scene.action to:self->_scene.target];
        });
    };
    // The preview as the window shows it; only the copy happens on the main thread, so the
    // checks do not delay the steps.
    const auto preview = [&] {
        __block rec::PreviewFrame shot;
        ui(^{ shot = self->_frame; });
        if (!shot.number) fail(@"the preview shows nothing while recording");
        return shot;
    };
    // The frame's mean color over part of it (fractions of its size), as R,G,B.
    const auto mean_color = [](const rec::PreviewFrame& f, double x, double y, double w, double h) {
        long r = 0, g = 0, b = 0;
        for (int i = 0; i < 10; ++i)
            for (int j = 0; j < 10; ++j) {
                const auto px = static_cast<std::size_t>((x + w * (i + 0.5) / 10) * f.width);
                const auto py = static_cast<std::size_t>((y + h * (j + 0.5) / 10) * f.height);
                const std::uint8_t* p = f.pixels.data() + (py * f.width + px) * 4;
                b += p[0]; g += p[1]; r += p[2];
            }
        return std::array<long, 3>{r / 100, g / 100, b / 100};
    };
    const auto is_color = [](const std::array<long, 3>& c, NSString* want) {
        const auto [r, g, b] = c;
        if ([want isEqualToString:@"black"]) return r < 40 && g < 40 && b < 40;
        return [want isEqualToString:@"green"] ? g > 120 && g > r + 60 && g > b + 60
                                               : r > 170 && r > g + 100 && r > b + 100;
    };
    const auto preview_color = [&](const rec::PreviewFrame& f, NSString* name, NSString* want, double x, double y, double w, double h) {
        const auto c = mean_color(f, x, y, w, h);
        if (!is_color(c, want)) fail([NSString stringWithFormat:@"%@: the preview is %ld,%ld,%ld, not %@", name, c[0], c[1], c[2], want]);
    };
    NSString* artifacts = [reportPath stringByDeletingLastPathComponent];
    const auto save_png = [&](const rec::PreviewFrame& f, NSString* name) {
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:f.width
            pixelsHigh:f.height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
            colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:f.width * 4 bitsPerPixel:32];
        for (std::size_t k = 0; k < f.pixels.size(); k += 4) {
            rep.bitmapData[k] = f.pixels[k + 2];
            rep.bitmapData[k + 1] = f.pixels[k + 1];
            rep.bitmapData[k + 2] = f.pixels[k];
            rep.bitmapData[k + 3] = 255;
        }
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
            writeToFile:[artifacts stringByAppendingPathComponent:[name stringByAppendingString:@".png"]] atomically:YES];
    };
    // Waits up to 15 s for the window's preview to show want there; returns that frame, or
    // saves the last one as NAME-failed.png and fails.
    const auto wait_preview = [&](NSString* name, NSString* want, double x, double y, double w, double h) {
        for (int i = 0; i < 150; ++i) {
            __block rec::PreviewFrame shot;
            ui(^{ shot = self->_frame; });
            if (shot.number && is_color(mean_color(shot, x, y, w, h), want)) return shot;
            usleep(100000);
        }
        __block rec::PreviewFrame last;
        ui(^{ last = self->_frame; });
        if (last.number) save_png(last, [name stringByAppendingString:@"-failed"]);
        fail([NSString stringWithFormat:@"%@: the preview never showed %@", name, want]);
        return rec::PreviewFrame{};
    };
    // Steps land at movie times the script checks, however slow the machine is.
    const auto recorded = [&](double seconds) {
        for (int i = 0; i < 1000; ++i) {
            if (self->_session->status().seconds >= seconds) return;
            usleep(10000);
        }
        fail([NSString stringWithFormat:@"the recording did not reach %.1f s", seconds]);
    };
    scene(0);
    // Before Start the preview already shows the scene; with Show preview off it stops.
    const auto idle = wait_preview(@"app-preview-idle", @"green", 0.76, 0.75, 0.18, 0.18);
    preview_color(idle, @"app-preview-idle", @"red", 0.02, 0.03, 0.15, 0.15);
    set_check(_showPreview, false);
    __block BOOL off = NO;
    for (int i = 0; i < 50 && !off; ++i) {
        ui(^{ off = !self->_frame.number && [self->_previewHint.stringValue isEqualToString:@"The preview is off"]; });
        if (!off) usleep(100000);
    }
    if (!off) fail(@"Show preview off left the preview on");
    set_check(_showPreview, true);
    wait_preview(@"app-preview-idle", @"green", 0.76, 0.75, 0.18, 0.18);
    note(@"idle-preview=ok");
    press(_record);
    wait_for(recording, 20);
    __block BOOL switchable = NO;
    ui(^{ switchable = self->_scene.enabled; });
    if (!switchable) fail(@"the Scene list is disabled while recording with a camera");
    recorded(2.0);
    const auto corner = preview();
    recorded(2.5);
    scene(2);  // Camera only
    recorded(4.0);
    const auto full = preview();
    // The preview: the camera in the bottom-right corner and the image's red block top left,
    // then the camera full frame.
    preview_color(corner, @"app-preview-corner", @"green", 0.76, 0.75, 0.18, 0.18);
    preview_color(corner, @"app-preview-corner", @"red", 0.02, 0.03, 0.15, 0.15);
    preview_color(full, @"app-preview-camera", @"green", 0.35, 0.4, 0.3, 0.2);
    note(@"preview=ok");
    recorded(5.0);
    __block BOOL hidden = NO;
    ui(^{
        NSMenu* menu = self->_statusItem.menu;
        [self menuNeedsUpdate:menu];
        const NSInteger at = [menu indexOfItemWithTitle:@"Screen only"];
        if (at >= 0) {
            [menu performActionForItemAtIndex:at];
            hidden = YES;
        }
    });
    if (!hidden) fail(@"the menu bar item's menu has no Screen only scene");
    recorded(7.5);
    press(_record);
    wait_for(@"Ready to record", 30);
    saved(@"camera");
    save_png(idle, @"app-preview-idle");
    save_png(corner, @"app-preview-corner");
    save_png(full, @"app-preview-camera");

    // 6b. Camera layout: before Start, drag the camera from its corner to the middle and its
    // top-left corner outwards; while recording, drag it to the top right. The events go to
    // the preview as the mouse would send them; points are fractions of the frame.
    const auto pointer = [&](NSEventType type, double x, double y) {
        ui(^{
            const NSRect r = [self previewFrameRect];
            const NSPoint at = [self->_preview convertPoint:NSMakePoint(r.origin.x + x * r.size.width,
                                                                        r.origin.y + (1 - y) * r.size.height)
                                                     toView:nil];
            NSEvent* event = [NSEvent mouseEventWithType:type location:at modifierFlags:0 timestamp:0
                                            windowNumber:self->_window.windowNumber context:nil eventNumber:0
                                              clickCount:1 pressure:1];
            if (type == NSEventTypeLeftMouseDown) [self->_preview mouseDown:event];
            else if (type == NSEventTypeLeftMouseDragged) [self->_preview mouseDragged:event];
            else [self->_preview mouseUp:event];
        });
    };
    const auto drag = [&](double x0, double y0, double x1, double y1) {
        pointer(NSEventTypeLeftMouseDown, x0, y0);
        for (int i = 1; i <= 10; ++i) {
            pointer(NSEventTypeLeftMouseDragged, x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10);
            usleep(20000);
        }
        pointer(NSEventTypeLeftMouseUp, x1, y1);
    };
    scene(0);
    wait_preview(@"app-layout", @"green", 0.76, 0.75, 0.18, 0.18);
    drag(0.855, 0.84, 0.5, 0.5);  // The camera's middle: it moves to 0.375,0.375
    wait_preview(@"app-layout", @"green", 0.42, 0.42, 0.16, 0.16);
    drag(0.376, 0.376, 0.3, 0.3);  // Its top-left corner: 0.3,0.3 to 0.625,0.625
    const auto resized = wait_preview(@"app-layout", @"green", 0.31, 0.31, 0.07, 0.07);
    __block BOOL sizable = YES;
    ui(^{ sizable = self->_cameraSize.enabled; });
    if (sizable) fail(@"Size stayed enabled for a dragged camera");
    press(_record);
    wait_for(recording, 20);
    recorded(2.0);
    drag(0.46, 0.46, 0.96, 0.21);  // To the right edge; the top snaps to the margin
    const auto moved = wait_preview(@"app-layout-moved", @"green", 0.72, 0.08, 0.22, 0.22);
    recorded(4.5);
    press(_record);
    wait_for(@"Ready to record", 30);
    saved(@"layout");
    save_png(resized, @"app-preview-layout");
    save_png(moved, @"app-preview-layout-moved");
    note(@"camera-layout=ok");

    // 6c. Camera look: before Start a circle with a thick black border and a drop shadow; while
    // recording, a rectangle again. The circle is the largest square in the camera's place
    // from 6b, against the frame's right edge. The script probes the movie where noted.
    const auto choose = [&](NSPopUpButton* popup, NSInteger index) {
        ui(^{
            [popup selectItemAtIndex:index];
            [popup sendAction:popup.action to:popup.target];
        });
    };
    const auto style_now = [&] {
        __block std::string text;
        ui(^{ text = rec::core::style_text(self->_settings.camera_style); });
        return text;
    };
    choose(_cameraShape, 2);  // Circle
    choose(_cameraBorder, 4);  // Thick black
    set_check(_shadow, true);
    __block BOOL roundable = YES;
    __block rec::Area square;
    ui(^{
        [self refresh];
        roundable = self->_roundness.enabled;
        square = rec::core::shaped_area(self->_settings.camera_style, rec::app::current_scene(self->_settings).camera.area, 1920, 1080);
    });
    if (roundable) fail(@"Roundness stayed enabled for a circle");
    if (style_now() != "circle,border=10:000000,shadow") fail([@"the camera style is " stringByAppendingString:ns(style_now())]);
    const double sx = square.x * 1920, sy = square.y * 1080, side = square.height * 1080;
    const double cx = sx + side / 2, cy = sy + side / 2;
    // Outside the circle, and below the rectangle's border once the shape changes.
    const double ox = sx + 6, oy = sy + 18;
    const auto rect = [](double x, double y, int w, int h) { return [NSString stringWithFormat:@"%d,%d,%d,%d", static_cast<int>(x), static_cast<int>(y), w, h]; };
    note([@"look-center=" stringByAppendingString:rect(cx - 20, cy - 20, 40, 40)]);
    note([@"look-corner=" stringByAppendingString:rect(sx + 4, sy + 16, 10, 10)]);
    note([@"look-border=" stringByAppendingString:rect(cx - 5, sy + 3, 10, 4)]);
    note([@"look-shadow=" stringByAppendingString:rect(cx - 10, sy + side + 2, 20, 6)]);
    const auto look = wait_preview(@"app-look", @"green", (cx - 20) / 1920, (cy - 20) / 1080, 40.0 / 1920, 40.0 / 1080);
    __block BOOL cut = NO;
    for (int i = 0; i < 150 && !cut; ++i) {
        __block rec::PreviewFrame shot;
        ui(^{ shot = self->_frame; });
        cut = shot.number && !is_color(mean_color(shot, ox / 1920, oy / 1080, 12.0 / 1920, 12.0 / 1080), @"green");
        if (!cut) usleep(100000);
    }
    if (!cut) fail(@"app-look: the preview never showed the circle's corner cut away");
    press(_record);
    wait_for(recording, 20);
    recorded(2.0);
    choose(_cameraShape, 0);  // Rectangle
    wait_preview(@"app-look-rectangle", @"green", ox / 1920, oy / 1080, 12.0 / 1920, 12.0 / 1080);
    recorded(5.0);
    press(_record);
    wait_for(@"Ready to record", 30);
    saved(@"look");
    save_png(look, @"app-preview-look");
    choose(_cameraBorder, 0);  // None
    set_check(_shadow, false);
    if (style_now() != "rectangle") fail([@"the camera style is " stringByAppendingString:ns(style_now())]);
    note(@"camera-look=ok");

    // 6d. Scenes: a new scene, renamed, puts the recorded screen in the bottom-right quarter by
    // dragging its top-left corner, and hides the camera with Show in this scene. Recording
    // starts in it and switches back to the first scene at 2 s.
    set_check(_image, false);  // It fills the frame, so it would take the corner.
    press(_newScene);
    __block NSString* sceneTitle;
    ui(^{
        self->_sceneName.stringValue = @"Corner screen";
        [self sceneNameChanged];
        sceneTitle = self->_scene.titleOfSelectedItem;
    });
    if (![sceneTitle isEqualToString:@"Corner screen"]) fail([@"the Scene list shows " stringByAppendingString:sceneTitle ?: @"nothing"]);
    wait_preview(@"app-scenes", @"green", 0.72, 0.08, 0.22, 0.22);  // The camera, top right as in the first scene.
    drag(0.004, 0.006, 0.5, 0.5);
    wait_preview(@"app-scenes", @"black", 0.1, 0.1, 0.3, 0.3);
    ui(^{
        const auto sources = rec::app::scene_sources(self->_settings);
        for (std::size_t i = 0; i < sources.size(); ++i)
            if (sources[i].kind == rec::app::SourceRef::Kind::camera)
                [self->_sourcesTable selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
    });
    set_check(_sourceShown, false);
    const auto arranged = wait_preview(@"app-scenes", @"black", 0.72, 0.08, 0.22, 0.22);
    press(_record);
    wait_for(recording, 20);
    recorded(2.0);
    scene(0);
    const auto switched = wait_preview(@"app-scenes-switched", @"green", 0.72, 0.08, 0.22, 0.22);
    recorded(4.5);
    press(_record);
    wait_for(@"Ready to record", 30);
    saved(@"scenes");
    save_png(arranged, @"app-preview-scenes");
    save_png(switched, @"app-preview-scenes-switched");
    note(@"scene-editor=ok");

    // 7. Volume and mute: system audio at -6 dB from the slider, muted with its box 2.5 s in,
    // unmuted from the menu bar item's menu 4.5 s in. The mix loses the beeps at 3 and 4 s.
    set_check(_microphone, true);
    ui(^{
        self->_systemVolume.integerValue = -6;
        [self->_systemVolume sendAction:self->_systemVolume.action to:self->_systemVolume.target];
    });
    press(_record);
    wait_for(recording, 20);
    usleep(2500000);
    set_check(_systemMute, true);
    usleep(2000000);
    __block BOOL unmuted = NO;
    ui(^{
        NSMenu* menu = self->_statusItem.menu;
        [self menuNeedsUpdate:menu];
        const NSInteger at = [menu indexOfItemWithTitle:@"Mute System Audio"];
        if (at >= 0 && [menu itemAtIndex:at].state == NSControlStateValueOn) {
            [menu performActionForItemAtIndex:at];
            unmuted = YES;
        }
    });
    if (!unmuted) fail(@"the menu bar item's menu has no checked Mute System Audio");
    usleep(2000000);
    press(_record);
    wait_for(@"Ready to record", 30);
    saved(@"mute");

    // 8. Quitting during a recording saves the movie, then the app exits.
    press(_record);
    wait_for(recording, 20);
    sleep(2);
    note(@"quit=started");
    dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
}
@end

namespace {
OSStatus on_hotkey(EventHandlerCallRef, EventRef event, void* context) {
    EventHotKeyID id{};
    GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof id, nullptr, &id);
    if (id.id == 2) [(__bridge RecController*)context saveReplay:nil];
    else [(__bridge RecController*)context toggleRecord:nil];
    return noErr;
}
} // namespace

namespace {
std::atomic_bool engine_interrupted{false};
extern "C" void on_engine_signal(int) { engine_interrupted.store(true); }
} // namespace

int main(int argc, const char* argv[]) {
    // `Rec --engine`: the background engine that hosts the app's recordings, so they can
    // outlive it (cli/engine.hpp). It exits after a minute without a recording or a client,
    // and saves its recording when logging out sends SIGTERM.
    if (argc == 2 && std::strcmp(argv[1], "--engine") == 0) {
        std::signal(SIGINT, on_engine_signal);
        std::signal(SIGTERM, on_engine_signal);
        return rec::cli::serve(rec::cli::engine_socket(), engine_interrupted, 60);
    }
    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        RecController* controller = [[RecController alloc] init];
        app.delegate = controller;
        [app run];
    }
    return 0;
}
