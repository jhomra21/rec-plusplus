#pragma once

#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "rec/rec.hpp"

#include <cstdint>
#include <string>

namespace rec::mac {

struct PixelSize { std::uint32_t width, height; };

// Displays and windows ScreenCaptureKit can capture. Null, with error set, on failure.
SCShareableContent* shareable(std::string& error);
// A display's size in pixels.
PixelSize native_size(SCDisplay* display);
// Pixels per point of the display that shows most of a rectangle in global points.
double pixels_per_point(SCShareableContent* content, CGRect frame);
std::string message(NSError* error, const char* fallback);
// Starts or stops a stream, waiting up to 15 s.
Result wait_capture(SCStream* stream, bool start);

} // namespace rec::mac
