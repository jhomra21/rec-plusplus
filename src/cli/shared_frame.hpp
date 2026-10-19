#pragma once

#include "rec/rec.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

// The engine's preview frames, shared with a client process through named shared memory:
// the engine writes the newest frame, the client copies it out. A sequence number that is
// odd while a frame is being written lets the reader retry instead of taking a torn frame.
namespace rec::cli {

class SharedFrame {
public:
    SharedFrame() = default;
    ~SharedFrame();
    SharedFrame(SharedFrame&& other) noexcept;
    SharedFrame& operator=(SharedFrame&& other) noexcept;
    SharedFrame(const SharedFrame&) = delete;
    SharedFrame& operator=(const SharedFrame&) = delete;

    // A new block, readable and writable by this user only, for frames of up to
    // max_pixels pixels. The name is unique to this process.
    static SharedFrame create(std::size_t max_pixels, std::string& error);
    // The block a create() elsewhere named, read-only.
    static SharedFrame open(const std::string& name, std::string& error);

    bool valid() const { return header_ != nullptr; }
    const std::string& name() const { return name_; }
    // Skipped when the frame is larger than the block.
    void write(const PreviewFrame& frame);
    // Copies the newest frame into frame when its number differs from frame.number.
    bool read(PreviewFrame& frame) const;

private:
    struct Header;
    void close();

    std::string name_;
    Header* header_ = nullptr;
    std::size_t size_ = 0;
    bool owner_ = false;
#ifdef _WIN32
    void* mapping_ = nullptr;
#endif
};

} // namespace rec::cli
