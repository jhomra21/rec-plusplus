#pragma once

#include "rec/rec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// Camera shapes, borders and drop shadows. The shapes, their roundness, the border width at
// 1080p and the shadow's size follow rec-rs (michaelshimeles/rec-rs d0d4126, src/compose.rs);
// the shadow here is computed from the distance to the edge instead of blurred.
namespace rec::core {

inline constexpr std::array<const char*, 6> kShapeNames{"rectangle", "rounded", "circle", "square", "pill", "squircle"};

inline const char* shape_name(Style::Shape shape) { return kShapeNames[static_cast<std::size_t>(shape)]; }

inline bool square_shape(Style::Shape s) {
    return s == Style::Shape::circle || s == Style::Shape::square || s == Style::Shape::squircle;
}

inline bool uses_roundness(Style::Shape s) {
    return s == Style::Shape::rounded || s == Style::Shape::square || s == Style::Shape::squircle;
}

inline bool plain(const Style& s) { return s.shape == Style::Shape::rectangle && s.border <= 0 && !s.shadow; }

// The part of area (fractions of a width x height canvas) a style fills. A square shape takes
// the largest square, against the side nearer the canvas's edge, or centered between them.
inline Area shaped_area(const Style& style, const Area& a, std::uint32_t width, std::uint32_t height) {
    if (!square_shape(style.shape) || !width || !height) return a;
    const double w = a.width * width, h = a.height * height, side = std::min(w, h);
    Area out = a;
    out.width = side / width;
    out.height = side / height;
    const auto align = [](double start, double length, double size) {
        const double before = start, after = 1 - start - length;
        if (std::abs(before - after) < 1e-3) return start + (length - size) / 2;
        return before < after ? start : start + length - size;
    };
    if (w > side) out.x = align(a.x, a.width, out.width);
    if (h > side) out.y = align(a.y, a.height, out.height);
    return out;
}

// Pixel sizes in a canvas height pixels tall.
struct Decor {
    double border = 0;  // Border width.
    double pad = 0;     // How far the shadow reaches past the shape.
    double drop = 0;    // How far the shadow sits below it.
};
inline constexpr double kShadowOpacity = 0.45;

inline Decor decor(const Style& s, std::uint32_t height) {
    Decor d;
    d.border = std::round(std::clamp(s.border, 0.0, 40.0) * height / 1080.0);
    if (s.shadow) {
        d.pad = std::max(3.0, std::round(height * 0.03));
        d.drop = std::round(height * 0.006);
    }
    return d;
}

// Superellipse exponent: 2 (a circle) at roundness 1, 4 (the classic squircle) at 0.5,
// 10 (nearly square) at 0.
inline double squircle_exponent(double roundness) {
    const double r = 1 - std::clamp(roundness, 0.0, 1.0);
    return 2 + 8 * r * r;
}

// Approximate signed distance in pixels (negative inside) from x, y to the edge of the shape
// filling a w x h box whose top-left corner is at 0, 0. The overlay shader mirrors this.
inline double shape_distance(Style::Shape shape, double roundness, double w, double h, double x, double y) {
    const double hw = w / 2, hh = h / 2, least = std::min(hw, hh);
    if (hw <= 0 || hh <= 0) return 1;
    const double ax = std::abs(x - hw), ay = std::abs(y - hh);
    switch (shape) {
    case Style::Shape::squircle: {
        const double n = squircle_exponent(roundness);
        return (std::pow(std::pow(ax / hw, n) + std::pow(ay / hh, n), 1 / n) - 1) * least;
    }
    case Style::Shape::circle:
        return (std::sqrt((ax / hw) * (ax / hw) + (ay / hh) * (ay / hh)) - 1) * least;
    default: {
        const double r = shape == Style::Shape::pill ? least
                         : shape == Style::Shape::rectangle ? 0
                                                            : std::clamp(roundness, 0.0, 1.0) * least;
        const double qx = ax - (hw - r), qy = ay - (hh - r);
        const double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
        return outside + std::min(std::max(qx, qy), 0.0) - r;
    }
    }
}

inline double coverage(double distance) { return std::clamp(0.5 - distance, 0.0, 1.0); }

inline double shadow_alpha(double distance, double pad) {
    const double t = std::clamp((distance + pad) / (2 * pad), 0.0, 1.0);
    return kShadowOpacity * (1 - t * t * (3 - 2 * t));
}

// Premultiplied BGRA pictures of a style for a camera drawn w x h pixels: what to draw
// before the camera (the shadow, padded by Decor::pad on every side and moved down by
// Decor::drop, then the border filling the shape) and the camera's coverage inside the
// border. Empty where a part is off.
enum class DecorPart { shadow, border, inside };
inline std::vector<std::uint8_t> decor_pixels(const Style& s, const Decor& d, DecorPart part, int w, int h) {
    const double inset = part == DecorPart::inside ? d.border : 0, pad = part == DecorPart::shadow ? d.pad : 0;
    if ((part == DecorPart::shadow && !s.shadow) || (part == DecorPart::border && d.border <= 0) || w <= 0 || h <= 0) return {};
    const int pw = w + static_cast<int>(2 * pad), ph = h + static_cast<int>(2 * pad);
    const double bw = w - 2 * inset, bh = h - 2 * inset;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(pw) * ph * 4, 0);
    const double red = (s.border_color >> 16 & 0xff) / 255.0, green = (s.border_color >> 8 & 0xff) / 255.0,
                 blue = (s.border_color & 0xff) / 255.0;
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) {
            const double dist = shape_distance(s.shape, s.roundness, bw, bh, x + 0.5 - pad - inset, y + 0.5 - pad - inset);
            const double a = part == DecorPart::shadow ? shadow_alpha(dist, pad) : bw > 0 && bh > 0 ? coverage(dist) : 0;
            auto* px = &out[(static_cast<std::size_t>(y) * pw + x) * 4];
            const double r = part == DecorPart::border ? red : part == DecorPart::inside ? 1 : 0;
            const double g = part == DecorPart::border ? green : part == DecorPart::inside ? 1 : 0;
            const double b = part == DecorPart::border ? blue : part == DecorPart::inside ? 1 : 0;
            px[0] = static_cast<std::uint8_t>(std::lround(b * a * 255));
            px[1] = static_cast<std::uint8_t>(std::lround(g * a * 255));
            px[2] = static_cast<std::uint8_t>(std::lround(r * a * 255));
            px[3] = static_cast<std::uint8_t>(std::lround(a * 255));
        }
    return out;
}

inline std::string check_style(const Style& s) {
    if (!std::isfinite(s.roundness) || s.roundness < 0 || s.roundness > 1) return "roundness must be between 0 and 1";
    if (!std::isfinite(s.border) || s.border < 0 || s.border > 40) return "the border must be 0 to 40 pixels";
    if (s.border_color > 0xffffff) return "the border color must be RRGGBB";
    return {};
}

// SHAPE[:ROUNDNESS][,border=PIXELS[:RRGGBB]][,shadow], for example circle,border=6,shadow.
inline std::string style_text(const Style& s) {
    char text[96];
    int n = std::snprintf(text, sizeof text, "%s", shape_name(s.shape));
    if (uses_roundness(s.shape)) n += std::snprintf(text + n, sizeof text - n, ":%g", s.roundness);
    if (s.border > 0) n += std::snprintf(text + n, sizeof text - n, ",border=%g:%06x", s.border, s.border_color);
    if (s.shadow) std::snprintf(text + n, sizeof text - n, ",shadow");
    return text;
}

inline bool parse_style(std::string_view text, Style& out, std::string& error) {
    Style s;
    const auto number = [](std::string_view v, double& to) {
        const std::string copy(v);
        char* end = nullptr;
        to = std::strtod(copy.c_str(), &end);
        return !copy.empty() && end == copy.c_str() + copy.size() && std::isfinite(to);
    };
    const auto bad = [&](const std::string& why) {
        error = why + "; use SHAPE[:ROUNDNESS][,border=PIXELS[:RRGGBB]][,shadow] with a shape of rectangle, rounded, "
                      "circle, square, pill or squircle";
        return false;
    };
    bool first = true;
    while (!text.empty() || first) {
        const auto comma = text.find(',');
        const auto word = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        if (first) {
            first = false;
            const auto colon = word.find(':');
            const auto name = word.substr(0, colon);
            const auto found = std::find_if(kShapeNames.begin(), kShapeNames.end(), [&](const char* n) { return name == n; });
            if (found == kShapeNames.end()) return bad("unknown shape '" + std::string(name) + "'");
            s.shape = static_cast<Style::Shape>(found - kShapeNames.begin());
            if (colon != std::string_view::npos && !number(word.substr(colon + 1), s.roundness)) return bad("bad roundness");
        } else if (word == "shadow") {
            s.shadow = true;
        } else if (word.rfind("border=", 0) == 0) {
            auto value = word.substr(7);
            const auto colon = value.find(':');
            if (colon != std::string_view::npos) {
                const auto hex = std::string(value.substr(colon + 1));
                char* end = nullptr;
                const auto color = std::strtoul(hex.c_str(), &end, 16);
                if (hex.size() != 6 || end != hex.c_str() + 6) return bad("the border color must be RRGGBB");
                s.border_color = static_cast<std::uint32_t>(color);
                value = value.substr(0, colon);
            }
            if (!number(value, s.border)) return bad("bad border width");
        } else {
            return bad("unknown style part '" + std::string(word) + "'");
        }
    }
    if (auto why = check_style(s); !why.empty()) return bad(why);
    out = s;
    return true;
}

} // namespace rec::core
