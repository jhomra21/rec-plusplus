#include "rec/rec.hpp"

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace rec {
namespace {

// Windows keeps each privacy switch as "Allow" or "Deny": the device-wide one under HKLM, the
// per-user one under HKCU, and "Let desktop apps access" under HKCU ...\NonPackaged.
bool denied(HKEY root, const std::wstring& path) {
    wchar_t value[16]{};
    DWORD size = sizeof value;
    if (RegGetValueW(root, path.c_str(), L"Value", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return false;
    return _wcsicmp(value, L"Deny") == 0;
}

} // namespace

Access permission_status(Permission permission) noexcept {
    if (permission == Permission::screen) return Access::granted;
    const std::wstring path = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\") +
                              (permission == Permission::microphone ? L"microphone" : L"webcam");
    if (denied(HKEY_LOCAL_MACHINE, path) || denied(HKEY_CURRENT_USER, path) ||
        denied(HKEY_CURRENT_USER, path + L"\\NonPackaged"))
        return Access::denied;
    return Access::granted;
}

// Desktop apps get no prompt on Windows; only Settings changes the switches.
void request_permission(Permission) {}

Result open_permission_settings(Permission permission) {
    if (permission == Permission::screen) return {"Windows asks no permission to record the screen"};
    const wchar_t* page = permission == Permission::microphone ? L"ms-settings:privacy-microphone" : L"ms-settings:privacy-webcam";
    const auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", page, nullptr, nullptr, SW_SHOWNORMAL));
    if (opened <= 32) return {"could not open Settings"};
    return {};
}

} // namespace rec
