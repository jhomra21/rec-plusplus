#pragma once

#include <string_view>
#include <vector>

namespace rec::core {

// A QR code (ISO/IEC 18004) of text in byte mode at error correction level M, versions 1 to
// 10 (up to 213 bytes), with the mask of least penalty. Row-major, true for dark; no quiet
// zone. Empty when the text is too long.
struct Qr {
    int size = 0;
    std::vector<bool> modules;
    bool dark(int x, int y) const { return modules[static_cast<std::size_t>(y) * size + x]; }
};

Qr qr_code(std::string_view text);

} // namespace rec::core
