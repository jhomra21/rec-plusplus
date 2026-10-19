#include "rec/rec.hpp"

namespace rec {
std::vector<Display> list_displays(std::string& error) {
    error = "screen capture requires macOS 15 or Windows 10 (1903) or newer";
    return {};
}
std::vector<Window> list_windows(std::string& error) {
    error = "window capture requires macOS 15 or Windows 10 (1903) or newer";
    return {};
}
std::vector<AudioDevice> list_microphones(std::string& error) {
    error = "microphone capture requires macOS 15 or Windows 10 (1903) or newer";
    return {};
}
std::vector<Camera> list_cameras(std::string& error) {
    error = "camera capture requires macOS 15 or Windows 10 (1903) or newer";
    return {};
}
Access permission_status(Permission) noexcept { return Access::restricted; }
void request_permission(Permission) {}
Result open_permission_settings(Permission) { return {"capture requires macOS 15 or Windows 10 (1903) or newer"}; }
double default_output_latency_ms() noexcept { return 0; }
Result probe_file(const std::string&, Probe&, std::optional<Region>) { return {"probing recordings requires macOS or Windows"}; }
class Recorder::Impl {};
Recorder::Recorder(Options) : impl_(std::make_unique<Impl>()) {}
Recorder::~Recorder() = default;
Result Recorder::start() { return {"screen capture requires macOS 15 or Windows 10 (1903) or newer"}; }
Result Recorder::stop() { return {}; }
Result Recorder::pause() { return {"not recording"}; }
Result Recorder::resume() { return {"not recording"}; }
bool Recorder::running() const noexcept { return false; }
bool Recorder::finished() const noexcept { return true; }
Stats Recorder::stats() const noexcept { return {}; }
std::string Recorder::failure() const { return {}; }
std::string Recorder::stream_failure() const { return {}; }
Result Recorder::save_replay(const std::string&, double* seconds) {
    if (seconds) *seconds = 0;
    return {"not recording"};
}
Result Recorder::set_overlay(std::size_t, bool, std::optional<Area>) { return {"not recording"}; }
Result Recorder::set_screen(bool, std::optional<Area>) { return {"not recording"}; }
Result Recorder::set_volume(AudioInput, double, bool) { return {"not recording"}; }
Result Recorder::set_layout(std::size_t, std::size_t, const Layout&) { return {"not recording"}; }
Result Recorder::set_style(std::size_t, const Style&) { return {"not recording"}; }
bool Recorder::preview(PreviewFrame&) const { return false; }
bool Recorder::preview(PreviewFrame&, std::size_t) const { return false; }
void Recorder::show_preview(bool) noexcept {}
} // namespace rec
