#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "platform/macos/overlay.hpp"
#include "platform/macos/h264_decoder.hpp"
#include "platform/macos/screen.h"
#include "core/clicks.hpp"
#include "core/overlay.hpp"
#include "core/phone.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

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
// BGRA on its own queue; the test camera draws core::test_camera_frame at 30 fps; a phone
// ("phone:TOKEN") is decoded with VideoToolbox on its own thread, at whatever size it sends.
class CameraFeed {
public:
    ~CameraFeed() { stop(); }

    Result open(const std::string& wanted) {
        if (wanted.rfind("phone:", 0) == 0) {
            std::string error;
            phone_ = core::PhoneCamera::open(wanted.substr(6), error);
            if (!phone_) return {error};
            name_ = "phone";
            return {};
        }
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
        // An unplugged camera, or a session that fails, stops delivering; its last frame stays.
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        observers_ = @[
            [center addObserverForName:AVCaptureDeviceWasDisconnectedNotification object:device queue:nil
                            usingBlock:^(NSNotification*) { lost_.store(true); }],
            [center addObserverForName:AVCaptureSessionRuntimeErrorNotification object:session_ queue:nil
                            usingBlock:^(NSNotification*) { lost_.store(true); }],
        ];
        return {};
    }

    void start() {
        if (phone_) {
            stop_ = false;
            phone_->set_sink([this](const std::uint8_t* data, std::size_t size, bool key) { queue_unit(data, size, key); });
            thread_ = std::thread([this] { decode(); });
        } else if (session_) {
            [session_ startRunning];
        } else if (pool_) {
            stop_ = false;
            thread_ = std::thread([this] { generate(); });
        }
    }

    void stop() {
        if (phone_) phone_->set_sink(nullptr);
        for (id observer in observers_) [NSNotificationCenter.defaultCenter removeObserver:observer];
        observers_ = nil;
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
        {
            std::lock_guard lock(units_mutex_);
            stop_ = true;
        }
        units_ready_.notify_all();
        if (thread_.joinable()) thread_.join();
        phone_.reset();
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
    bool lost() const { return lost_.load(); }

private:
    // On the phone's network thread. The slots keep their capacity as they circulate with
    // the decoder's buffer, so a steady stream allocates nothing here.
    void queue_unit(const std::uint8_t* data, std::size_t size, bool key) {
        {
            std::lock_guard lock(units_mutex_);
            // The decoder fell behind by two seconds: start again at the next keyframe.
            if (unit_count_ == units_.size()) {
                unit_count_ = 0;
                skip_to_key_ = true;
            }
            if (skip_to_key_ && !key) {
                phone_->request_keyframe();
                return;
            }
            skip_to_key_ = false;
            units_[(unit_head_ + unit_count_++) % units_.size()].assign(data, data + size);
        }
        units_ready_.notify_one();
    }

    void decode() {
        H264Decoder decoder([this](CVPixelBufferRef pixel) { receive(pixel); });
        std::vector<std::uint8_t> unit;
        for (;;) {
            {
                std::unique_lock lock(units_mutex_);
                units_ready_.wait(lock, [this] { return stop_.load() || unit_count_ > 0; });
                if (stop_) return;
                unit.swap(units_[unit_head_]);
                unit_head_ = (unit_head_ + 1) % units_.size();
                --unit_count_;
            }
            if (!decoder.decode(unit.data(), unit.size())) phone_->request_keyframe();
        }
    }

    void generate() {
        const auto begin = std::chrono::steady_clock::now();
        // REC_E2E_LOSE_CAMERA=MS stops the test camera as an unplug would.
        const char* lose = std::getenv("REC_E2E_LOSE_CAMERA");
        const std::uint64_t lose_frame = lose ? std::strtoull(lose, nullptr, 10) * core::kTestCameraFps / 1000 : UINT64_MAX;
        for (std::uint64_t n = 0; !stop_; ++n) {
            if (n == lose_frame) { lost_.store(true); return; }
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
    NSArray* __strong observers_ = nil;
    std::atomic_bool lost_{false};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic_bool stop_{false};
    std::thread thread_;
    std::shared_ptr<core::PhoneCamera> phone_;
    std::mutex units_mutex_;
    std::condition_variable units_ready_;
    std::array<std::vector<std::uint8_t>, 60> units_;  // Access units waiting for the decoder, a ring.
    std::size_t unit_head_ = 0, unit_count_ = 0;
    bool skip_to_key_ = false;
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

// Lanczos shrinking of NV12 frames on the GPU with Metal Performance Shaders. The luma and
// chroma planes are scaled separately, so there is no color conversion and no float
// intermediate the size of the source. Core Image's Lanczos filter did both, and could not
// keep up with a 4K capture.
class PlaneScaler {
public:
    PlaneScaler() = default;
    PlaneScaler(const PlaneScaler&) = delete;
    PlaneScaler& operator=(const PlaneScaler&) = delete;
    ~PlaneScaler() { stop(); }

    bool start(id<MTLDevice> device) {
        if (!device || !MPSSupportsMTLDevice(device)) return false;
        queue_ = [device newCommandQueue];
        filter_ = [[MPSImageLanczosScale alloc] initWithDevice:device];
        // Past the edge the filter would read black and darken the outermost pixels.
        filter_.edgeMode = MPSImageEdgeModeClamp;
        return queue_ && filter_ &&
               CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &cache_) == kCVReturnSuccess;
    }

    bool ready() const { return cache_ != nullptr; }

    void stop() {
        if (pool_) { CVPixelBufferPoolRelease(pool_); pool_ = nullptr; }
        if (cache_) { CFRelease(cache_); cache_ = nullptr; }
        filter_ = nil;
        queue_ = nil;
    }

    // A new frame with source shrunk to width x height, or null. Waits for the GPU.
    CVPixelBufferRef scale(CVPixelBufferRef source, std::uint32_t width, std::uint32_t height) {
        if (!pool_ || width != width_ || height != height_) {
            if (pool_) { CVPixelBufferPoolRelease(pool_); pool_ = nullptr; }
            NSDictionary* attributes = @{
                (id)kCVPixelBufferPixelFormatTypeKey: @(CVPixelBufferGetPixelFormatType(source)),
                (id)kCVPixelBufferWidthKey: @(width),
                (id)kCVPixelBufferHeightKey: @(height),
                (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                (id)kCVPixelBufferMetalCompatibilityKey: @YES
            };
            if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes, &pool_) != kCVReturnSuccess)
                return nullptr;
            width_ = width;
            height_ = height;
        }
        if (CVPixelBufferGetPlaneCount(source) != 2) return nullptr;
        CVPixelBufferRef out = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool_, &out) != kCVReturnSuccess) return nullptr;
        CVMetalTextureRef textures[4] = {};
        bool ok = texture(source, 0, MTLPixelFormatR8Unorm, textures[0]) &&
                  texture(source, 1, MTLPixelFormatRG8Unorm, textures[1]) &&
                  texture(out, 0, MTLPixelFormatR8Unorm, textures[2]) && texture(out, 1, MTLPixelFormatRG8Unorm, textures[3]);
        if (ok) {
            id<MTLCommandBuffer> commands = [queue_ commandBuffer];
            for (int plane = 0; plane < 2; ++plane)
                [filter_ encodeToCommandBuffer:commands
                                 sourceTexture:CVMetalTextureGetTexture(textures[plane])
                            destinationTexture:CVMetalTextureGetTexture(textures[plane + 2])];
            [commands commit];
            [commands waitUntilCompleted];
            ok = commands.status == MTLCommandBufferStatusCompleted;
        }
        for (CVMetalTextureRef t : textures)
            if (t) CFRelease(t);
        CVMetalTextureCacheFlush(cache_, 0);
        if (!ok) {
            CVPixelBufferRelease(out);
            return nullptr;
        }
        CVBufferPropagateAttachments(source, out);
        return out;
    }

private:
    bool texture(CVPixelBufferRef buffer, std::size_t plane, MTLPixelFormat format, CVMetalTextureRef& out) {
        NSDictionary* usage = @{(id)kCVMetalTextureUsage: @(MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)};
        return CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache_, buffer, (__bridge CFDictionaryRef)usage,
                                                         format, CVPixelBufferGetWidthOfPlane(buffer, plane),
                                                         CVPixelBufferGetHeightOfPlane(buffer, plane), plane,
                                                         &out) == kCVReturnSuccess &&
               out;
    }

    id<MTLCommandQueue> __strong queue_ = nil;
    MPSImageLanczosScale* __strong filter_ = nil;
    CVMetalTextureCacheRef cache_ = nullptr;
    CVPixelBufferPoolRef pool_ = nullptr;
    std::uint32_t width_ = 0, height_ = 0;
};
class Overlays::Impl {
public:
    // A style's pictures for one size, made on the CPU once and reused every frame.
    struct DecorImages {
        int width = 0, height = 0;
        std::string style;  // core::style_text
        CIImage* __strong shadow = nil;
        CIImage* __strong border = nil;
        CIImage* __strong inside = nil;
    };
    struct Layer {
        Overlay overlay;
        CIImage* __strong image = nil;
        std::unique_ptr<CameraFeed> camera;
        std::unique_ptr<ScreenFeed> screen;
        DecorImages decor[2];  // One per canvas, each used only by the thread that composes it.
    };

    ~Impl() { stop(); }

    Result start(std::uint32_t width, std::uint32_t height, const Options& options, const ClickSource& clicks) {
        if (auto error = core::check_overlays(options.overlays); !error.empty()) return {error};
        if (!core::valid(options.screen_area)) return {"the screen area must lie inside the frame: x, y, width and height are fractions of it"};
        if (auto error = core::check_crop("the screen", options.screen_crop); !error.empty()) return {error};
        if (auto error = core::check_canvas(options); !error.empty()) return {error};
        canvas_count_ = options.canvas2.width ? 2 : 1;
        canvases_[kMainCanvas].width = width;
        canvases_[kMainCanvas].height = height;
        canvases_[kMainCanvas].screen = {options.screen_visible, options.screen_area, options.screen_crop};
        for (const auto& overlay : options.overlays) canvases_[kMainCanvas].layers.push_back({overlay.visible, overlay.area, overlay.crop});
        if (canvas_count_ == 2) {
            auto& second = canvases_[kSecondCanvas];
            second.width = options.canvas2.width;
            second.height = options.canvas2.height;
            second.screen = options.canvas2.screen;
            for (std::size_t i = 0; i < options.overlays.size(); ++i) second.layers.push_back(core::canvas_layout(options, i));
        }
        // No color management: pixels keep their values, as the capture and the encoder see them.
        NSDictionary* context_options = @{kCIContextWorkingColorSpace: [NSNull null], kCIContextOutputColorSpace: [NSNull null],
                                          kCIContextCacheIntermediates: @NO};
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        context_ = device ? [CIContext contextWithMTLDevice:device options:context_options] : [CIContext contextWithOptions:context_options];
        if (!context_) return {"could not set up overlay drawing"};
        // Without Metal Performance Shaders, Core Image shrinks a native-size capture instead.
        if (options.scaler == Scaler::lanczos) lanczos_.start(device);
        for (std::size_t i = 0; i < canvas_count_; ++i) {
            auto& canvas = canvases_[i];
            NSDictionary* attributes = @{
                (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                (id)kCVPixelBufferWidthKey: @(canvas.width),
                (id)kCVPixelBufferHeightKey: @(canvas.height),
                (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                (id)kCVPixelBufferMetalCompatibilityKey: @YES
            };
            if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes, &canvas.pool) != kCVReturnSuccess)
                return {"could not allocate composited frames"};
        }
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

    CVPixelBufferRef compose(std::size_t index, CVPixelBufferRef base) {
        if (index >= canvas_count_) return nullptr;
        Canvas& canvas = canvases_[index];
        struct Job { Layer* layer; Layout layout; Style style; };
        Job jobs[core::kMaxOverlays];
        std::size_t count = 0;
        Layout screen;
        // The presses are stamped when this reads them, about one frame late, so marks are
        // drawn for now rather than for the frame's slot, which is 40 ms behind.
        const auto now = core::Clicks::Clock::now();
        core::DesktopRect click_place;
        if (clicks_ && index == kMainCanvas) {
            read_clicks(now);
            click_place = click_area();
        }
        {
            // The second canvas is drawn on its own thread, with the capture's place the main
            // canvas found last.
            std::lock_guard lock(mutex_);
            for (std::size_t i = 0; i < layers_.size(); ++i)
                if (canvas.layers[i].visible) jobs[count++] = {&layers_[i], canvas.layers[i], layers_[i].overlay.style};
            screen = canvas.screen;
            if (index == kMainCanvas) click_now_ = click_place;
            else click_place = click_now_;
        }
        const auto full = [](const Area& a) { return a.x <= 0 && a.y <= 0 && a.width >= 1 && a.height >= 1; };
        const std::uint32_t width = canvas.width, height = canvas.height;
        const auto base_width = static_cast<std::uint32_t>(CVPixelBufferGetWidth(base));
        const auto base_height = static_cast<std::uint32_t>(CVPixelBufferGetHeight(base));
        std::array<core::ClickMark, core::Clicks::kMax> marks;
        std::size_t mark_count = 0;
        if (clicks_ && screen.visible)
            mark_count = clicks_->marks(now, click_place, base_width, base_height, width, height, screen.area, screen.crop, marks);
        const bool whole = screen.visible && full(screen.area) && full(screen.crop);
        // A capture at its native size (Scaler::lanczos) is shrunk here, straight to the size
        // it takes in the main canvas.
        struct Shrunk {
            CVPixelBufferRef frame = nullptr;
            ~Shrunk() { if (frame) CVPixelBufferRelease(frame); }
        } shrunk;
        if (index == kMainCanvas && screen.visible && full(screen.crop) && lanczos_.ready() &&
            (base_width != width || base_height != height)) {
            const auto p = core::place(Overlay::Kind::image, screen.area, width, height, base_width, base_height);
            const auto even = [](double size) { return std::max<std::uint32_t>(2, static_cast<std::uint32_t>(std::lround(size / 2)) * 2); };
            // Filling the frame, it takes the frame's exact size, so no further pass is needed.
            const std::uint32_t w = whole ? width : even(p.right - p.left), h = whole ? height : even(p.bottom - p.top);
            if (w < base_width || h < base_height) {
                shrunk.frame = lanczos_.scale(base, w, h);
                if (!shrunk.frame) return nullptr;
                base = shrunk.frame;
            }
        }
        const bool fits = CVPixelBufferGetWidth(base) == width && CVPixelBufferGetHeight(base) == height;
        if (index == kMainCanvas && whole && fits && !count && !mark_count) return CVPixelBufferRetain(base);
        @autoreleasepool {
            CVPixelBufferRef out = nullptr;
            // The second canvas's pool is bounded: past it, its encoder has fallen behind and
            // the frame is skipped.
            NSDictionary* limit = index == kMainCanvas ? nil : @{(id)kCVPixelBufferPoolAllocationThresholdKey: @(kSecondFrames)};
            if (CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault, canvas.pool, (__bridge CFDictionaryRef)limit,
                                                                   &out) != kCVReturnSuccess)
                return nullptr;
            CVBufferPropagateAttachments(base, out);
            const CGRect frame = CGRectMake(0, 0, width, height);
            CIImage* result = nil;
            if (whole && fits) {
                result = [CIImage imageWithCVPixelBuffer:base];
            } else {
                result = [[CIImage imageWithColor:[CIColor colorWithRed:0 green:0 blue:0]] imageByCroppingToRect:frame];
                if (screen.visible)
                    result = [placed([CIImage imageWithCVPixelBuffer:base], Overlay::Kind::image, screen, width, height)
                        imageByCompositingOverImage:result];
            }
            for (std::size_t i = 0; i < mark_count; ++i) result = [mark(marks[i], height) imageByCompositingOverImage:result];
            for (std::size_t i = 0; i < count; ++i) {
                Layer& layer = *jobs[i].layer;
                CIImage* picture = layer.image;
                if (layer.camera || layer.screen) {
                    CVPixelBufferRef pixel = layer.camera ? layer.camera->latest() : layer.screen->latest();
                    if (!pixel) continue;
                    picture = [CIImage imageWithCVPixelBuffer:pixel options:@{kCIImageColorSpace: [NSNull null]}];
                    CVPixelBufferRelease(pixel);
                }
                const Style& style = jobs[i].style;
                Layout layout = jobs[i].layout;
                layout.area = core::shaped_area(style, layout.area, width, height);
                CGRect target;
                CIImage* shown = placed(picture, layer.overlay.kind, layout, width, height, &target);
                if (core::plain(style)) {
                    result = [shown imageByCompositingOverImage:result];
                    continue;
                }
                const auto d = core::decor(style, height);
                const DecorImages& decor = decor_for(layer.decor[index], style, d, static_cast<int>(std::lround(target.size.width)),
                                                     static_cast<int>(std::lround(target.size.height)));
                const auto at = [](CIImage* image, double x, double y) {
                    return [image imageByApplyingTransform:CGAffineTransformMakeTranslation(x, y)];
                };
                // Core Image's origin is at the bottom left, so the shadow drops toward lower y.
                if (decor.shadow)
                    result = [at(decor.shadow, target.origin.x - d.pad, target.origin.y - d.pad - d.drop) imageByCompositingOverImage:result];
                if (decor.border) result = [at(decor.border, target.origin.x, target.origin.y) imageByCompositingOverImage:result];
                if (!decor.inside) continue;
                result = [shown imageByApplyingFilter:@"CIBlendWithAlphaMask"
                                  withInputParameters:@{kCIInputBackgroundImageKey: result,
                                                        kCIInputMaskImageKey: at(decor.inside, target.origin.x, target.origin.y)}];
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
            canvases_[kMainCanvas].layers[index].area = *area;
        }
        canvases_[kMainCanvas].layers[index].visible = visible;
        return {};
    }

    Result set_screen(bool visible, const std::optional<Area>& area) {
        if (area && !core::valid(*area)) return {"the screen area must lie inside the frame: x, y, width and height are fractions of it"};
        std::lock_guard lock(mutex_);
        if (area) canvases_[kMainCanvas].screen.area = *area;
        canvases_[kMainCanvas].screen.visible = visible;
        return {};
    }

    Result set_layout(std::size_t canvas, std::size_t layer, const Layout& layout) {
        if (canvas >= canvas_count_) return {canvas == kSecondCanvas ? "this recording has no second canvas" : "no such canvas"};
        if (auto error = core::check_layout(layer, layout); !error.empty()) return {error};
        std::lock_guard lock(mutex_);
        if (layer == kScreenLayer) {
            canvases_[canvas].screen = layout;
            return {};
        }
        if (layer >= layers_.size()) return {"no overlay " + std::to_string(layer + 1)};
        canvases_[canvas].layers[layer] = layout;
        return {};
    }
    Result set_style(std::size_t index, const Style& style) {
        if (auto error = core::check_style(style); !error.empty()) return {error};
        std::lock_guard lock(mutex_);
        if (index >= layers_.size()) return {"no overlay " + std::to_string(index + 1)};
        if (layers_[index].overlay.kind != Overlay::Kind::camera) return {"shapes, borders and shadows are for a camera"};
        layers_[index].overlay.style = style;
        return {};
    }
    Result set_color(ColorTarget, const ColorAdjust& color) {
        if (auto error = core::check_color(color); !error.empty()) return {error};
        if (color.brightness || color.contrast || color.saturation) return {"color adjustment is not yet supported on macOS"};
        return {};
    }

    std::uint64_t camera_frames() const {
        std::lock_guard lock(mutex_);
        std::uint64_t total = 0;
        for (const auto& layer : layers_)
            if (layer.camera) total += layer.camera->frames();
        return total;
    }

    bool camera_lost() const {
        std::lock_guard lock(mutex_);
        for (const auto& layer : layers_)
            if (layer.camera && layer.camera->lost()) return true;
        return false;
    }

    void stop() {
        std::lock_guard lock(mutex_);
        for (auto& layer : layers_) {
            if (layer.camera) layer.camera->stop();
            if (layer.screen) layer.screen->stop();
        }
        layers_.clear();
        for (auto& canvas : canvases_) {
            if (canvas.pool) CVPixelBufferPoolRelease(canvas.pool);
            canvas = {};
        }
        canvas_count_ = 1;
        lanczos_.stop();
        context_ = nil;
    }

private:
    static constexpr std::uint32_t kMarkSize = 128;
    // The second canvas's frames: its encoder's queue of four plus what VideoToolbox holds.
    static constexpr int kSecondFrames = 12;

    // One picture of the recording: its size, frames and the layout of every source.
    struct Canvas {
        std::uint32_t width = 0, height = 0;
        CVPixelBufferPoolRef pool = nullptr;
        Layout screen;
        std::vector<Layout> layers;  // One per layers_ entry.
    };

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

    // A click mark centered on its point in a canvas height pixels tall, faded to its
    // opacity. Core Image's origin is at the bottom left.
    CIImage* mark(const core::ClickMark& m, std::uint32_t height) const {
        const double scale = m.diameter / kMarkSize;
        CIImage* picture = [click_marks_[m.right ? 1 : 0]
            imageByApplyingTransform:CGAffineTransformMake(scale, 0, 0, scale, m.x - m.diameter / 2, height - m.y - m.diameter / 2)];
        if (m.alpha >= 1) return picture;
        // CIColorMatrix works on unpremultiplied color, so scaling alpha alone fades the mark.
        return [picture imageByApplyingFilter:@"CIColorMatrix"
                          withInputParameters:@{@"inputAVector": [CIVector vectorWithX:0 Y:0 Z:0 W:m.alpha]}];
    }

    // A picture's crop scaled and cropped into its area of a width x height canvas, as
    // core::place lays it out.
    // A style's pictures for a camera drawn width x height, made again when either changes.
    static const DecorImages& decor_for(DecorImages& decor, const Style& style, const core::Decor& d, int width, int height) {
        const auto key = core::style_text(style);
        if (decor.width == width && decor.height == height && decor.style == key) return decor;
        const auto image = [&](core::DecorPart part, double pad) -> CIImage* {
            auto pixels = core::decor_pixels(style, d, part, width, height);
            if (pixels.empty()) return nil;
            const int w = width + static_cast<int>(2 * pad), h = height + static_cast<int>(2 * pad);
            return [CIImage imageWithBitmapData:[NSData dataWithBytes:pixels.data() length:pixels.size()]
                                    bytesPerRow:static_cast<size_t>(w) * 4
                                           size:CGSizeMake(w, h)
                                         format:kCIFormatBGRA8
                                     colorSpace:nil];
        };
        decor = {width, height, key, image(core::DecorPart::shadow, d.pad), image(core::DecorPart::border, 0),
                 image(core::DecorPart::inside, 0)};
        return decor;
    }

    CIImage* placed(CIImage* picture, Overlay::Kind kind, const Layout& layout, std::uint32_t width, std::uint32_t height,
                    CGRect* where = nullptr) const {
        const CGRect extent = picture.extent;
        const double pw = extent.size.width, ph = extent.size.height;
        const auto p = core::place(kind, layout.area, width, height, static_cast<std::uint32_t>(pw), static_cast<std::uint32_t>(ph),
                                   layout.crop);
        // Core Image puts the origin at the bottom left.
        const CGRect shown = CGRectMake(extent.origin.x + p.u0 * pw, extent.origin.y + (1 - p.v1) * ph,
                                        (p.u1 - p.u0) * pw, (p.v1 - p.v0) * ph);
        const CGRect target = CGRectMake(p.left, height - p.bottom, p.right - p.left, p.bottom - p.top);
        if (where) *where = target;
        const double sx = target.size.width / shown.size.width, sy = target.size.height / shown.size.height;
        CIImage* cropped = [picture imageByCroppingToRect:shown];
        CGAffineTransform move = CGAffineTransformMakeTranslation(-shown.origin.x, -shown.origin.y);
        move = CGAffineTransformConcat(move, CGAffineTransformMakeScale(sx, sy));
        move = CGAffineTransformConcat(move, CGAffineTransformMakeTranslation(target.origin.x, target.origin.y));
        return [[cropped imageByApplyingTransform:move highQualityDownsample:YES] imageByCroppingToRect:target];
    }

    CIContext* __strong context_ = nil;
    PlaneScaler lanczos_;
    mutable std::mutex mutex_;  // Guards the layer list and each canvas's layouts.
    std::vector<Layer> layers_;
    std::array<Canvas, 2> canvases_;
    std::size_t canvas_count_ = 1;
    core::DesktopRect click_now_;  // The capture's place on the desktop for the marks; guarded by mutex_.
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
CVPixelBufferRef Overlays::compose(std::size_t canvas, CVPixelBufferRef base) { return impl_->compose(canvas, base); }
Result Overlays::set(std::size_t index, bool visible, const std::optional<Area>& area) { return impl_->set(index, visible, area); }
Result Overlays::set_screen(bool visible, const std::optional<Area>& area) { return impl_->set_screen(visible, area); }
Result Overlays::set_layout(std::size_t canvas, std::size_t layer, const Layout& layout) {
    return impl_->set_layout(canvas, layer, layout);
}
Result Overlays::set_style(std::size_t index, const Style& style) { return impl_->set_style(index, style); }
Result Overlays::set_color(ColorTarget target, const ColorAdjust& color) { return impl_->set_color(target, color); }
std::uint64_t Overlays::camera_frames() const { return impl_->camera_frames(); }
bool Overlays::camera_lost() const { return impl_->camera_lost(); }
void Overlays::stop() { impl_->stop(); }

} // namespace rec::mac
