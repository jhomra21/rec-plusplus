#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rec::win {

struct Monitor {
    HMONITOR handle = nullptr;
    std::string device;
    std::uint32_t width = 0, height = 0;
    bool primary = false;
};

// Primary display first, so display 1 is the main display as on macOS.
std::vector<Monitor> monitors();

} // namespace rec::win
