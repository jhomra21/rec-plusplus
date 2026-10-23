#include "core/qr.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>

namespace rec::core {
namespace {

constexpr int kMaxVersion = 10;
// Level M, by version: error correction codewords per block, and blocks.
constexpr std::array<int, kMaxVersion + 1> kEccPerBlock{0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
constexpr std::array<int, kMaxVersion + 1> kBlocks{0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};

int raw_modules(int version) {
    int n = (16 * version + 128) * version + 64;
    if (version >= 2) {
        const int align = version / 7 + 2;
        n -= (25 * align - 10) * align - 55;
        if (version >= 7) n -= 36;
    }
    return n;
}

int data_codewords(int version) { return raw_modules(version) / 8 - kEccPerBlock[version] * kBlocks[version]; }

std::uint8_t gf_multiply(std::uint8_t a, std::uint8_t b) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((b >> i) & 1) * a;
    }
    return static_cast<std::uint8_t>(z);
}

std::vector<std::uint8_t> rs_divisor(int degree) {
    std::vector<std::uint8_t> result(static_cast<std::size_t>(degree));
    result.back() = 1;
    std::uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (std::size_t j = 0; j < result.size(); ++j) {
            result[j] = gf_multiply(result[j], root);
            if (j + 1 < result.size()) result[j] ^= result[j + 1];
        }
        root = gf_multiply(root, 0x02);
    }
    return result;
}

std::vector<std::uint8_t> rs_remainder(const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& divisor) {
    std::vector<std::uint8_t> result(divisor.size());
    for (std::uint8_t b : data) {
        const std::uint8_t factor = b ^ result.front();
        result.erase(result.begin());
        result.push_back(0);
        for (std::size_t i = 0; i < result.size(); ++i) result[i] ^= gf_multiply(divisor[i], factor);
    }
    return result;
}

class Builder {
public:
    explicit Builder(int version) : version_(version), size_(version * 4 + 17),
        dark_(static_cast<std::size_t>(size_) * size_), function_(dark_.size()) {}

    Qr build(const std::vector<std::uint8_t>& codewords) {
        draw_function_patterns();
        draw_codewords(codewords);
        int best = 0;
        long lowest = LONG_MAX;
        for (int mask = 0; mask < 8; ++mask) {
            apply_mask(mask);
            draw_format(mask);
            if (const long p = penalty(); p < lowest) { lowest = p; best = mask; }
            apply_mask(mask);
        }
        apply_mask(best);
        draw_format(best);
        Qr qr;
        qr.size = size_;
        qr.modules.assign(dark_.begin(), dark_.end());
        return qr;
    }

private:
    std::size_t at(int x, int y) const { return static_cast<std::size_t>(y) * size_ + x; }
    void set_function(int x, int y, bool dark) { dark_[at(x, y)] = dark; function_[at(x, y)] = true; }

    void draw_function_patterns() {
        for (int i = 0; i < size_; ++i) {
            set_function(6, i, i % 2 == 0);
            set_function(i, 6, i % 2 == 0);
        }
        draw_finder(3, 3);
        draw_finder(size_ - 4, 3);
        draw_finder(3, size_ - 4);
        const auto positions = alignment_positions();
        const int n = static_cast<int>(positions.size());
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (!((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)))
                    draw_alignment(positions[i], positions[j]);
        draw_format(0);
        draw_version();
    }

    void draw_finder(int x, int y) {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const int distance = std::max(std::abs(dx), std::abs(dy));
                const int xx = x + dx, yy = y + dy;
                if (xx >= 0 && xx < size_ && yy >= 0 && yy < size_) set_function(xx, yy, distance != 2 && distance != 4);
            }
    }

    void draw_alignment(int x, int y) {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) set_function(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }

    std::vector<int> alignment_positions() const {
        if (version_ == 1) return {};
        const int n = version_ / 7 + 2;
        const int step = (version_ * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
        std::vector<int> result;
        for (int i = 0, pos = size_ - 7; i < n - 1; ++i, pos -= step) result.insert(result.begin(), pos);
        result.insert(result.begin(), 6);
        return result;
    }

    // Level M is 00 in the format bits.
    void draw_format(int mask) {
        const int data = mask;
        int rem = data;
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        const int bits = (data << 10 | rem) ^ 0x5412;
        const auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
        for (int i = 0; i <= 5; ++i) set_function(8, i, bit(i));
        set_function(8, 7, bit(6));
        set_function(8, 8, bit(7));
        set_function(7, 8, bit(8));
        for (int i = 9; i < 15; ++i) set_function(14 - i, 8, bit(i));
        for (int i = 0; i < 8; ++i) set_function(size_ - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i) set_function(8, size_ - 15 + i, bit(i));
        set_function(8, size_ - 8, true);
    }

    void draw_version() {
        if (version_ < 7) return;
        int rem = version_;
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        const long bits = static_cast<long>(version_) << 12 | rem;
        for (int i = 0; i < 18; ++i) {
            const bool dark = ((bits >> i) & 1) != 0;
            const int a = size_ - 11 + i % 3, b = i / 3;
            set_function(a, b, dark);
            set_function(b, a, dark);
        }
    }

    void draw_codewords(const std::vector<std::uint8_t>& data) {
        std::size_t i = 0;
        for (int right = size_ - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vertical = 0; vertical < size_; ++vertical)
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? size_ - 1 - vertical : vertical;
                    if (!function_[at(x, y)] && i < data.size() * 8) {
                        dark_[at(x, y)] = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                        ++i;
                    }
                }
        }
    }

    void apply_mask(int mask) {
        for (int y = 0; y < size_; ++y)
            for (int x = 0; x < size_; ++x) {
                bool invert = false;
                switch (mask) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                default: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (!function_[at(x, y)] && invert) dark_[at(x, y)] = !dark_[at(x, y)];
            }
    }

    // Finder-like runs (1:1:3:1:1 with four light modules on a side) in a line's run history,
    // newest first; the line's edges count as light.
    int finder_count(const std::array<int, 7>& runs) const {
        const int n = runs[1];
        const bool core = n > 0 && runs[2] == n && runs[3] == n * 3 && runs[4] == n && runs[5] == n;
        return (core && runs[0] >= n * 4 && runs[6] >= n ? 1 : 0) + (core && runs[6] >= n * 4 && runs[0] >= n ? 1 : 0);
    }

    void add_run(int length, std::array<int, 7>& runs) const {
        if (runs[0] == 0) length += size_;
        std::copy_backward(runs.begin(), runs.end() - 1, runs.end());
        runs[0] = length;
    }

    long line_penalty(bool horizontal) const {
        long result = 0;
        for (int a = 0; a < size_; ++a) {
            bool color = false;
            int run = 0;
            std::array<int, 7> runs{};
            for (int b = 0; b < size_; ++b) {
                const bool dark = horizontal ? dark_[at(b, a)] : dark_[at(a, b)];
                if (dark == color) {
                    ++run;
                    if (run == 5) result += 3;
                    else if (run > 5) ++result;
                } else {
                    add_run(run, runs);
                    if (!color) result += finder_count(runs) * 40;
                    color = dark;
                    run = 1;
                }
            }
            // The line ends: close the run, then pad with light as if the quiet zone followed.
            if (color) {
                add_run(run, runs);
                run = 0;
            }
            run += size_;
            add_run(run, runs);
            result += finder_count(runs) * 40;
        }
        return result;
    }

    long penalty() const {
        long result = line_penalty(true) + line_penalty(false);
        for (int y = 0; y + 1 < size_; ++y)
            for (int x = 0; x + 1 < size_; ++x) {
                const char c = dark_[at(x, y)];
                if (c == dark_[at(x + 1, y)] && c == dark_[at(x, y + 1)] && c == dark_[at(x + 1, y + 1)]) result += 3;
            }
        const long total = static_cast<long>(size_) * size_;
        const long dark = std::count(dark_.begin(), dark_.end(), char{1});
        const long k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
        return result + k * 10;
    }

    int version_, size_;
    std::vector<char> dark_, function_;
};

} // namespace

Qr qr_code(std::string_view text) {
    int version = 1;
    const auto count_bits = [](int v) { return v <= 9 ? 8 : 16; };
    while (version <= kMaxVersion && 4 + count_bits(version) + 8 * static_cast<long>(text.size()) > data_codewords(version) * 8)
        ++version;
    if (version > kMaxVersion) return {};

    std::vector<bool> bits;
    const auto put = [&bits](unsigned long value, int length) {
        for (int i = length - 1; i >= 0; --i) bits.push_back(((value >> i) & 1) != 0);
    };
    put(0x4, 4);
    put(text.size(), count_bits(version));
    for (unsigned char c : text) put(c, 8);
    const std::size_t capacity = static_cast<std::size_t>(data_codewords(version)) * 8;
    put(0, static_cast<int>(std::min<std::size_t>(4, capacity - bits.size())));
    put(0, static_cast<int>((8 - bits.size() % 8) % 8));
    for (std::uint8_t pad = 0xEC; bits.size() < capacity; pad ^= 0xEC ^ 0x11) put(pad, 8);
    std::vector<std::uint8_t> data(bits.size() / 8);
    for (std::size_t i = 0; i < bits.size(); ++i) data[i >> 3] |= static_cast<std::uint8_t>(bits[i] << (7 - (i & 7)));

    // Split into blocks, add each one's error correction, and interleave.
    const int blocks = kBlocks[version], ecc = kEccPerBlock[version];
    const int raw = raw_modules(version) / 8;
    const int short_blocks = blocks - raw % blocks, short_length = raw / blocks;
    const auto divisor = rs_divisor(ecc);
    std::vector<std::vector<std::uint8_t>> parts;
    for (int i = 0, k = 0; i < blocks; ++i) {
        const int length = short_length - ecc + (i < short_blocks ? 0 : 1);
        std::vector<std::uint8_t> block(data.begin() + k, data.begin() + k + length);
        k += length;
        const auto remainder = rs_remainder(block, divisor);
        if (i < short_blocks) block.push_back(0);
        block.insert(block.end(), remainder.begin(), remainder.end());
        parts.push_back(std::move(block));
    }
    std::vector<std::uint8_t> codewords;
    for (std::size_t i = 0; i < parts[0].size(); ++i)
        for (int j = 0; j < blocks; ++j)
            if (i != static_cast<std::size_t>(short_length - ecc) || j >= short_blocks) codewords.push_back(parts[j][i]);
    return Builder(version).build(codewords);
}

} // namespace rec::core
