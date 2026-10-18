#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "platform/macos/overlay.hpp"
#include "core/overlay.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace rec::mac {
class CameraFeed;
void receive_camera(CameraFeed* feed, CMSampleBufferRef sample);
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

class Overlays::Impl {
public:
    struct Layer {
        Overlay overlay;
        CIImage* __strong image = nil;
        std::unique_ptr<CameraFeed> camera;
    };

    ~Impl() { stop(); }

    Result start(std::uint32_t width, std::uint32_t height, const std::vector<Overlay>& overlays) {
        if (auto error = core::check_overlays(overlays); !error.empty()) return {error};
        width_ = width;
        height_ = height;
        // No color management: pixels keep their values, as the capture and the encoder see them.
        NSDictionary* options = @{kCIContextWorkingColorSpace: [NSNull null], kCIContextOutputColorSpace: [NSNull null],
                                  kCIContextCacheIntermediates: @NO};
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        context_ = device ? [CIContext contextWithMTLDevice:device options:options] : [CIContext contextWithOptions:options];
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
        for (const auto& overlay : overlays) {
            Layer layer;
            layer.overlay = overlay;
            if (overlay.kind == Overlay::Kind::image) {
                NSString* path = [NSString stringWithUTF8String:overlay.source.c_str()];
                if (path) layer.image = [CIImage imageWithContentsOfURL:[NSURL fileURLWithPath:path]
                                                                options:@{kCIImageColorSpace: [NSNull null]}];
                if (!layer.image || CGRectIsEmpty(layer.image.extent) || CGRectIsInfinite(layer.image.extent))
                    return {"could not open the image " + overlay.source};
                if (layer.image.extent.size.width > 8192 || layer.image.extent.size.height > 8192)
                    return {"the image " + overlay.source + " must be at most 8192x8192"};
            } else {
                layer.camera = std::make_unique<CameraFeed>();
                if (auto opened = layer.camera->open(overlay.source); !opened) return opened;
            }
            layers_.push_back(std::move(layer));
        }
        for (auto& layer : layers_)
            if (layer.camera) layer.camera->start();
        return {};
    }

    CVPixelBufferRef compose(CVPixelBufferRef base) {
        struct Job { Layer* layer; Area area; };
        Job jobs[core::kMaxOverlays];
        std::size_t count = 0;
        {
            std::lock_guard lock(mutex_);
            for (auto& layer : layers_)
                if (layer.overlay.visible) jobs[count++] = {&layer, layer.overlay.area};
        }
        if (!count) return CVPixelBufferRetain(base);
        @autoreleasepool {
            CVPixelBufferRef out = nullptr;
            if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool_, &out) != kCVReturnSuccess) return nullptr;
            CVBufferPropagateAttachments(base, out);
            CIImage* result = [CIImage imageWithCVPixelBuffer:base];
            const double frame_height = height_;
            for (std::size_t i = 0; i < count; ++i) {
                Layer& layer = *jobs[i].layer;
                CIImage* picture = layer.image;
                if (layer.camera) {
                    CVPixelBufferRef pixel = layer.camera->latest();
                    if (!pixel) continue;
                    picture = [CIImage imageWithCVPixelBuffer:pixel options:@{kCIImageColorSpace: [NSNull null]}];
                    CVPixelBufferRelease(pixel);
                }
                const CGRect extent = picture.extent;
                const double pw = extent.size.width, ph = extent.size.height;
                const auto p = core::place(layer.overlay.kind, jobs[i].area, width_, height_,
                                           static_cast<std::uint32_t>(pw), static_cast<std::uint32_t>(ph));
                // Core Image puts the origin at the bottom left.
                const CGRect shown = CGRectMake(extent.origin.x + p.u0 * pw, extent.origin.y + (1 - p.v1) * ph,
                                                (p.u1 - p.u0) * pw, (p.v1 - p.v0) * ph);
                const CGRect target = CGRectMake(p.left, frame_height - p.bottom, p.right - p.left, p.bottom - p.top);
                CGAffineTransform move = CGAffineTransformMakeTranslation(-shown.origin.x, -shown.origin.y);
                move = CGAffineTransformConcat(move, CGAffineTransformMakeScale(target.size.width / shown.size.width,
                                                                                target.size.height / shown.size.height));
                move = CGAffineTransformConcat(move, CGAffineTransformMakeTranslation(target.origin.x, target.origin.y));
                CIImage* placed = [[[picture imageByCroppingToRect:shown] imageByApplyingTransform:move highQualityDownsample:YES]
                    imageByCroppingToRect:target];
                result = [placed imageByCompositingOverImage:result];
            }
            [context_ render:result toCVPixelBuffer:out bounds:CGRectMake(0, 0, width_, height_) colorSpace:nil];
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

    std::uint64_t camera_frames() const {
        std::lock_guard lock(mutex_);
        std::uint64_t total = 0;
        for (const auto& layer : layers_)
            if (layer.camera) total += layer.camera->frames();
        return total;
    }

    void stop() {
        std::lock_guard lock(mutex_);
        for (auto& layer : layers_)
            if (layer.camera) layer.camera->stop();
        layers_.clear();
        if (pool_) { CVPixelBufferPoolRelease(pool_); pool_ = nullptr; }
        context_ = nil;
    }

private:
    std::uint32_t width_ = 0, height_ = 0;
    CIContext* __strong context_ = nil;
    CVPixelBufferPoolRef pool_ = nullptr;
    mutable std::mutex mutex_;  // Guards the layer list and each layer's visibility and area.
    std::vector<Layer> layers_;
};

Overlays::Overlays() : impl_(std::make_unique<Impl>()) {}
Overlays::~Overlays() = default;
Result Overlays::start(std::uint32_t width, std::uint32_t height, const std::vector<Overlay>& overlays) {
    return impl_->start(width, height, overlays);
}
CVPixelBufferRef Overlays::compose(CVPixelBufferRef base) { return impl_->compose(base); }
Result Overlays::set(std::size_t index, bool visible, const std::optional<Area>& area) { return impl_->set(index, visible, area); }
std::uint64_t Overlays::camera_frames() const { return impl_->camera_frames(); }
void Overlays::stop() { impl_->stop(); }

} // namespace rec::mac
