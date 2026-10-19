#include "rec/rec.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>

#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace rec {
namespace {

Access av_status(AVMediaType media) {
    switch ([AVCaptureDevice authorizationStatusForMediaType:media]) {
    case AVAuthorizationStatusAuthorized: return Access::granted;
    case AVAuthorizationStatusDenied: return Access::denied;
    case AVAuthorizationStatusRestricted: return Access::restricted;
    default: return Access::not_determined;
    }
}

} // namespace

// The screen has no "not asked yet" state that can be read without asking.
Access permission_status(Permission permission) noexcept {
    @autoreleasepool {
        if (permission == Permission::screen) return CGPreflightScreenCaptureAccess() ? Access::granted : Access::denied;
        return av_status(permission == Permission::microphone ? AVMediaTypeAudio : AVMediaTypeVideo);
    }
}

void request_permission(Permission permission) {
    @autoreleasepool {
        if (permission == Permission::screen) {
            if (!CGPreflightScreenCaptureAccess()) CGRequestScreenCaptureAccess();
            return;
        }
        AVMediaType media = permission == Permission::microphone ? AVMediaTypeAudio : AVMediaTypeVideo;
        if ([AVCaptureDevice authorizationStatusForMediaType:media] == AVAuthorizationStatusNotDetermined)
            [AVCaptureDevice requestAccessForMediaType:media completionHandler:^(BOOL) {}];
    }
}

Result open_permission_settings(Permission permission) {
    const char* url = permission == Permission::screen
        ? "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"
        : permission == Permission::microphone ? "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"
                                               : "x-apple.systempreferences:com.apple.preference.security?Privacy_Camera";
    // /usr/bin/open needs no AppKit in the engine library.
    char* argv[] = {const_cast<char*>("open"), const_cast<char*>(url), nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr, argv, environ) != 0) return {"could not open System Settings"};
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {"could not open System Settings"};
    return {};
}

} // namespace rec
