#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "platform/macos/overlay.hpp"
#include "platform/macos/screen.h"
#include "core/clicks.hpp"
#include "core/overlay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace rec::mac {
class CameraFeed;
class ScreenFeed;
void receive_camera(CameraFeed* feed, CMSampleBufferRef sample);
void receive_screen(ScreenFeed* feed, CMSampleBufferRef sample);
}

@interface RecCameraOutput : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, assign) rec::mac::CameraFeed* feed;
@end

@implementation RecCameraOutput
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)buffer
           fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    if (self.feed) rec::mac::receive_camera(self.feed, buffer);
}
@end

@interface RecScreenOutput : NSObject <SCStreamOutput>
@property(nonatomic, assign) rec::mac::ScreenFeed* feed;
@end

@implementation RecScreenOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)buffer ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type == SCStreamOutputTypeScreen && self.feed) rec::mac::receive_screen(self.feed, buffer);
}
@end

namespace rec::mac {
namespace {
NSArray<AVCaptureDevice*>* camera_devices() {
    NSMutableArray<AVCaptureDeviceType>* types = [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
    if (@available(macOS 14.0, *)) {
        [types addObject:AVCaptureDeviceTypeExternal];
        [types addObject:AVCaptureDeviceTypeContinuityCamera];
    }
    return [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                                  mediaType:AVMediaTypeVideo
                                                                   position:AVCaptureDevicePositionUnspecified].devices;
}

Result camera_access() {
    const auto status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (status == AVAuthorizationStatusAuthorized) return {};
    if (status == AVAuthorizationStatusNotDetermined) {
        __block BOOL granted = NO;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL allowed) {
            granted = allowed;
            dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC));
        if (granted) return {};
    }
    return {"Camera permission is off. Turn it on for this app in System Settings > Privacy & Security > Camera."};
}

std::string text(NSString* value) { return value ? std::string(value.UTF8String ?: "") : std::string(); }

// BT.709 tags, so Core Image converts the capture and the composite with the same matrix.
void tag_709(CVPixelBufferRef pixel) {
    CVBufferSetAttachment(pixel, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixel, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixel, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
}
} // namespace

std::vector<Camera> cameras() {
    @autoreleasepool {
        std::vector<Camera> result;
        for (AVCaptureDevice* device in camera_devices()) result.push_back({text(device.uniqueID), text(device.localizedName)});
        return result;
    }
}

// The newest frame of one camera. A device camera runs an AVCaptureSession that delivers
// BGRA on its own queue; the test camera draws core::test_camera_frame at 30 fps.
class CameraFeed {
public:
    ~CameraFeed() { stop(); }

    Result open(const std::string& wanted) {
        if (wanted == "test") {
            NSDictionary* attributes = @{
                (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                (id)kCVPixelBufferWidthKey: @(core::kTestCameraWidth),
                (id)kCVPixelBufferHeightKey: @(core::kTestCameraHeight),
                (id)kCVPixelBufferIOSurfacePropertiesKey: @{}
            };
            if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes, &pool_) != kCVReturnSuccess)
                return {"could not allocate the test camera"};
            return {};
        }
        if (auto allowed = camera_access(); !allowed) return allowed;
        NSArray<AVCaptureDevice*>* devices = camera_devices();
        AVCaptureDevice* device = nil;
        if (wanted == "default") {
            device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo] ?: devices.firstObject;
        } else {
            NSString* needle = [NSString stringWithUTF8String:wanted.c_str()];
            for (AVCaptureDevice* candidate in devices)
                if ([candidate.uniqueID isEqualToString:needle]) device = candidate;
            if (!device && needle)
                for (AVCaptureDevice* candidate in devices)
                    if ([candidate.localizedName rangeOfString:needle options:NSCaseInsensitiveSearch].location != NSNotFound) {
                        device = candidate;
                        break;
                    }
        }
        if (!device) return {devices.count ? "camera not found: " + wanted + "; run `rec devices`" : std::string("no camera found")};
        name_ = text(device.localizedName);
        session_ = [[AVCaptureSession alloc] init];
        [session_ beginConfiguration];
        for (AVCaptureSessionPreset preset in @[AVCaptureSessionPreset1920x1080, AVCaptureSessionPreset1280x720, AVCaptureSessionPresetHigh])
            if ([session_ canSetSessionPreset:preset]) { session_.sessionPreset = preset; break; }
        NSError* error = nil;
        AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
        if (!input || ![session_ canAddInput:input]) {
            [session_ commitConfiguration];
            return {"could not open the camera " + name_ + (error ? ": " + text(error.localizedDescription) : std::string())};
        }
        [session_ addInput:input];
        output_ = [[AVCaptureVideoDataOutput alloc] init];
        output_.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
        output_.alwaysDiscardsLateVideoFrames = YES;
        queue_ = dispatch_queue_create("rec.camera", DISPATCH_QUEUE_SERIAL);
        receiver_ = [[RecCameraOutput alloc] init];
        receiver_.feed = this;
        [output_ setSampleBufferDelegate:receiver_ queue:queue_];
        if (![session_ canAddOutput:output_]) {
            [session_ commitConfiguration];
            return {"the camera " + name_ + " cannot deliver video"};
        }
        [session_ addOutput:output_];
        [session_ commitConfiguration];
        return {};
    }

    void start() {
        if (session_) [session_ startRunning];
        else if (pool_) { stop_ = false; thread_ = std::thread([this] { generate(); }); }
    }

    void stop() {
        if (session_) {
            [session_ stopRunning];
            [output_ setSampleBufferDelegate:nil queue:nil];
            // Let a callback already on the queue finish before the feed goes away.
            if (queue_) dispatch_sync(queue_, ^{});
            receiver_.feed = nullptr;
            session_ = nil;
            output_ = nil;
            receiver_ = nil;
            queue_ = nil;
        }
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        std::lock_guard lock(mutex_);
        if (latest_) { CVPixelBufferRelease(latest_); latest_ = nullptr; }
        if (pool_) { CVPixelBufferPoolRelease(pool_); pool_ = nullptr; }
    }

    void receive(CVPixelBufferRef pixel) {
        if (!pixel) return;
        CVPixelBufferRetain(pixel);
        {
            std::lock_guard lock(mutex_);
            if (latest_) CVPixelBufferRelease(latest_);
            latest_ = pixel;
        }
        frames_.fetch_add(1);
    }

    // Retained; the caller releases it.
    CVPixelBufferRef latest() {
        std::lock_guard lock(mutex_);
        if (latest_) CVPixelBufferRetain(latest_);
        return latest_;
    }

    std::uint64_t frames() const { return frames_.load(); }

private:
    void generate() {
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint64_t n = 0; !stop_; ++n) {
            CVPixelBufferRef pixel = nullptr;
            if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool_, &pixel) == kCVReturnSuccess) {
                CVPixelBufferLockBaseAddress(pixel, 0);
                core::test_camera_frame(n, static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pixel)),
                                        CVPixelBufferGetBytesPerRow(pixel));
                CVPixelBufferUnlockBaseAddress(pixel, 0);
                receive(pixel);
                CVPixelBufferRelease(pixel);
            }
            std::this_thread::sleep_until(begin + std::chrono::microseconds((n + 1) * 1000000 / core::kTestCameraFps));
        }
    }

    std::string name_;
    AVCaptureSession* __strong session_ = nil;
    AVCaptureVideoDataOutput* __strong output_ = nil;
    RecCameraOutput* __strong receiver_ = nil;
    dispatch_queue_t queue_ = nullptr;
    CVPixelBufferPoolRef pool_ = nullptr;  // The test camera's frames.
    std::mutex mutex_;
    CVPixelBufferRef latest_ = nullptr;
    std::atomic<std::uint64_t> frames_{0};
    std::atomic_bool stop_{false};
    std::thread thread_;
};

void receive_camera(CameraFeed* feed, CMSampleBufferRef sample) { feed->receive(CMSampleBufferGetImageBuffer(sample)); }

// The newest frame of a display or window, from its own ScreenCaptureKit stream in BGRA.
// The callback only retains the newest complete frame.
class ScreenFeed {
public:
    ~ScreenFeed() { stop(); }

    Result open(const Overlay& overlay, bool cursor, std::uint32_t fps) {
        std::string error;
        SCShareableContent* content = shareable(error);
        if (!content) return {error};
        const bool window = overlay.kind == Overlay::Kind::window;
        const auto name = window ? std::string("the window") : "display " + std::to_string(overlay.display);
        SCContentFilter* filter = nil;
        SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
        double width = 0, height = 0;
        if (window) {
            SCWindow* found = nil;
            for (SCWindow* candidate in content.windows)
                if (candidate.windowID == overlay.window) { found = candidate; break; }
            if (!found) return {"window not found; run `rec windows`"};
            if (!found.onScreen) return {"the window is not on screen; show it (not minimized, on this Space), then retry"};
            const double scale = pixels_per_point(content, found.frame);
            width = found.frame.size.width * scale;
            height = found.frame.size.height * scale;
            filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:found];
        } else {
            if (overlay.display == 0 || overlay.display > content.displays.count) return {name + " not found; run `rec devices`"};
            SCDisplay* display = content.displays[overlay.display - 1];
            const auto native = native_size(display);
            width = native.width;
            height = native.height;
            if (overlay.region) {
                const auto& r = *overlay.region;
                if (r.x + static_cast<std::int64_t>(r.width) > native.width || r.y + static_cast<std::int64_t>(r.height) > native.height)
                    return {"the region does not fit " + name + " (" + std::to_string(native.width) + "x" +
                            std::to_string(native.height) + ")"};
                // The source rectangle is in points; the region is in pixels.
                const double scale = display.width > 0 ? static_cast<double>(native.width) / display.width : 1.0;
                config.sourceRect = CGRectMake(r.x / scale, r.y / scale, r.width / scale, r.height / scale);
                width = r.width;
                height = r.height;
            }
            filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        }
        if (width < 2 || height < 2) return {name + " has no usable size"};
        config.width = static_cast<size_t>(std::lround(width)) & ~static_cast<size_t>(1);
        config.height = static_cast<size_t>(std::lround(height)) & ~static_cast<size_t>(1);
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.minimumFrameInterval = CMTimeMake(1, static_cast<int32_t>(std::clamp<std::uint32_t>(fps, 1, 60)));
        config.queueDepth = 3;
        config.showsCursor = cursor;
        config.scalesToFit = YES;
        config.preservesAspectRatio = YES;
        receiver_ = [[RecScreenOutput alloc] init];
        receiver_.feed = this;
        queue_ = dispatch_queue_create("rec.overlay.screen", DISPATCH_QUEUE_SERIAL);
        stream_ = [[SCStream alloc] initWithFilter:filter configuration:config delegate:nil];
        NSError* added = nil;
        if (![stream_ addStreamOutput:receiver_ type:SCStreamOutputTypeScreen sampleHandlerQueue:queue_ error:&added]) {
            stream_ = nil;
            return {message(added, ("could not capture " + name).c_str())};
        }
        return {};
    }

    Result start() {
        if (auto started = wait_capture(stream_, true); !started) return started;
        running_ = true;
        return {};
    }

    void stop() {
        if (stream_) {
            if (running_) wait_capture(stream_, false);
            [stream_ removeStreamOutput:receiver_ type:SCStreamOutputTypeScreen error:nil];
            // Let a callback already on the queue finish before the feed goes away.
            if (queue_) dispatch_sync(queue_, ^{});
            receiver_.feed = nullptr;
            stream_ = nil;
            receiver_ = nil;
            queue_ = nil;
            running_ = false;
        }
        std::lock_guard lock(mutex_);
        if (latest_) { CVPixelBufferRelease(latest_); latest_ = nullptr; }
    }

    void receive(CMSampleBufferRef sample) {
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (attachments && CFArrayGetCount(attachments) > 0) {
            NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
            NSNumber* status = info[SCStreamFrameInfoStatus];
            if (status && status.integerValue != SCFrameStatusComplete) return;
        }
        CVPixelBufferRef pixel = CMSampleBufferGetImageBuffer(sample);
        if (!pixel) return;
        CVPixelBufferRetain(pixel);
        std::lock_guard lock(mutex_);
        if (latest_) CVPixelBufferRelease(latest_);
        latest_ = pixel;
    }

    // Retained; the caller releases it.
    CVPixelBufferRef latest() {
        std::lock_guard lock(mutex_);
        if (latest_) CVPixelBufferRetain(latest_);
        return latest_;
    }

private:
    SCStream* __strong stream_ = nil;
    RecScreenOutput* __strong receiver_ = nil;
    dispatch_queue_t queue_ = nullptr;
    bool running_ = false;
    std::mutex mutex_;
    CVPixelBufferRef latest_ = nullptr;
};

void receive_screen(ScreenFeed* feed, CMSampleBufferRef sample) { feed->receive(sample); }
class Overlays::Impl {
public:
    struct Layer {
        Overlay overlay;
        CIImage* __strong image = nil;
        std::unique_ptr<CameraFeed> camera;
        std::unique_ptr<ScreenFeed> screen;
    };

    ~Impl() { stop(); }

    Result start(std::uint32_t width, std::uint32_t height, const Options& options, const ClickSource& clicks) {
        if (auto error = core::check_overlays(options.overlays); !error.empty()) return {error};
        if (!core::valid(options.screen_area)) return {"the screen area must lie inside the frame: x, y, width and height are fractions of it"};
        width_ = width;
        height_ = height;
        screen_area_ = options.screen_area;
        screen_visible_ = options.screen_visible;
        // No color management: pixels keep their values, as the capture and the encoder see them.
        NSDictionary* context_options = @{kCIContextWorkingColorSpace: [NSNull null], kCIContextOutputColorSpace: [NSNull null],
                                          kCIContextCacheIntermediates: @NO};
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        context_ = device ? [CIContext contextWithMTLDevice:device options:context_options] : [CIContext contextWithOptions:context_options];
        if (!context_) return {"could not set up overlay drawing"};
        NSDictionary* attributes = @{
            (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            (id)kCVPixelBufferWidthKey: @(width),
            (id)kCVPixelBufferHeightKey: @(height),
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
            (id)kCVPixelBufferMetalCompatibilityKey: @YES
        };
        if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes, &pool_) != kCVReturnSuccess)
            return {"could not allocate composited frames"};
        if (options.clicks) {
            click_source_ = clicks;
            for (int right = 0; right < 2; ++right) {
                const auto pixels = core::click_mark_pixels(kMarkSize, right);
                click_marks_[right] = [CIImage imageWithBitmapData:[NSData dataWithBytes:pixels.data() length:pixels.size()]
                                                       bytesPerRow:kMarkSize * 4
                                                              size:CGSizeMake(kMarkSize, kMarkSize)
                                                            format:kCIFormatBGRA8
                                                        colorSpace:nil];
                if (!click_marks_[right]) return {"could not make the click marks"};
            }
            clicks_ = std::make_unique<core::Clicks>();
            for (int right = 0; right < 2; ++right) {
                presses_[right] = CGEventSourceCounterForEventType(kCGEventSourceStateCombinedSessionState,
                                                                   right ? kCGEventRightMouseDown : kCGEventLeftMouseDown);
                held_[right] = false;
            }
        }
        for (const auto& overlay : options.overlays) {
            Layer layer;
            layer.overlay = overlay;
            switch (overlay.kind) {
            case Overlay::Kind::image: {
                NSString* path = [NSString stringWithUTF8String:overlay.source.c_str()];
                if (path) layer.image = [CIImage imageWithContentsOfURL:[NSURL fileURLWithPath:path]
                                                                options:@{kCIImageColorSpace: [NSNull null]}];
                if (!layer.image || CGRectIsEmpty(layer.image.extent) || CGRectIsInfinite(layer.image.extent))
                    return {"could not open the image " + overlay.source};
                if (layer.image.extent.size.width > 8192 || layer.image.extent.size.height > 8192)
                    return {"the image " + overlay.source + " must be at most 8192x8192"};
                break;
            }
            case Overlay::Kind::camera:
                layer.camera = std::make_unique<CameraFeed>();
                if (auto opened = layer.camera->open(overlay.source); !opened) return opened;
                break;
            case Overlay::Kind::display:
            case Overlay::Kind::window:
                layer.screen = std::make_unique<ScreenFeed>();
                if (auto opened = layer.screen->open(overlay, options.cursor, options.fps); !opened) return opened;
                break;
            }
            layers_.push_back(std::move(layer));
        }
        for (auto& layer : layers_) {
            if (layer.camera) layer.camera->start();
            if (layer.screen)
                if (auto started = layer.screen->start(); !started) return started;
        }
        return {};
    }

    CVPixelBufferRef compose(CVPixelBufferRef base) {
        struct Job { Layer* layer; Area area; };
        Job jobs[core::kMaxOverlays];
        std::size_t count = 0;
        Area screen_area;
        bool screen_visible = true;
        {
            std::lock_guard lock(mutex_);
            for (auto& layer : layers_)
                if (layer.overlay.visible) jobs[count++] = {&layer, layer.overlay.area};
            screen_area = screen_area_;
            screen_visible = screen_visible_;
        }
        std::array<core::ClickMark, core::Clicks::kMax> marks;
        std::size_t mark_count = 0;
        if (clicks_) {
            // The presses are stamped when this reads them, about one frame late, so marks are
            // drawn for now rather than for the frame's slot, which is 40 ms behind.
            const auto now = core::Clicks::Clock::now();
            read_clicks(now);
            if (screen_visible) mark_count = clicks_->marks(now, click_area(), width_, height_, screen_area, marks);
        }
        const bool whole = screen_visible && screen_area.x <= 0 && screen_area.y <= 0 && screen_area.width >= 1 && screen_area.height >= 1;
        if (whole && !count && !mark_count) return CVPixelBufferRetain(base);
        @autoreleasepool {
            CVPixelBufferRef out = nullptr;
            if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool_, &out) != kCVReturnSuccess) return nullptr;
            CVBufferPropagateAttachments(base, out);
            const CGRect frame = CGRectMake(0, 0, width_, height_);
            CIImage* result = nil;
            if (whole) {
                result = [CIImage imageWithCVPixelBuffer:base];
            } else {
                result = [[CIImage imageWithColor:[CIColor colorWithRed:0 green:0 blue:0]] imageByCroppingToRect:frame];
                if (screen_visible)
                    result = [placed([CIImage imageWithCVPixelBuffer:base], Overlay::Kind::image, screen_area) imageByCompositingOverImage:result];
            }
            for (std::size_t i = 0; i < mark_count; ++i) result = [mark(marks[i]) imageByCompositingOverImage:result];
            for (std::size_t i = 0; i < count; ++i) {
                Layer& layer = *jobs[i].layer;
                CIImage* picture = layer.image;
                if (layer.camera || layer.screen) {
                    CVPixelBufferRef pixel = layer.camera ? layer.camera->latest() : layer.screen->latest();
                    if (!pixel) continue;
                    picture = [CIImage imageWithCVPixelBuffer:pixel options:@{kCIImageColorSpace: [NSNull null]}];
                    CVPixelBufferRelease(pixel);
                }
                result = [placed(picture, layer.overlay.kind, jobs[i].area) imageByCompositingOverImage:result];
            }
            [context_ render:result toCVPixelBuffer:out bounds:frame colorSpace:nil];
            return out;
        }
    }

    Result set(std::size_t index, bool visible, const std::optional<Area>& area) {
        std::lock_guard lock(mutex_);
        if (index >= layers_.size()) return {"no overlay " + std::to_string(index + 1)};
        if (area) {
            if (auto error = core::check_area(index, *area); !error.empty()) return {error};
            layers_[index].overlay.area = *area;
        }
        layers_[index].overlay.visible = visible;
        return {};
    }

    Result set_screen(bool visible, const std::optional<Area>& area) {
        if (area && !core::valid(*area)) return {"the screen area must lie inside the frame: x, y, width and height are fractions of it"};
        std::lock_guard lock(mutex_);
        if (area) screen_area_ = *area;
        screen_visible_ = visible;
        return {};
    }

    std::uint64_t camera_frames() const {
        std::lock_guard lock(mutex_);
        std::uint64_t total = 0;
        for (const auto& layer : layers_)
            if (layer.camera) total += layer.camera->frames();
        return total;
    }

    void stop() {
        std::lock_guard lock(mutex_);
        for (auto& layer : layers_) {
            if (layer.camera) layer.camera->stop();
            if (layer.screen) layer.screen->stop();
        }
        layers_.clear();
        if (pool_) { CVPixelBufferPoolRelease(pool_); pool_ = nullptr; }
        context_ = nil;
    }

private:
    static constexpr std::uint32_t kMarkSize = 128;

    // Turns new mouse-down counts and released buttons into presses and releases at the
    // pointer. A press and release between two reads still counts.
    void read_clicks(core::Clicks::Clock::time_point now) {
        CGEventRef event = CGEventCreate(nullptr);
        const CGPoint at = event ? CGEventGetLocation(event) : CGPointZero;
        if (event) CFRelease(event);
        for (int right = 0; right < 2; ++right) {
            const auto presses = CGEventSourceCounterForEventType(kCGEventSourceStateCombinedSessionState,
                                                                  right ? kCGEventRightMouseDown : kCGEventLeftMouseDown);
            const bool down = CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,
                                                       right ? kCGMouseButtonRight : kCGMouseButtonLeft);
            if (presses != presses_[right]) {
                presses_[right] = presses;
                clicks_->press(now, at.x, at.y, right);
                held_[right] = true;
            }
            if (held_[right] && !down) {
                clicks_->release(now, right);
                held_[right] = false;
            }
        }
    }

    // The main capture's rectangle on the desktop in points, looked up again every half
    // second or so, since a window moves.
    core::DesktopRect click_area() {
        if (click_lookups_++ % 16) return click_rect_;
        CGRect bounds = CGRectNull;
        if (click_source_.window) {
            // The array holds window IDs as pointer-sized values.
            const void* ids[] = {reinterpret_cast<const void*>(static_cast<std::uintptr_t>(click_source_.window))};
            CFArrayRef wanted = CFArrayCreate(nullptr, ids, 1, nullptr);
            CFArrayRef info = wanted ? CGWindowListCreateDescriptionFromArray(wanted) : nullptr;
            if (info && CFArrayGetCount(info) > 0) {
                auto window = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(info, 0));
                auto rect = static_cast<CFDictionaryRef>(CFDictionaryGetValue(window, kCGWindowBounds));
                if (!rect || !CGRectMakeWithDictionaryRepresentation(rect, &bounds)) bounds = CGRectNull;
            }
            if (info) CFRelease(info);
            if (wanted) CFRelease(wanted);
        } else if (click_source_.display) {
            bounds = CGDisplayBounds(click_source_.display);
            if (const auto& r = click_source_.region; r && bounds.size.width > 0) {
                // The region is in the display's pixels.
                auto mode = CGDisplayCopyDisplayMode(click_source_.display);
                const double scale = mode ? CGDisplayModeGetPixelWidth(mode) / bounds.size.width : 1.0;
                if (mode) CGDisplayModeRelease(mode);
                bounds = CGRectMake(bounds.origin.x + r->x / scale, bounds.origin.y + r->y / scale, r->width / scale, r->height / scale);
            }
        }
        if (CGRectIsNull(bounds)) return click_rect_ = {};
        return click_rect_ = {bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height};
    }

    // A click mark centered on its point, faded to its opacity. Core Image's origin is at the
    // bottom left.
    CIImage* mark(const core::ClickMark& m) const {
        const double scale = m.diameter / kMarkSize;
        CIImage* picture = [click_marks_[m.right ? 1 : 0]
            imageByApplyingTransform:CGAffineTransformMake(scale, 0, 0, scale, m.x - m.diameter / 2, height_ - m.y - m.diameter / 2)];
        if (m.alpha >= 1) return picture;
        // CIColorMatrix works on unpremultiplied color, so scaling alpha alone fades the mark.
        return [picture imageByApplyingFilter:@"CIColorMatrix"
                          withInputParameters:@{@"inputAVector": [CIVector vectorWithX:0 Y:0 Z:0 W:m.alpha]}];
    }

    // A picture scaled and cropped into its area, as core::place lays it out.
    CIImage* placed(CIImage* picture, Overlay::Kind kind, const Area& area) const {
        const CGRect extent = picture.extent;
        const double pw = extent.size.width, ph = extent.size.height;
        const auto p = core::place(kind, area, width_, height_, static_cast<std::uint32_t>(pw), static_cast<std::uint32_t>(ph));
        // Core Image puts the origin at the bottom left.
        const CGRect shown = CGRectMake(extent.origin.x + p.u0 * pw, extent.origin.y + (1 - p.v1) * ph,
                                        (p.u1 - p.u0) * pw, (p.v1 - p.v0) * ph);
        const CGRect target = CGRectMake(p.left, height_ - p.bottom, p.right - p.left, p.bottom - p.top);
        CGAffineTransform move = CGAffineTransformMakeTranslation(-shown.origin.x, -shown.origin.y);
        move = CGAffineTransformConcat(move, CGAffineTransformMakeScale(target.size.width / shown.size.width,
                                                                        target.size.height / shown.size.height));
        move = CGAffineTransformConcat(move, CGAffineTransformMakeTranslation(target.origin.x, target.origin.y));
        return [[[picture imageByCroppingToRect:shown] imageByApplyingTransform:move highQualityDownsample:YES]
            imageByCroppingToRect:target];
    }

    std::uint32_t width_ = 0, height_ = 0;
    CIContext* __strong context_ = nil;
    CVPixelBufferPoolRef pool_ = nullptr;
    mutable std::mutex mutex_;  // Guards the layer list, each layer's visibility and area, and the screen's.
    std::vector<Layer> layers_;
    Area screen_area_;
    bool screen_visible_ = true;
    std::unique_ptr<core::Clicks> clicks_;
    ClickSource click_source_;
    core::DesktopRect click_rect_;
    std::uint64_t click_lookups_ = 0;
    std::uint32_t presses_[2] = {};
    bool held_[2] = {};
    CIImage* __strong click_marks_[2] = {nil, nil};
};

Overlays::Overlays() : impl_(std::make_unique<Impl>()) {}
Overlays::~Overlays() = default;
Result Overlays::start(std::uint32_t width, std::uint32_t height, const Options& options, const ClickSource& clicks) {
    return impl_->start(width, height, options, clicks);
}
CVPixelBufferRef Overlays::compose(CVPixelBufferRef base) { return impl_->compose(base); }
Result Overlays::set(std::size_t index, bool visible, const std::optional<Area>& area) { return impl_->set(index, visible, area); }
Result Overlays::set_screen(bool visible, const std::optional<Area>& area) { return impl_->set_screen(visible, area); }
std::uint64_t Overlays::camera_frames() const { return impl_->camera_frames(); }
void Overlays::stop() { impl_->stop(); }

} // namespace rec::mac
