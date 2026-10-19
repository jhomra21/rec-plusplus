#include "cli/shared_frame.hpp"

#include <atomic>
#include <cstring>
#include <new>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rec::cli {

struct SharedFrame::Header {
    std::atomic<std::uint64_t> sequence;  // Odd while the writer changes the frame.
    std::uint64_t number;
    std::uint32_t width, height;
    std::uint64_t capacity;  // Pixel bytes after the header.
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

namespace {
constexpr std::size_t kHeaderSize = 64;

std::string unique_name() {
    static std::atomic<unsigned> count{0};
#ifdef _WIN32
    return "Local\\rec-preview-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++count);
#else
    // macOS limits shared memory names to 31 characters.
    return "/rec-" + std::to_string(getpid()) + "-" + std::to_string(++count);
#endif
}

#ifdef _WIN32
std::wstring wide(const std::string& text) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}
#endif
} // namespace

SharedFrame::~SharedFrame() { close(); }

SharedFrame::SharedFrame(SharedFrame&& other) noexcept { *this = std::move(other); }

SharedFrame& SharedFrame::operator=(SharedFrame&& other) noexcept {
    if (this != &other) {
        close();
        name_ = std::move(other.name_);
        header_ = std::exchange(other.header_, nullptr);
        size_ = std::exchange(other.size_, 0);
        owner_ = std::exchange(other.owner_, false);
#ifdef _WIN32
        mapping_ = std::exchange(other.mapping_, nullptr);
#endif
    }
    return *this;
}

void SharedFrame::close() {
    if (header_) {
#ifdef _WIN32
        UnmapViewOfFile(header_);
#else
        munmap(header_, size_);
        if (owner_) shm_unlink(name_.c_str());
#endif
    }
#ifdef _WIN32
    if (mapping_) CloseHandle(mapping_);
    mapping_ = nullptr;
#endif
    header_ = nullptr;
    size_ = 0;
    owner_ = false;
}

SharedFrame SharedFrame::create(std::size_t max_pixels, std::string& error) {
    SharedFrame f;
    f.name_ = unique_name();
    f.size_ = kHeaderSize + max_pixels * 4;
    void* view = nullptr;
#ifdef _WIN32
    // The default security of a named object admits only this user (and the system).
    f.mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(f.size_ >> 32),
                                    static_cast<DWORD>(f.size_), wide(f.name_).c_str());
    if (f.mapping_) view = MapViewOfFile(f.mapping_, FILE_MAP_WRITE, 0, 0, f.size_);
#else
    const int fd = shm_open(f.name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0) {
        f.owner_ = true;
        if (ftruncate(fd, static_cast<off_t>(f.size_)) == 0) {
            view = mmap(nullptr, f.size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (view == MAP_FAILED) view = nullptr;
        }
        ::close(fd);
        if (!view) shm_unlink(f.name_.c_str());
    }
#endif
    if (!view) {
        error = "could not share the preview";
        f.close();
        return {};
    }
    f.owner_ = true;
    f.header_ = new (view) Header{};
    f.header_->capacity = max_pixels * 4;
    return f;
}

SharedFrame SharedFrame::open(const std::string& name, std::string& error) {
    SharedFrame f;
    f.name_ = name;
    void* view = nullptr;
#ifdef _WIN32
    f.mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, wide(name).c_str());
    if (f.mapping_) view = MapViewOfFile(f.mapping_, FILE_MAP_READ, 0, 0, 0);
    MEMORY_BASIC_INFORMATION info{};
    if (view && VirtualQuery(view, &info, sizeof info)) f.size_ = info.RegionSize;
#else
    const int fd = shm_open(name.c_str(), O_RDONLY, 0);
    struct stat st{};
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= static_cast<off_t>(kHeaderSize)) {
        f.size_ = static_cast<std::size_t>(st.st_size);
        view = mmap(nullptr, f.size_, PROT_READ, MAP_SHARED, fd, 0);
        if (view == MAP_FAILED) view = nullptr;
    }
    if (fd >= 0) ::close(fd);
#endif
    if (!view || f.size_ < kHeaderSize) {
        error = "could not open the engine's preview";
        f.close();
        return {};
    }
    f.header_ = static_cast<Header*>(view);
    return f;
}

void SharedFrame::write(const PreviewFrame& frame) {
    if (!header_ || !owner_) return;
    const std::size_t bytes = static_cast<std::size_t>(frame.width) * frame.height * 4;
    if (bytes > header_->capacity || frame.pixels.size() < bytes) return;
    const auto sequence = header_->sequence.load(std::memory_order_relaxed);
    header_->sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    header_->width = frame.width;
    header_->height = frame.height;
    header_->number = frame.number;
    std::memcpy(reinterpret_cast<std::uint8_t*>(header_) + kHeaderSize, frame.pixels.data(), bytes);
    header_->sequence.store(sequence + 2, std::memory_order_release);
}

bool SharedFrame::read(PreviewFrame& frame) const {
    if (!header_) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto before = header_->sequence.load(std::memory_order_acquire);
        if (before & 1) {
            std::this_thread::yield();
            continue;
        }
        const auto number = header_->number;
        if (number == 0 || number == frame.number) return false;
        const std::uint32_t width = header_->width, height = header_->height;
        const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
        if (bytes > header_->capacity || kHeaderSize + bytes > size_) return false;
        frame.pixels.resize(bytes);
        std::memcpy(frame.pixels.data(), reinterpret_cast<const std::uint8_t*>(header_) + kHeaderSize, bytes);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (header_->sequence.load(std::memory_order_relaxed) != before) continue;
        frame.width = width;
        frame.height = height;
        frame.number = number;
        return true;
    }
    return false;
}

} // namespace rec::cli
