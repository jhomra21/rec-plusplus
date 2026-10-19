// The Windows app: one Win32 window of standard controls (accessible to screen readers and
// the keyboard as they are), a notification-area icon, and a global hotkey. Recording runs in
// rec::app::Session; this file only presents it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincred.h>
// Defines the accessibility annotation GUIDs (CLSID_AccPropServices, PROPID_ACC_*) here.
#include <initguid.h>
#include <oleacc.h>

#include "app/session.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
using rec::app::Session;
using rec::app::Settings;
using rec::app::State;
using rec::app::Status;

constexpr UINT kSessionChanged = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 2;
constexpr UINT_PTR kPollTimer = 1, kPreviewTimer = 2;
constexpr int kHotkey = 1, kReplayHotkey = 2;
constexpr UINT kTrayId = 1;
// Sent as the WM_COMMAND code for the Source list once its selection change is handled.
constexpr int kPickRegion = 0x7001;

enum Control : int {
    kStatus = 100, kDetail, kLive, kSystemLabel, kSystemMeter, kSystemVolume, kSystemVolumeText, kSystemMute,
    kMicLabel, kMicMeter, kMicVolume, kMicVolumeText, kMicMute,
    kRecord, kPause, kSaveReplay,
    kCaptureHeader, kSourceLabel, kSource, kSystemAudio, kMicrophone, kMicDevice, kCursor, kClicks,
    kQualityHeader, kResolutionLabel, kResolution, kFpsLabel, kFps, kCodecLabel, kCodec, kFormatLabel, kFormat,
    kStreamHeader, kStream, kServiceLabel, kService, kServerLabel, kServer, kKeyLabel, kKey, kBitrateLabel, kBitrate,
    kService2Label, kService2, kServer2Label, kServer2, kKey2Label, kKey2,
    kReplayHeader, kReplay, kReplayLength,
    kCameraHeader, kCamera, kCameraDevice, kPositionLabel, kCorner, kSizeLabel, kCameraSize, kSceneLabel, kScene,
    kImage, kImageFile, kBrowseImage,
    kFolderHeader, kFolder, kChangeFolder, kOpenFolder, kShowLast, kPreview, kShowPreview,
    kScenesHeader, kNewScene, kDeleteScene, kSceneNameLabel, kSceneName, kSourcesLabel, kSceneSources, kSourceShown,
    kAddDisplay, kAddWindow, kRemoveSource,
};

enum Command : UINT {
    kMenuRecord = 1000, kMenuPause, kMenuShow, kMenuFolder, kMenuQuit, kMenuReplay, kMenuMuteSystem, kMenuMuteMic,
    // The Add Display and Add Window menus, then one command per scene.
    kMenuAddDisplay = 1100, kMenuAddWindow = 1200, kMenuScene = 2000
};
constexpr std::size_t kMaxMenuDisplays = 100, kMaxMenuWindows = 800, kMaxMenuScenes = 999;

using rec::app::choice_index;
using rec::app::kCameraSizes;
using rec::app::kCodecs;
using rec::app::kCorners;
using rec::app::kFormats;
using rec::app::kFrameRates;
using rec::app::kResolutions;
using rec::app::kSecondServices;
using rec::app::kServices;

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring environment(const wchar_t* name) {
    wchar_t buffer[1024];
    const DWORD n = GetEnvironmentVariableW(name, buffer, 1024);
    return n && n < 1024 ? std::wstring(buffer, n) : std::wstring();
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::wstring text_of(HWND control) {
    std::wstring out(static_cast<std::size_t>(GetWindowTextLengthW(control)), L'\0');
    if (!out.empty()) GetWindowTextW(control, out.data(), static_cast<int>(out.size()) + 1);
    return out;
}

// "App - Title", shortened for a menu or list.
std::wstring window_label(const std::string& app, const std::string& title) {
    std::wstring text = wide(title.empty() ? app : app.empty() ? title : app + " - " + title);
    if (text.size() > 70) text = text.substr(0, 69) + L"\u2026";
    return text;
}

std::wstring duration_label(std::uint32_t seconds) {
    return seconds < 60 ? std::to_wstring(seconds) + L" seconds"
                        : std::to_wstring(seconds / 60) + (seconds == 60 ? L" minute" : L" minutes");
}

std::wstring decibels(int db) { return (db > 0 ? L"+" : L"") + std::to_wstring(db) + L" dB"; }

std::wstring clock(double seconds) {
    const auto whole = static_cast<long long>(seconds);
    wchar_t out[32];
    if (whole >= 3600) std::swprintf(out, 32, L"%lld:%02lld:%02lld", whole / 3600, whole / 60 % 60, whole % 60);
    else std::swprintf(out, 32, L"%02lld:%02lld", whole / 60, whole % 60);
    return out;
}

// Stream keys live in Windows Credential Manager, one generic credential per server URL,
// never in the settings file.
std::wstring key_target(const std::string& server) { return L"rec/stream/" + wide(server); }

std::string load_key(const std::string& server) {
    PCREDENTIALW credential = nullptr;
    if (server.empty() || !CredReadW(key_target(server).c_str(), CRED_TYPE_GENERIC, 0, &credential)) return {};
    std::string key(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    CredFree(credential);
    return key;
}

void store_key(const std::string& server, const std::string& key) {
    if (server.empty() || key.empty() || load_key(server) == key) return;
    auto target = key_target(server);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.CredentialBlobSize = static_cast<DWORD>(key.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(key.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    wchar_t user[] = L"stream key";
    credential.UserName = user;
    CredWriteW(&credential, 0);
}

// A filled circle with an anti-aliased edge, for the window and notification-area icons.
HICON dot_icon(int size, COLORREF color, bool ring) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof header;
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color_bitmap = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color_bitmap) return nullptr;
    auto* pixels = static_cast<std::uint32_t*>(bits);
    const double center = size / 2.0, outer = size * 0.42, inner = size * 0.26;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double d = std::hypot(x + 0.5 - center, y + 0.5 - center);
            double alpha = std::clamp(outer - d + 0.5, 0.0, 1.0);
            if (ring) alpha = std::min(alpha, std::clamp(d - inner + 0.5, 0.0, 1.0));
            const auto a = static_cast<std::uint32_t>(alpha * 255);
            pixels[y * size + x] = a << 24 | static_cast<std::uint32_t>(GetRValue(color)) << 16 |
                                   static_cast<std::uint32_t>(GetGValue(color)) << 8 | GetBValue(color);
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{TRUE, 0, 0, mask, color_bitmap};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color_bitmap);
    return icon;
}

// A dimmed overlay over every display: drag a rectangle to record it; Esc, a right click or
// switching away cancels. The rectangle is a color-keyed hole, so its content shows undimmed.
struct RegionPick {
    bool done = false, dragging = false, chosen = false;
    POINT from{}, to{};
    int dpi = 96;
};
constexpr COLORREF kHole = RGB(255, 0, 255);

RECT span(POINT a, POINT b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)}; }

LRESULT CALLBACK region_procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE)
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* pick = reinterpret_cast<RegionPick*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!pick) return DefWindowProcW(window, message, w, l);
    const POINT at{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (message) {
    case WM_LBUTTONDOWN:
        pick->dragging = true;
        pick->from = pick->to = at;
        SetCapture(window);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (pick->dragging) { pick->to = at; InvalidateRect(window, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (pick->dragging) {
            pick->to = at;
            pick->dragging = false;
            pick->chosen = pick->done = true;
            ReleaseCapture();
        }
        return 0;
    case WM_RBUTTONDOWN:
        pick->done = true;
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) pick->done = true;
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !pick->chosen) pick->done = true;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT client;
        GetClientRect(window, &client);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old_bitmap = SelectObject(buffer, bitmap);
        FillRect(buffer, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        HFONT font = CreateFontW(-MulDiv(16, pick->dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ old_font = SelectObject(buffer, font);
        SetBkMode(buffer, TRANSPARENT);
        SetTextColor(buffer, RGB(255, 255, 255));
        if (pick->dragging) {
            const RECT r = span(pick->from, pick->to);
            HBRUSH hole = CreateSolidBrush(kHole);
            FillRect(buffer, &r, hole);
            DeleteObject(hole);
            RECT frame = r;
            for (int i = 0; i < 2; ++i) {
                InflateRect(&frame, 1, 1);
                FrameRect(buffer, &frame, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            }
            wchar_t size[48];
            std::swprintf(size, 48, L"%ld \u00D7 %ld", r.right - r.left, r.bottom - r.top);
            RECT label{r.left, r.bottom + MulDiv(6, pick->dpi, 96), r.left + MulDiv(300, pick->dpi, 96),
                       r.bottom + MulDiv(40, pick->dpi, 96)};
            DrawTextW(buffer, size, -1, &label, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            // Centered on the main display, whose top-left is the screen origin.
            RECT main{-GetSystemMetrics(SM_XVIRTUALSCREEN), -GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
            main.right = main.left + GetSystemMetrics(SM_CXSCREEN);
            main.bottom = main.top + GetSystemMetrics(SM_CYSCREEN);
            DrawTextW(buffer, L"Drag to select the area to record. Esc cancels.", -1, &main,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(buffer, old_font);
        DeleteObject(font);
        BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, w, l);
    }
}

// The dragged rectangle in screen pixels, or nothing when cancelled or too small.
std::optional<RECT> pick_region(HINSTANCE instance, UINT dpi) {
    static const ATOM kind = [instance] {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = region_procedure;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpszClassName = L"RecRegionPicker";
        return RegisterClassExW(&wc);
    }();
    if (!kind) return std::nullopt;
    RegionPick pick;
    pick.dpi = static_cast<int>(dpi);
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    HWND overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"RecRegionPicker",
                                   L"Select a region to record", WS_POPUP, x, y, GetSystemMetrics(SM_CXVIRTUALSCREEN),
                                   GetSystemMetrics(SM_CYVIRTUALSCREEN), nullptr, nullptr, instance, &pick);
    if (!overlay) return std::nullopt;
    SetLayeredWindowAttributes(overlay, kHole, 150, LWA_ALPHA | LWA_COLORKEY);
    ShowWindow(overlay, SW_SHOW);
    SetForegroundWindow(overlay);
    SetFocus(overlay);
    MSG message;
    while (!pick.done) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(overlay);
    if (!pick.chosen) return std::nullopt;
    RECT r = span(pick.from, pick.to);
    OffsetRect(&r, x, y);
    if (r.right - r.left < 16 || r.bottom - r.top < 16) return std::nullopt;
    return r;
}

class App {
public:
    int run(HINSTANCE instance, int show) {
        instance_ = instance;
        home_ = environment(L"REC_APP_HOME");
        test_pattern_ = environment(L"REC_APP_TEST_PATTERN") == L"1";
        settings_path_ = narrow((home_.empty() ? known_folder(FOLDERID_RoamingAppData) + L"\\rec" : home_) + L"\\settings.conf");
        settings_ = rec::app::load_settings(settings_path_);
        if (settings_.folder.empty())
            settings_.folder = narrow(home_.empty() ? known_folder(FOLDERID_Videos) : home_ + L"\\recordings");
        // Device discovery initializes COM for itself; keep it off this STA thread.
        std::thread([this] {
            std::string error;
            displays_ = rec::list_displays(error);
            microphones_ = rec::list_microphones(error);
            cameras_ = {{"default", "Default camera"}};
            for (auto& camera : rec::list_cameras(error)) cameras_.push_back(std::move(camera));
        }).join();
        if (test_pattern_) cameras_.push_back({"test", "Test camera"});
        if (std::none_of(cameras_.begin(), cameras_.end(), [this](const rec::Camera& c) { return c.id == settings_.camera_device; }))
            cameras_.push_back({settings_.camera_device, settings_.camera_device + " (not connected)"});

        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = &App::procedure;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"RecWindow";
        RegisterClassExW(&wc);
        WNDCLASSEXW preview{sizeof preview};
        preview.lpfnWndProc = &App::preview_procedure;
        preview.hInstance = instance;
        preview.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        preview.lpszClassName = L"RecPreview";
        RegisterClassExW(&preview);
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Rec",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 480, 760, nullptr, nullptr, instance, this);
        if (!window_) return 1;
        ShowWindow(window_, show);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (IsDialogMessageW(window_, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_NCCREATE) {
            auto* app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->window_ = window;
        }
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return app ? app->handle(message, w, l) : DefWindowProcW(window, message, w, l);
    }

    static LRESULT CALLBACK preview_procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(window, &paint);
            if (app) app->paint_preview(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            if (app) app->paint_preview(window, reinterpret_cast<HDC>(w));
            return 0;
        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
        case WM_MOUSELEAVE:
        case WM_CAPTURECHANGED:
            if (app) app->preview_mouse(window, message, l);
            return 0;
        case WM_SETCURSOR:
            if (app && LOWORD(l) == HTCLIENT && app->preview_cursor(window)) return TRUE;
            return DefWindowProcW(window, message, w, l);
        default:
            return DefWindowProcW(window, message, w, l);
        }
    }

    // Where the frame is drawn in the preview: the output's shape, centered.
    RECT frame_rect(HWND window) const {
        RECT area;
        GetClientRect(window, &area);
        if (!frame_.number) return {};
        const int width = area.right, height = area.bottom;
        int w = width, h = static_cast<int>(static_cast<long long>(width) * frame_.height / frame_.width);
        if (h > height) {
            h = height;
            w = static_cast<int>(static_cast<long long>(height) * frame_.width / frame_.height);
        }
        const int x = (width - w) / 2, y = (height - h) / 2;
        return {x, y, x + w, y + h};
    }

    // The pointer at l in frame fractions, and the corner handle's size in them.
    struct FramePoint { double x, y, handle_x, handle_y; };
    std::optional<FramePoint> frame_point(HWND window, LPARAM l) const {
        const RECT r = frame_rect(window);
        const double w = r.right - r.left, h = r.bottom - r.top;
        if (w <= 0 || h <= 0) return std::nullopt;
        return FramePoint{(GET_X_LPARAM(l) - r.left) / w, (GET_Y_LPARAM(l) - r.top) / h, px(12) / w, px(12) / h};
    }

    // The source a press at l takes hold of in the preview: the topmost one shown there, in
    // this scene, before Start or in this recording.
    struct Hit {
        rec::app::SourceRef source;
        rec::app::AreaDrag drag;
    };
    std::optional<Hit> hit_source(HWND window, LPARAM l) const {
        if (!frame_.number || (state_ != State::idle && state_ != State::recording && state_ != State::paused))
            return std::nullopt;
        const auto p = frame_point(window, l);
        if (!p) return std::nullopt;
        const auto source = rec::app::source_at(settings_, p->x, p->y, p->handle_x, p->handle_y);
        if (!source) return std::nullopt;
        const auto& item = rec::app::scene_item(rec::app::current_scene(settings_), *source);
        const auto drag = rec::app::grab_area(item.area, p->x, p->y, p->handle_x, p->handle_y,
                                              source->kind == rec::app::SourceRef::Kind::camera);
        if (drag.grab == rec::app::AreaDrag::Grab::none) return std::nullopt;
        return Hit{*source, drag};
    }

    // Dragging a source in the preview moves it, dragging a corner resizes it; the recording
    // or the preview before Start follows at once.
    void preview_mouse(HWND window, UINT message, LPARAM l) {
        using Grab = rec::app::AreaDrag::Grab;
        switch (message) {
        case WM_LBUTTONDOWN: {
            const auto hit = hit_source(window, l);
            if (!hit) return;
            drag_ = hit->drag;
            dragged_ = hit->source;
            select_source(hit->source);
            SetCapture(window);
            outline_ = drag_.from;
            InvalidateRect(window, nullptr, FALSE);
            return;
        }
        case WM_MOUSEMOVE:
            if (drag_.grab != Grab::none) {
                const auto p = frame_point(window, l);
                if (!p) return;
                outline_ = rec::app::drag_area(drag_, p->x, p->y);
                rec::app::scene_item(rec::app::current_scene(settings_), dragged_).area = outline_;
                if (dragged_.kind == rec::app::SourceRef::Kind::camera) show_camera_place();
                session_.show_scene(settings_);
                InvalidateRect(window, nullptr, FALSE);
                return;
            }
            if (const auto hit = hit_source(window, l); (hit ? std::optional(hit->source) : std::nullopt) != hover_) {
                hover_ = hit ? std::optional(hit->source) : std::nullopt;
                if (hover_) {
                    TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, window, 0};
                    TrackMouseEvent(&track);
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            return;
        case WM_MOUSELEAVE:
            hover_.reset();
            InvalidateRect(window, nullptr, FALSE);
            return;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (drag_.grab == Grab::none) return;
            drag_ = {};
            if (message == WM_LBUTTONUP) ReleaseCapture();
            rec::app::save_settings(settings_path_, settings_);
            InvalidateRect(window, nullptr, FALSE);
            refresh();
            return;
        default:
            return;
        }
    }

    bool preview_cursor(HWND window) {
        using Grab = rec::app::AreaDrag::Grab;
        POINT at;
        GetCursorPos(&at);
        ScreenToClient(window, &at);
        rec::app::AreaDrag grab = drag_;
        if (grab.grab == Grab::none)
            if (const auto hit = hit_source(window, MAKELPARAM(at.x, at.y))) grab = hit->drag;
        if (grab.grab == Grab::none) return false;
        const LPCWSTR shape = grab.grab == Grab::move ? IDC_SIZEALL : grab.left == grab.top ? IDC_SIZENWSE : IDC_SIZENESW;
        SetCursor(LoadCursorW(nullptr, shape));
        return true;
    }
    // The newest frame in the output's shape, centered on dark gray.
    void paint_preview(HWND window, HDC dc) {
        RECT area;
        GetClientRect(window, &area);
        const int saved = SaveDC(dc);
        if (frame_.number) {
            const RECT frame = frame_rect(window);
            const int x = frame.left, y = frame.top, w = frame.right - frame.left, h = frame.bottom - frame.top;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof info.bmiHeader;
            info.bmiHeader.biWidth = static_cast<LONG>(frame_.width);
            info.bmiHeader.biHeight = -static_cast<LONG>(frame_.height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, x, y, w, h, 0, 0, static_cast<int>(frame_.width), static_cast<int>(frame_.height),
                          frame_.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
            // The outline of the source held or under the pointer.
            const auto camera = drag_.grab != rec::app::AreaDrag::Grab::none ? std::optional<rec::Area>(outline_)
                              : hover_ ? std::optional(rec::app::scene_item(rec::app::current_scene(settings_), *hover_).area)
                                       : std::nullopt;
            if (camera) {
                RECT box{x + static_cast<int>(std::lround(camera->x * w)), y + static_cast<int>(std::lround(camera->y * h)),
                         x + static_cast<int>(std::lround((camera->x + camera->width) * w)),
                         y + static_cast<int>(std::lround((camera->y + camera->height) * h))};
                FrameRect(dc, &box, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                InflateRect(&box, -1, -1);
                FrameRect(dc, &box, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                InflateRect(&box, -1, -1);
                FrameRect(dc, &box, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            ExcludeClipRect(dc, x, y, x + w, y + h);
        }
        FillRect(dc, &area, preview_brush_);
        if (!frame_.number) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(160, 160, 168));
            SelectObject(dc, font_);
            InflateRect(&area, -px(16), 0);
            DrawTextW(dc, preview_hint_.c_str(), -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        RestoreDC(dc, saved);
    }

    LRESULT handle(UINT message, WPARAM w, LPARAM l) {
        switch (message) {
        case WM_CREATE:
            create();
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(l);
            dpi_ = HIWORD(w);
            make_fonts();
            layout();
            SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            fit();
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            auto dc = reinterpret_cast<HDC>(w);
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, color_for(id));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_COMMAND:
            command(LOWORD(w), HIWORD(w));
            return 0;
        case WM_HSCROLL: {
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
            if (id == kSystemVolume || id == kMicVolume)
                change_volume(id == kSystemVolume ? rec::AudioInput::system : rec::AudioInput::microphone,
                              LOWORD(w) == TB_ENDTRACK);
            return 0;
        }
        case WM_TIMER:
            if (w == kPollTimer) refresh();
            else if (w == kPreviewTimer && session_.preview(frame_)) InvalidateRect(get(kPreview), nullptr, FALSE);
            return 0;
        case kSessionChanged:
            refresh();
            return 0;
        case WM_SHOWWINDOW:
        case WM_SIZE:
            // Visibility settles after these; the preview follows it.
            PostMessageW(window_, kSessionChanged, 0, 0);
            return DefWindowProcW(window_, message, w, l);
        case WM_HOTKEY:
            if (w == kHotkey) toggle_record();
            else if (w == kReplayHotkey) save_replay();
            return 0;
        case kTrayMessage:
            tray_event(LOWORD(l));
            return 0;
        case WM_CLOSE:
            // Closing the window never stops a recording: it keeps going in the notification area.
            if (busy()) ShowWindow(window_, SW_HIDE);
            else quit();
            return 0;
        case WM_QUERYENDSESSION:
            // Windows names the app on its "apps are preventing log off" screen while the movie is saved.
            if (busy()) ShutdownBlockReasonCreate(window_, L"Saving your recording");
            return TRUE;
        case WM_ENDSESSION:
            if (w) {
                session_.stop();
                session_.wait();
                remember();
            }
            ShutdownBlockReasonDestroy(window_);
            return 0;
        case WM_DESTROY:
            if (access_) {
                const MSAAPROPID props[]{PROPID_ACC_NAME, PROPID_ACC_VALUE};
                for (int slider : {kSystemVolume, kMicVolume})
                    access_->ClearHwndProps(get(slider), OBJID_CLIENT, CHILDID_SELF, props, 2);
                access_->Release();
                access_ = nullptr;
            }
            UnregisterHotKey(window_, kHotkey);
            UnregisterHotKey(window_, kReplayHotkey);
            Shell_NotifyIconW(NIM_DELETE, &tray_);
            DeleteObject(preview_brush_);
            PostQuitMessage(0);
            return 0;
        default:
            if (message == taskbar_created_ && taskbar_created_) add_tray();
            return DefWindowProcW(window_, message, w, l);
        }
    }

    int px(int dips) const { return MulDiv(dips, static_cast<int>(dpi_), 96); }

    void make_fonts() {
        for (HFONT* font : {&font_, &bold_, &title_})
            if (*font) { DeleteObject(*font); *font = nullptr; }
        NONCLIENTMETRICSW metrics{sizeof metrics};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0, dpi_);
        LOGFONTW base = metrics.lfMessageFont;
        font_ = CreateFontIndirectW(&base);
        LOGFONTW bold = base;
        bold.lfWeight = FW_SEMIBOLD;
        bold_ = CreateFontIndirectW(&bold);
        LOGFONTW title = bold;
        title.lfHeight = -MulDiv(18, static_cast<int>(dpi_), 72);
        title_ = CreateFontIndirectW(&title);
        for (auto& [id, hwnd] : controls_) {
            const bool header = id == kCaptureHeader || id == kQualityHeader || id == kStreamHeader || id == kReplayHeader ||
                                id == kCameraHeader || id == kFolderHeader || id == kScenesHeader;
            const HFONT font = id == kStatus ? title_ : header || id == kLive ? bold_ : font_;
            SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
    }

    HWND add(int id, const wchar_t* type, const wchar_t* text, DWORD style, DWORD extended = 0) {
        HWND hwnd = CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        controls_.push_back({id, hwnd});
        return hwnd;
    }
    HWND get(int id) const { return GetDlgItem(window_, id); }
    // Status texts can contain "&"; field labels use it for their access key.
    HWND label(int id, const wchar_t* text) {
        const bool plain = id == kStatus || id == kDetail || id == kLive || id == kFolder || id == kImageFile ||
                           id == kSystemVolumeText || id == kMicVolumeText;
        return add(id, WC_STATICW, text, SS_LEFT | (plain ? SS_NOPREFIX : 0) | (id == kDetail ? SS_ENDELLIPSIS : 0));
    }
    HWND combo(int id) { return add(id, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP); }
    HWND check(int id, const wchar_t* text) { return add(id, WC_BUTTONW, text, BS_AUTOCHECKBOX | WS_TABSTOP); }
    HWND button(int id, const wchar_t* text, DWORD style = BS_PUSHBUTTON) { return add(id, WC_BUTTONW, text, style | WS_TABSTOP); }
    HWND edit(int id, DWORD style = 0) { return add(id, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP | style, WS_EX_CLIENTEDGE); }

    void create() {
        dpi_ = GetDpiForWindow(window_);
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        // Labels come right before their control: screen readers name the control after them.
        label(kStatus, L"Ready to record");
        label(kDetail, L"");
        label(kLive, L"");
        // Each source: its level before volume, the volume, and mute.
        label(kSystemLabel, L"System audio");
        add(kSystemMeter, PROGRESS_CLASSW, L"", PBS_SMOOTH);
        add(kSystemVolume, TRACKBAR_CLASSW, L"System audio volume", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP);
        label(kSystemVolumeText, L"");
        check(kSystemMute, L"Mute system");
        label(kMicLabel, L"Microphone");
        add(kMicMeter, PROGRESS_CLASSW, L"", PBS_SMOOTH);
        add(kMicVolume, TRACKBAR_CLASSW, L"Microphone volume", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP);
        label(kMicVolumeText, L"");
        check(kMicMute, L"Mute mic");
        button(kRecord, L"&Start Recording", BS_DEFPUSHBUTTON);
        button(kPause, L"&Pause");
        button(kSaveReplay, L"Save Rep&lay");
        add(kPreview, L"RecPreview", L"Preview", 0);
        preview_brush_ = CreateSolidBrush(RGB(28, 28, 32));
        check(kShowPreview, L"Show pre&view");

        label(kCaptureHeader, L"Capture");
        label(kSourceLabel, L"&Source");
        combo(kSource);
        check(kSystemAudio, L"System &audio");
        check(kMicrophone, L"&Microphone");
        HWND mic = combo(kMicDevice);
        check(kCursor, L"Show &cursor");
        check(kClicks, L"Show c&licks");

        label(kQualityHeader, L"Quality");
        label(kResolutionLabel, L"&Resolution");
        HWND resolution = combo(kResolution);
        label(kFpsLabel, L"&Frame rate");
        HWND fps = combo(kFps);
        label(kCodecLabel, L"Co&dec");
        HWND codec = combo(kCodec);
        label(kFormatLabel, L"F&ormat");
        HWND format = combo(kFormat);

        label(kFolderHeader, L"Recordings");
        label(kFolder, L"");
        button(kChangeFolder, L"C&hange Folder...");
        button(kOpenFolder, L"Open Fo&lder");
        button(kShowLast, L"Show Last Recordin&g");

        label(kStreamHeader, L"Live stream");
        check(kStream, L"&Go live while recording");
        label(kServiceLabel, L"Ser&vice");
        HWND service = combo(kService);
        label(kServerLabel, L"Server &URL");
        edit(kServer);
        label(kKeyLabel, L"Stream &key");
        edit(kKey, ES_PASSWORD);
        label(kBitrateLabel, L"&Bitrate (kb/s)");
        edit(kBitrate, ES_NUMBER);
        label(kService2Label, L"Also stream to");
        HWND service2 = combo(kService2);
        label(kServer2Label, L"Second server URL");
        edit(kServer2);
        label(kKey2Label, L"Second stream key");
        edit(kKey2, ES_PASSWORD);

        label(kReplayHeader, L"Replay");
        check(kReplay, L"Keep the las&t");
        HWND replay = combo(kReplayLength);

        label(kScenesHeader, L"Scenes");
        label(kSceneLabel, L"Sce&ne");
        combo(kScene);
        button(kNewScene, L"New Scene");
        button(kDeleteScene, L"Delete Scene");
        label(kSceneNameLabel, L"Name");
        edit(kSceneName);
        label(kSourcesLabel, L"Sources");
        add(kSceneSources, WC_LISTBOXW, L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE);
        check(kSourceShown, L"Show in this scene");
        button(kAddDisplay, L"Add Display...");
        button(kAddWindow, L"Add Window...");
        button(kRemoveSource, L"Remove");

        label(kCameraHeader, L"Camera and image");
        check(kCamera, L"Cam&era");
        HWND camera = combo(kCameraDevice);
        label(kPositionLabel, L"Pos&ition");
        HWND corner = combo(kCorner);
        label(kSizeLabel, L"Si&ze");
        HWND size = combo(kCameraSize);
        check(kImage, L"Image overla&y");
        label(kImageFile, L"");
        button(kBrowseImage, L"Bro&wse...");

        SendMessageW(mic, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Default microphone"));
        for (const auto& device : microphones_)
            SendMessageW(mic, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(device.name).c_str()));
        const auto fill = [](HWND list, const auto& choices) {
            for (const auto& choice : choices)
                SendMessageW(list, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(choice.label).c_str()));
        };
        fill(resolution, kResolutions);
        fill(codec, kCodecs);
        fill(format, kFormats);
        fill(service, kServices);
        fill(service2, kSecondServices);
        fill(corner, kCorners);
        fill(size, kCameraSizes);
        for (const auto& c : cameras_) SendMessageW(camera, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(c.name).c_str()));
        for (auto seconds : rec::app::kReplayLengths)
            SendMessageW(replay, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(duration_label(seconds).c_str()));
        for (auto rate : kFrameRates) SendMessageW(fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((std::to_wstring(rate) + L" fps").c_str()));
        for (int meter : {kSystemMeter, kMicMeter}) SendMessageW(get(meter), PBM_SETRANGE32, 0, 1000);
        for (int slider : {kSystemVolume, kMicVolume}) {
            SendMessageW(get(slider), TBM_SETRANGEMIN, FALSE, rec::app::kVolumeMin);
            SendMessageW(get(slider), TBM_SETRANGEMAX, FALSE, rec::app::kVolumeMax);
            SendMessageW(get(slider), TBM_SETPAGESIZE, 0, 6);
        }
        // Screen readers would name a slider after the row label and read its position as a
        // percentage; annotate the name and the value in dB instead.
        if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&access_)))) {
            access_->SetHwndPropStr(get(kSystemVolume), OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, L"System audio volume");
            access_->SetHwndPropStr(get(kMicVolume), OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, L"Microphone volume");
        }

        show_settings();
        make_fonts();
        layout();
        fit();
        idle_icon_ = dot_icon(GetSystemMetricsForDpi(SM_CXSMICON, dpi_), RGB(120, 120, 128), true);
        recording_icon_ = dot_icon(GetSystemMetricsForDpi(SM_CXSMICON, dpi_), RGB(225, 40, 40), false);
        HICON big = dot_icon(GetSystemMetricsForDpi(SM_CXICON, dpi_), RGB(225, 40, 40), false);
        SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(recording_icon_));
        add_tray();
        hotkey_ = RegisterHotKey(window_, kHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R') != FALSE;
        replay_hotkey_ = RegisterHotKey(window_, kReplayHotkey, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'R') != FALSE;
        refresh();
    }

    // A full-width status area over two columns of settings: what and how to record on the
    // left, where it goes on the right. Two columns keep the window shorter than a 1080p
    // screen at 150% scaling.
    void layout() {
        const int margin = px(20), column = px(420), scenes_column = px(290), label_width = px(130), row = px(26),
                  gap = px(8);
        const int full = 2 * column + scenes_column + px(60);
        int x = margin, y = margin, width = full;
        const auto place = [&](int id, int left, int top, int w, int h) { MoveWindow(get(id), left, top, w, h, FALSE); };
        const auto line = [&](int id, int h) { place(id, x, y, width, h); y += h + gap; };
        const auto pair = [&](int label_id, int control_id, int h) {
            place(label_id, x, y + px(4), label_width - gap, px(20));
            // A drop-down list's height includes its open list.
            const bool list = control_id == kSource || control_id == kMicDevice || control_id == kResolution ||
                              control_id == kFps || control_id == kCodec || control_id == kFormat || control_id == kService || control_id == kService2 ||
                              control_id == kReplayLength;
            place(control_id, x + label_width, y, width - label_width, list ? px(240) : h);
            y += h + gap;
        };
        const auto header = [&](int id) { y += px(10); line(id, px(20)); };
        // A source's name and level meter, then its volume slider, value and mute.
        const auto level = [&](int label_id, int meter_id, int slider_id, int value_id, int mute_id) {
            const int value = px(50), mute = px(110);
            place(label_id, x, y, label_width - gap, px(20));
            place(meter_id, x + label_width, y + px(4), width - label_width, px(12));
            y += px(22);
            const int slider = width - value - mute - 2 * gap;
            place(slider_id, x, y, slider, px(26));
            place(value_id, x + slider + gap, y + px(4), value, px(20));
            place(mute_id, x + slider + value + 2 * gap, y + px(2), mute, px(22));
            y += px(26) + gap;
        };
        // A checkbox with its device list or setting beside it.
        const auto beside = [&](int check_id, int control_id) {
            place(check_id, x, y, label_width - gap, px(22));
            place(control_id, x + label_width, y, width - label_width, px(240));
            y += row + gap;
        };

        // Status, levels and the record buttons on the left, the preview on the right.
        width = column;
        line(kStatus, px(34));
        line(kDetail, px(20));
        line(kLive, px(20));
        level(kSystemLabel, kSystemMeter, kSystemVolume, kSystemVolumeText, kSystemMute);
        level(kMicLabel, kMicMeter, kMicVolume, kMicVolumeText, kMicMute);
        const int button_width = px(110);
        place(kRecord, x, y, width - 2 * (button_width + gap), px(40));
        place(kPause, x + width - 2 * button_width - gap, y, button_width, px(40));
        place(kSaveReplay, x + width - button_width, y, button_width, px(40));
        y += px(40);
        const int preview_height = column * 9 / 16;
        place(kPreview, margin + column + px(30), margin, column, preview_height);
        place(kShowPreview, margin + column + px(30), margin + preview_height + gap, column, px(22));
        y = std::max(y, margin + preview_height + gap + px(22)) + gap;
        const int columns_top = y;

        width = column;
        header(kCaptureHeader);
        pair(kSourceLabel, kSource, row);
        line(kSystemAudio, px(22));
        beside(kMicrophone, kMicDevice);
        place(kCursor, x, y, label_width - gap, px(22));
        place(kClicks, x + label_width, y, width - label_width, px(22));
        y += px(22) + gap;

        header(kQualityHeader);
        pair(kResolutionLabel, kResolution, row);
        pair(kFpsLabel, kFps, row);
        pair(kCodecLabel, kCodec, row);
        pair(kFormatLabel, kFormat, row);

        header(kFolderHeader);
        line(kFolder, px(20));
        const int third = (width - 2 * gap) / 3;
        place(kChangeFolder, x, y, third, px(30));
        place(kOpenFolder, x + third + gap, y, third, px(30));
        place(kShowLast, x + 2 * (third + gap), y, third, px(30));
        y += px(30);
        const int left_bottom = y;

        x = margin + column + px(30);
        y = columns_top;
        header(kStreamHeader);
        line(kStream, px(22));
        pair(kServiceLabel, kService, row);
        pair(kServerLabel, kServer, row);
        pair(kKeyLabel, kKey, row);
        pair(kBitrateLabel, kBitrate, row);
        pair(kService2Label, kService2, row);
        pair(kServer2Label, kServer2, row);
        pair(kKey2Label, kKey2, row);

        header(kReplayHeader);
        beside(kReplay, kReplayLength);
        const int right_bottom = y;

        // The scenes, their sources, the camera and the image in a narrow third column.
        x = margin + 2 * (column + px(30));
        y = margin - px(10);
        width = scenes_column;
        const int short_label = px(64);
        const auto row_of = [&](int label_id, int control_id, bool list) {
            place(label_id, x, y + px(4), short_label - gap, px(20));
            place(control_id, x + short_label, y, width - short_label, list ? px(240) : row);
            y += row + gap;
        };
        header(kScenesHeader);
        row_of(kSceneLabel, kScene, true);
        const int half_width = (width - gap) / 2;
        place(kNewScene, x, y, half_width, row);
        place(kDeleteScene, x + half_width + gap, y, width - half_width - gap, row);
        y += row + gap;
        row_of(kSceneNameLabel, kSceneName, false);
        line(kSourcesLabel, px(20));
        line(kSceneSources, px(124));
        line(kSourceShown, px(22));
        const int button_third = (width - 2 * gap) / 3;
        place(kAddDisplay, x, y, button_third, row);
        place(kAddWindow, x + button_third + gap, y, button_third, row);
        place(kRemoveSource, x + 2 * (button_third + gap), y, width - 2 * (button_third + gap), row);
        y += row + gap;

        header(kCameraHeader);
        const int box = px(104);
        place(kCamera, x, y, box - gap, px(22));
        place(kCameraDevice, x + box, y, width - box, px(240));
        y += row + gap;
        row_of(kPositionLabel, kCorner, true);
        row_of(kSizeLabel, kCameraSize, true);
        place(kImage, x, y, box - gap, px(22));
        place(kImageFile, x + box, y + px(4), width - box, px(20));
        y += row;
        place(kBrowseImage, x + box, y, width - box, row);
        y += row;

        content_width_ = full + 2 * margin;
        content_height_ = std::max({left_bottom, right_bottom, y}) + margin;
        InvalidateRect(window_, nullptr, TRUE);
    }
    void fit() {
        RECT frame{0, 0, content_width_, content_height_};        AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongW(window_, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongW(window_, GWL_EXSTYLE)), dpi_);
        SetWindowPos(window_, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    COLORREF color_for(int id) const {
        if (id == kLive) return RGB(150, 40, 200);
        if (id == kStatus) {
            if (state_ == State::recording) return RGB(210, 30, 30);
            if (state_ == State::paused) return RGB(190, 120, 0);
        }
        if (id == kDetail && attention_) return RGB(190, 60, 0);
        if (id == kDetail || id == kFolder || id == kImageFile) return GetSysColor(COLOR_GRAYTEXT);
        return GetSysColor(COLOR_WINDOWTEXT);
    }

    void select(int id, int index) { SendMessageW(get(id), CB_SETCURSEL, static_cast<WPARAM>(index), 0); }
    int selected(int id) const { return static_cast<int>(SendMessageW(get(id), CB_GETCURSEL, 0, 0)); }
    void set_check(int id, bool on) { SendMessageW(get(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
    bool checked(int id) const { return SendMessageW(get(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }

    void show_settings() {
        const auto& s = settings_;
        fill_sources();
        set_check(kSystemAudio, s.system_audio);
        set_check(kMicrophone, s.microphone);
        int mic = 0;
        for (std::size_t i = 0; i < microphones_.size(); ++i)
            if (microphones_[i].id == s.microphone_device) mic = static_cast<int>(i) + 1;
        select(kMicDevice, mic);
        show_volume(rec::AudioInput::system);
        show_volume(rec::AudioInput::microphone);
        set_check(kCursor, s.cursor);
        set_check(kClicks, s.clicks);
        select(kResolution, choice_index(kResolutions, s.resolution));
        select(kFps, static_cast<int>(std::find(kFrameRates.begin(), kFrameRates.end(), s.fps) - kFrameRates.begin()) % static_cast<int>(kFrameRates.size()));
        select(kCodec, choice_index(kCodecs, s.codec));
        select(kFormat, choice_index(kFormats, s.format));
        set_check(kStream, s.stream);
        select(kService, choice_index(kServices, s.destination));
        select(kService2, choice_index(kSecondServices, s.destination2));
        SetWindowTextW(get(kBitrate), std::to_wstring(s.stream_kbps).c_str());
        set_check(kReplay, s.replay);
        select(kReplayLength, static_cast<int>(std::find(rec::app::kReplayLengths.begin(), rec::app::kReplayLengths.end(), s.replay_seconds) -
                                               rec::app::kReplayLengths.begin()) % static_cast<int>(rec::app::kReplayLengths.size()));
        set_check(kCamera, s.camera);
        int camera = 0;
        for (std::size_t i = 0; i < cameras_.size(); ++i)
            if (cameras_[i].id == s.camera_device) camera = static_cast<int>(i);
        select(kCameraDevice, camera);
        set_check(kImage, s.image);
        set_check(kShowPreview, s.preview);
        show_image();
        show_server();
        show_second_server();
        SetWindowTextW(get(kFolder), wide(s.folder).c_str());
        show_scenes();
    }

    void show_volume(rec::AudioInput input) {
        const bool system = input == rec::AudioInput::system;
        const int volume = system ? settings_.system_volume : settings_.mic_volume;
        SendMessageW(get(system ? kSystemVolume : kMicVolume), TBM_SETPOS, TRUE, volume);
        show_decibels(system, volume);
        set_check(system ? kSystemMute : kMicMute, system ? settings_.system_muted : settings_.mic_muted);
    }

    void show_decibels(bool system, int volume) {
        const auto text = decibels(volume);
        SetWindowTextW(get(system ? kSystemVolumeText : kMicVolumeText), text.c_str());
        HWND slider = get(system ? kSystemVolume : kMicVolume);
        if (access_ && SUCCEEDED(access_->SetHwndPropStr(slider, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_VALUE, text.c_str())))
            NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, slider, OBJID_CLIENT, CHILDID_SELF);
    }

    // Reads a source's slider and mute box, and applies them at once while recording.
    void change_volume(rec::AudioInput input, bool save) {
        const bool system = input == rec::AudioInput::system;
        int& volume = system ? settings_.system_volume : settings_.mic_volume;
        bool& muted = system ? settings_.system_muted : settings_.mic_muted;
        volume = static_cast<int>(SendMessageW(get(system ? kSystemVolume : kMicVolume), TBM_GETPOS, 0, 0));
        muted = checked(system ? kSystemMute : kMicMute);
        show_decibels(system, volume);
        const auto s = session_.status();
        if ((s.state == State::recording || s.state == State::paused) && (system ? s.system_audio : s.microphone)) {
            if (auto changed = session_.set_volume(input, volume, muted); !changed) {
                note_ = L"Volume not changed: " + wide(changed.error);
                note_attention_ = true;
                note_until_ = GetTickCount64() + 5000;
            }
        }
        if (save) rec::app::save_settings(settings_path_, settings_);
        refresh();
    }

    void show_image() {
        const auto& path = settings_.image_path;
        SetWindowTextW(get(kImageFile), path.empty() ? L"No image chosen" : wide(path.substr(path.find_last_of("/\\") + 1)).c_str());
    }

    void fill_sources() {
        const auto& s = settings_;
        sources_.clear();
        std::vector<std::wstring> labels;
        const auto add_source = [&](Source source, std::wstring text) {
            sources_.push_back(std::move(source));
            labels.push_back(std::move(text));
        };
        for (std::size_t i = 0; i < std::max<std::size_t>(1, displays_.size()); ++i) {
            wchar_t name[96];
            if (i < displays_.size()) std::swprintf(name, 96, L"Display %zu (%ux%u)", i + 1, displays_[i].width, displays_[i].height);
            else std::swprintf(name, 96, L"Display 1");
            add_source({Source::Kind::display, static_cast<std::uint32_t>(i + 1)}, name);
        }
        if (const auto r = rec::app::parse_region(s.region)) {
            wchar_t name[128];
            std::swprintf(name, 128, L"Region %ux%u at %d,%d on Display %u", r->width, r->height, r->x, r->y, s.display);
            add_source({Source::Kind::region, s.display}, name);
        }
        add_source({Source::Kind::pick}, L"Select a region...");
        std::string error;
        const auto windows = rec::list_windows(error);
        const auto named = [](const std::string& app, const std::string& title) { return L"Window: " + window_label(app, title); };
        const bool open = std::any_of(windows.begin(), windows.end(), [&](const rec::Window& w) {
            return w.app == s.window_app && w.title == s.window_title;
        });
        if (s.source == "window" && !open && !(s.window_app.empty() && s.window_title.empty()))
            add_source({Source::Kind::window, 0, s.window_app, s.window_title}, named(s.window_app, s.window_title) + L" (not open)");
        for (const auto& w : windows) add_source({Source::Kind::window, 0, w.app, w.title}, named(w.app, w.title));

        HWND list = get(kSource);
        SendMessageW(list, CB_RESETCONTENT, 0, 0);
        for (const auto& text : labels) SendMessageW(list, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        int chosen = 0;
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            const auto& source = sources_[i];
            const bool match = s.source == "window"
                                   ? source.kind == Source::Kind::window && source.app == s.window_app && source.title == s.window_title
                               : s.source == "region" ? source.kind == Source::Kind::region
                                                      : source.kind == Source::Kind::display && source.display == s.display;
            if (match) { chosen = static_cast<int>(i); break; }
        }
        select(kSource, chosen);
    }

    // Hides Rec, lets the person drag a rectangle, and records that part of the display under it.
    void choose_region() {
        ShowWindow(window_, SW_HIDE);
        const auto picked = pick_region(instance_, dpi_);
        show_window();
        if (picked) {
            const POINT center{(picked->left + picked->right) / 2, (picked->top + picked->bottom) / 2};
            MONITORINFOEXW info{};
            info.cbSize = sizeof info;
            GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &info);
            RECT clipped{};
            IntersectRect(&clipped, &*picked, &info.rcMonitor);
            std::uint32_t display = 1;
            const auto device = narrow(info.szDevice);
            for (std::size_t i = 0; i < displays_.size(); ++i) {
                const auto& name = displays_[i].name;
                if (name.size() >= device.size() && name.compare(name.size() - device.size(), device.size(), device) == 0)
                    display = static_cast<std::uint32_t>(i + 1);
            }
            const rec::Region region{clipped.left - info.rcMonitor.left, clipped.top - info.rcMonitor.top,
                                     static_cast<std::uint32_t>(clipped.right - clipped.left) & ~1u,
                                     static_cast<std::uint32_t>(clipped.bottom - clipped.top) & ~1u};
            if (region.width >= 16 && region.height >= 16) {
                settings_.source = "region";
                settings_.display = display;
                settings_.region = rec::app::region_text(region);
            }
        }
        fill_sources();
        remember();
    }

    // Presets show their fixed server; a custom server is typed in.
    void show_server() {
        const bool custom = settings_.destination == "custom";
        SetWindowTextW(get(kServer), wide(custom ? settings_.server : rec::app::server_url(settings_)).c_str());
        SendMessageW(get(kServer), EM_SETREADONLY, !custom, 0);
        SetWindowTextW(get(kKey), wide(load_key(rec::app::server_url(settings_))).c_str());
    }

    void show_second_server() {
        const bool custom2 = settings_.destination2 == "custom";
        SetWindowTextW(get(kServer2), wide(custom2 ? settings_.server2 : rec::app::second_server_url(settings_)).c_str());
        SendMessageW(get(kServer2), EM_SETREADONLY, !custom2, 0);
        SetWindowTextW(get(kKey2), wide(load_key(rec::app::second_server_url(settings_))).c_str());
    }

    void read_settings() {
        auto& s = settings_;
        const int source = selected(kSource);
        if (source >= 0 && source < static_cast<int>(sources_.size())) {
            const auto& chosen = sources_[static_cast<std::size_t>(source)];
            if (chosen.kind == Source::Kind::display || chosen.kind == Source::Kind::region) {
                s.source = chosen.kind == Source::Kind::display ? "display" : "region";
                s.display = chosen.display;
            } else if (chosen.kind == Source::Kind::window) {
                s.source = "window";
                s.window_app = chosen.app;
                s.window_title = chosen.title;
            }
        }
        s.system_audio = checked(kSystemAudio);
        s.microphone = checked(kMicrophone);
        const int mic = selected(kMicDevice);
        s.microphone_device = mic > 0 && mic <= static_cast<int>(microphones_.size()) ? microphones_[mic - 1].id : "default";
        s.cursor = checked(kCursor);
        s.clicks = checked(kClicks);
        const auto at = [this](int id) { return static_cast<std::size_t>(std::max(0, selected(id))); };
        s.resolution = kResolutions[at(kResolution)].value;
        s.fps = kFrameRates[at(kFps)];
        s.codec = kCodecs[at(kCodec)].value;
        s.format = kFormats[at(kFormat)].value;
        s.stream = checked(kStream);
        s.replay = checked(kReplay);
        s.replay_seconds = rec::app::kReplayLengths[at(kReplayLength)];
        s.camera = checked(kCamera);
        s.camera_device = cameras_[std::min(at(kCameraDevice), cameras_.size() - 1)].id;
        s.image = checked(kImage);
        s.preview = checked(kShowPreview);
        s.destination = kServices[at(kService)].value;
        if (s.destination == "custom") s.server = narrow(text_of(get(kServer)));
        s.destination2 = kSecondServices[at(kService2)].value;
        if (s.destination2 == "custom") s.server2 = narrow(text_of(get(kServer2)));
        const auto kbps = std::wcstoul(text_of(get(kBitrate)).c_str(), nullptr, 10);
        s.stream_kbps = static_cast<std::uint32_t>(std::clamp<unsigned long>(kbps ? kbps : 6000, 500, 50000));
    }

    void remember() {
        read_settings();
        rec::app::save_settings(settings_path_, settings_);
    }

    void command(int id, int code) {
        handle_command(id, code);
        // A changed setting can change what the idle preview shows.
        if (state_ == State::idle && !quitting_ && (code == BN_CLICKED || code == CBN_SELCHANGE)) refresh();
    }

    void handle_command(int id, int code) {
        switch (id) {
        case kRecord: case kMenuRecord: toggle_record(); return;
        case kPause: case kMenuPause: toggle_pause(); return;
        case kSaveReplay: case kMenuReplay: save_replay(); return;
        case kMenuShow: show_window(); return;
        case kOpenFolder: case kMenuFolder: open_folder(); return;
        case kMenuQuit: quit(); return;
        case kChangeFolder: choose_folder(); return;
        case kBrowseImage: choose_image(); return;
        case kScene:
            if (code == CBN_SELCHANGE) change_scene(static_cast<std::size_t>(std::max(0, selected(kScene))));
            return;
        case kNewScene:
            if (rec::app::add_scene(settings_)) {
                show_scenes();
                apply_scene();
            }
            return;
        case kDeleteScene:
            rec::app::remove_scene(settings_);
            show_scenes();
            apply_scene();
            return;
        case kSceneName:
            if (code == EN_CHANGE && !filling_) {
                auto& scene = rec::app::current_scene(settings_);
                scene.name = narrow(text_of(get(kSceneName)));
                HWND list = get(kScene);
                SendMessageW(list, CB_DELETESTRING, settings_.scene, 0);
                SendMessageW(list, CB_INSERTSTRING, settings_.scene, reinterpret_cast<LPARAM>(wide(scene.name).c_str()));
                select(kScene, static_cast<int>(settings_.scene));
            } else if (code == EN_KILLFOCUS) {
                rec::app::save_settings(settings_path_, settings_);
            }
            return;
        case kSceneSources:
            if (code == LBN_SELCHANGE) {
                const auto sources = rec::app::scene_sources(settings_);
                const auto at = static_cast<std::size_t>(SendMessageW(get(kSceneSources), LB_GETCURSEL, 0, 0));
                if (at < sources.size()) select_source(sources[at]);
            }
            return;
        case kSourceShown:
            if (code == BN_CLICKED) {
                rec::app::scene_item(rec::app::current_scene(settings_), selected_).visible = checked(kSourceShown);
                show_scene_sources();
                apply_scene();
            }
            return;
        case kAddDisplay: add_source_menu(false); return;
        case kAddWindow: add_source_menu(true); return;
        case kRemoveSource: remove_source(); return;
        case kCorner: case kCameraSize:
            if (code == CBN_SELCHANGE) place_camera();
            return;
        case kShowLast: show_last(); return;
        case kService: case kService2:
            if (code == CBN_SELCHANGE) {
                read_settings();
                if (id == kService) show_server();
                else show_second_server();
                refresh();
            }
            return;
        case kSource:
            if (code == CBN_DROPDOWN) {
                // Windows open and close while Rec waits; list the current ones.
                read_settings();
                fill_sources();
            } else if (code == CBN_SELCHANGE) {
                const int at = selected(kSource);
                if (at >= 0 && at < static_cast<int>(sources_.size()) && sources_[static_cast<std::size_t>(at)].kind == Source::Kind::pick)
                    PostMessageW(window_, WM_COMMAND, MAKEWPARAM(kSource, kPickRegion), 0);
                else
                    read_settings();
            } else if (code == kPickRegion) {
                choose_region();
            }
            return;
        case kServer:
            if (code == EN_KILLFOCUS && settings_.destination == "custom") {
                read_settings();
                SetWindowTextW(get(kKey), wide(load_key(rec::app::server_url(settings_))).c_str());
            }
            return;
        case kServer2:
            if (code == EN_KILLFOCUS && settings_.destination2 == "custom") {
                read_settings();
                SetWindowTextW(get(kKey2), wide(load_key(rec::app::second_server_url(settings_))).c_str());
            }
            return;
        case kCamera: case kImage:
            if (code == BN_CLICKED) {
                read_settings();
                show_scene_sources();
            }
            return;
        case kStream: case kSystemAudio: case kMicrophone:
            return;
        case kShowPreview:
            if (code == BN_CLICKED) remember();
            return;
        case kSystemMute: case kMicMute:
            if (code == BN_CLICKED) change_volume(id == kSystemMute ? rec::AudioInput::system : rec::AudioInput::microphone, true);
            return;
        case kMenuMuteSystem: case kMenuMuteMic:
            set_check(id == kMenuMuteSystem ? kSystemMute : kMicMute, !checked(id == kMenuMuteSystem ? kSystemMute : kMicMute));
            change_volume(id == kMenuMuteSystem ? rec::AudioInput::system : rec::AudioInput::microphone, true);
            return;
        default:
            if (id >= kMenuScene && id < kMenuScene + static_cast<int>(kMaxMenuScenes)) {
                change_scene(static_cast<std::size_t>(id - kMenuScene));
            } else if (id >= kMenuAddDisplay && id < kMenuAddDisplay + static_cast<int>(kMaxMenuDisplays)) {
                rec::app::Layer layer;
                layer.display = static_cast<std::uint32_t>(id - kMenuAddDisplay + 1);
                add_layer(layer);
            } else if (id >= kMenuAddWindow && id < kMenuAddWindow + static_cast<int>(kMaxMenuWindows)) {
                const auto at = static_cast<std::size_t>(id - kMenuAddWindow);
                if (at >= menu_windows_.size()) return;
                rec::app::Layer layer;
                layer.kind = "window";
                layer.window_app = menu_windows_[at].app;
                layer.window_title = menu_windows_[at].title;
                add_layer(layer);
            }
            return;
        }
    }

    void toggle_record() {
        const auto state = session_.status().state;
        if (state == State::recording || state == State::paused) {
            session_.stop();
        } else if (state == State::idle) {
            remember();
            const auto key = narrow(text_of(get(kKey)));
            const auto key2 = narrow(text_of(get(kKey2)));
            if (auto started = session_.start(settings_, key, key2, test_pattern_); !started) {
                MessageBoxW(window_, wide(started.error).c_str(), L"Rec could not start recording", MB_ICONWARNING);
                return;
            }
            if (settings_.stream) {
                store_key(rec::app::server_url(settings_), key);
                store_key(rec::app::second_server_url(settings_), key2);
            }
        }
        refresh();
    }

    void toggle_pause() {
        const auto state = session_.status().state;
        if (state == State::recording) session_.pause();
        else if (state == State::paused) session_.resume();
        refresh();
    }

    bool busy() { return session_.status().state != State::idle; }

    void quit() {
        quitting_ = true;
        if (busy()) {
            session_.stop();
            ShowWindow(window_, SW_HIDE);
            refresh();
            return;  // kSessionChanged arrives once the movie is saved.
        }
        remember();
        DestroyWindow(window_);
    }

    void save_replay() {
        std::string path;
        double seconds = 0;
        if (auto saved = session_.save_replay(settings_.folder, path, seconds); !saved) {
            note_ = L"Replay not saved: " + wide(saved.error);
            note_attention_ = true;
        } else {
            wchar_t length[32];
            std::swprintf(length, 32, L"%.1f s", seconds);
            note_ = L"Saved replay (" + std::wstring(length) + L"): " + wide(path.substr(path.find_last_of("/\\") + 1));
            note_attention_ = false;
            settings_.last_recording = path;
            rec::app::save_settings(settings_path_, settings_);
        }
        note_until_ = GetTickCount64() + 5000;
        refresh();
    }

    // While recording the scene shows at once; otherwise it is the scene to start with, shown
    // in the preview.
    void change_scene(std::size_t index) {
        if (index >= settings_.scenes.size()) return;
        settings_.scene = static_cast<std::uint32_t>(index);
        show_scenes();
        apply_scene();
    }

    // Shows the current scene in the recording or the preview, and saves it.
    void apply_scene() {
        if (auto shown = session_.show_scene(settings_); !shown) {
            note_ = L"Scene not changed: " + wide(shown.error);
            note_attention_ = true;
            note_until_ = GetTickCount64() + 5000;
        }
        rec::app::save_settings(settings_path_, settings_);
        refresh();
    }

    // The Scene list, the scene's name, its sources and the camera's place.
    void show_scenes() {
        HWND list = get(kScene);
        SendMessageW(list, CB_RESETCONTENT, 0, 0);
        for (const auto& scene : settings_.scenes)
            SendMessageW(list, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(scene.name).c_str()));
        const auto& scene = rec::app::current_scene(settings_);
        select(kScene, static_cast<int>(settings_.scene));
        filling_ = true;
        if (narrow(text_of(get(kSceneName))) != scene.name) SetWindowTextW(get(kSceneName), wide(scene.name).c_str());
        filling_ = false;
        show_scene_sources();
        show_camera_place();
    }

    // Every source, as the scene shows it; the selected one stays selected.
    void show_scene_sources() {
        const auto sources = rec::app::scene_sources(settings_);
        const auto& scene = rec::app::current_scene(settings_);
        HWND list = get(kSceneSources);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        int chosen = 0;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            std::wstring name = wide(rec::app::source_name(settings_, sources[i]));
            if (!rec::app::scene_item(scene, sources[i]).visible) name += L" (hidden)";
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            if (sources[i] == selected_) chosen = static_cast<int>(i);
        }
        selected_ = sources[static_cast<std::size_t>(chosen)];
        SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(chosen), 0);
        set_check(kSourceShown, rec::app::scene_item(scene, selected_).visible);
    }

    void select_source(const rec::app::SourceRef& source) {
        selected_ = source;
        show_scene_sources();
        refresh();
    }

    // The Position and Size lists name the camera's place in this scene when it is a preset.
    void show_camera_place() {
        const auto& area = rec::app::current_scene(settings_).camera.area;
        const auto corner = rec::app::corner_of(area);
        if (selected(kCorner) != choice_index(kCorners, corner)) select(kCorner, choice_index(kCorners, corner));
        if (const auto size = rec::app::size_of(area); !size.empty()) select(kCameraSize, choice_index(kCameraSizes, size));
        EnableWindow(get(kCameraSize), IsWindowEnabled(get(kCorner)) && corner != "custom");
    }

    // A preset Position or Size puts the camera there in this scene.
    void place_camera() {
        const std::string corner = kCorners[static_cast<std::size_t>(std::max(0, selected(kCorner)))].value;
        if (corner == "custom") return;
        const std::string size = kCameraSizes[static_cast<std::size_t>(std::max(0, selected(kCameraSize)))].value;
        rec::app::current_scene(settings_).camera.area = rec::app::corner_area(corner, size);
        show_camera_place();
        apply_scene();
    }

    void add_source_menu(bool windows) {
        HMENU menu = CreatePopupMenu();
        if (windows) {
            std::string error;
            menu_windows_ = rec::list_windows(error);
            if (menu_windows_.size() > kMaxMenuWindows) menu_windows_.resize(kMaxMenuWindows);
            for (std::size_t i = 0; i < menu_windows_.size(); ++i)
                AppendMenuW(menu, MF_STRING, kMenuAddWindow + i, window_label(menu_windows_[i].app, menu_windows_[i].title).c_str());
            if (menu_windows_.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No windows are open");
        } else {
            for (std::size_t i = 0; i < std::max<std::size_t>(1, displays_.size()); ++i) {
                wchar_t name[96];
                if (i < displays_.size()) std::swprintf(name, 96, L"Display %zu (%ux%u)", i + 1, displays_[i].width, displays_[i].height);
                else std::swprintf(name, 96, L"Display 1");
                AppendMenuW(menu, MF_STRING, kMenuAddDisplay + i, name);
            }
        }
        RECT button{};
        GetWindowRect(get(windows ? kAddWindow : kAddDisplay), &button);
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, button.left, button.bottom, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    // A new display or window, shown over the whole frame in this scene and hidden in the others.
    void add_layer(const rec::app::Layer& layer) {
        if (state_ != State::idle) return;
        if (!rec::app::add_layer(settings_, layer)) {
            note_ = L"Scenes hold at most " + std::to_wstring(rec::app::kMaxLayers) + L" displays and windows";
            note_attention_ = true;
            note_until_ = GetTickCount64() + 5000;
            refresh();
            return;
        }
        selected_ = {rec::app::SourceRef::Kind::layer, settings_.layers.size() - 1};
        show_scene_sources();
        rec::app::save_settings(settings_path_, settings_);
        refresh();
    }

    void remove_source() {
        if (state_ != State::idle || selected_.kind != rec::app::SourceRef::Kind::layer) return;
        rec::app::remove_layer(settings_, selected_.layer);
        selected_ = {};
        show_scene_sources();
        rec::app::save_settings(settings_path_, settings_);
        refresh();
    }
    void choose_image() {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(L"Choose an image to show over the recording");
        const COMDLG_FILTERSPEC types[]{{L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff"}, {L"All files", L"*.*"}};
        dialog->SetFileTypes(2, types);
        if (SUCCEEDED(dialog->Show(window_))) {
            IShellItem* item = nullptr;
            PWSTR path = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                settings_.image_path = narrow(path);
                set_check(kImage, true);
                show_image();
                remember();
            }
            CoTaskMemFree(path);
            if (item) item->Release();
        }
        dialog->Release();
        refresh();
    }

    void show_window() {
        ShowWindow(window_, SW_SHOW);
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
    }

    void open_folder() {
        CreateDirectoryW(wide(settings_.folder).c_str(), nullptr);
        ShellExecuteW(window_, L"open", wide(settings_.folder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void show_last() {
        if (settings_.last_recording.empty()) return;
        std::wstring path = wide(settings_.last_recording);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.c_str())) {
            SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            ILFree(item);
        }
    }

    void choose_folder() {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(L"Choose where Rec saves recordings");
        IShellItem* current = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(wide(settings_.folder).c_str(), nullptr, IID_PPV_ARGS(&current)))) {
            dialog->SetFolder(current);
            current->Release();
        }
        if (SUCCEEDED(dialog->Show(window_))) {
            IShellItem* item = nullptr;
            PWSTR path = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                settings_.folder = narrow(path);
                SetWindowTextW(get(kFolder), path);
                remember();
            }
            CoTaskMemFree(path);
            if (item) item->Release();
        }
        dialog->Release();
    }

    void add_tray() {
        tray_ = {sizeof tray_};
        tray_.hWnd = window_;
        tray_.uID = kTrayId;
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        tray_.uCallbackMessage = kTrayMessage;
        tray_.hIcon = idle_icon_;
        wcscpy_s(tray_.szTip, L"Rec");
        Shell_NotifyIconW(NIM_ADD, &tray_);
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    }

    void update_tray(HICON icon, const std::wstring& tip) {
        if (icon == tray_.hIcon && tip == tray_.szTip) return;
        tray_.hIcon = icon;
        wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE);
        tray_.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        Shell_NotifyIconW(NIM_MODIFY, &tray_);
    }

    void tray_event(UINT event) {
        if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP) {
            show_window();
            return;
        }
        if (event != WM_CONTEXTMENU && event != WM_RBUTTONUP) return;
        const auto state = session_.status().state;
        const bool active = state == State::recording || state == State::paused;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (state == State::starting || state == State::finishing ? MF_GRAYED : 0), kMenuRecord,
                    active ? L"Stop Recording\tCtrl+Alt+R" : L"Start Recording\tCtrl+Alt+R");
        AppendMenuW(menu, MF_STRING | (active ? 0 : MF_GRAYED), kMenuPause, state == State::paused ? L"Resume" : L"Pause");
        AppendMenuW(menu, MF_STRING | (active && session_.status().replay ? 0 : MF_GRAYED), kMenuReplay,
                    L"Save Replay\tCtrl+Alt+Shift+R");
        if (const auto status = session_.status(); active && (status.system_audio || status.microphone)) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            if (status.system_audio)
                AppendMenuW(menu, MF_STRING | (checked(kSystemMute) ? MF_CHECKED : 0), kMenuMuteSystem, L"Mute System Audio");
            if (status.microphone)
                AppendMenuW(menu, MF_STRING | (checked(kMicMute) ? MF_CHECKED : 0), kMenuMuteMic, L"Mute Microphone");
        }
        if (settings_.scenes.size() > 1) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            for (std::size_t i = 0; i < std::min(settings_.scenes.size(), kMaxMenuScenes); ++i)
                AppendMenuW(menu, MF_STRING | (settings_.scene == i ? MF_CHECKED : 0), kMenuScene + i,
                            wide(settings_.scenes[i].name).c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuShow, L"Show Rec");
        AppendMenuW(menu, MF_STRING, kMenuFolder, L"Open Recordings Folder");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit Rec");
        POINT at{};
        GetCursorPos(&at);
        SetForegroundWindow(window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, at.x, at.y, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    void meter(int id, float peak, float& shown) {
        // dB scale over the last 60 dB, falling back at about 20 dB a second.
        const float level = peak > 0 ? std::clamp((20.0f * std::log10(peak) + 60.0f) / 60.0f, 0.0f, 1.0f) : 0.0f;
        shown = std::max(level, shown - 0.017f);
        const auto position = static_cast<int>(shown * 1000);
        // Setting one past and back skips the progress bar's smoothing animation.
        SendMessageW(get(id), PBM_SETPOS, static_cast<WPARAM>(std::min(1000, position + 1)), 0);
        SendMessageW(get(id), PBM_SETPOS, static_cast<WPARAM>(position), 0);
    }

    void set_text(int id, const std::wstring& text) {
        if (text_of(get(id)) != text) SetWindowTextW(get(id), text.c_str());
    }

    void refresh() {
        const Status s = session_.status();
        const bool active = s.state == State::recording || s.state == State::paused;
        const bool changed = s.state != state_;
        state_ = s.state;
        if (!s.saved.empty() && s.saved != settings_.last_recording) {
            settings_.last_recording = s.saved;
            rec::app::save_settings(settings_path_, settings_);
            note_.clear();
        }

        std::wstring status, detail;
        attention_ = false;
        switch (s.state) {
        case State::idle:
            status = L"Ready to record";
            if (!s.error.empty()) { detail = L"Recording failed: " + wide(s.error); attention_ = true; }
            else if (!s.stream_error.empty()) { detail = L"Stream stopped early: " + wide(s.stream_error); attention_ = true; }
            else if (!s.saved.empty()) {
                detail = (s.window_closed ? L"The window closed. Saved " : L"Saved ") +
                         wide(s.saved.substr(s.saved.find_last_of("/\\") + 1));
            }
            else detail = hotkey_ ? L"Ctrl+Alt+R starts and stops recording from anywhere." : L"Ctrl+Alt+R is used by another app.";
            break;
        case State::starting: status = L"Starting..."; break;
        case State::recording: status = L"\u25CF Recording  " + clock(s.seconds); break;
        case State::paused: status = L"Paused  " + clock(s.seconds); break;
        case State::finishing: status = L"Saving..."; break;
        }
        if (active) {
            detail = std::to_wstring(s.frames) + L" frames";
            if (s.dropped) { detail += L", " + std::to_wstring(s.dropped) + L" dropped"; attention_ = true; }
            if (s.audio_dropped) { detail += L", audio drops " + std::to_wstring(s.audio_dropped); attention_ = true; }
            if (s.streaming && !s.stream_error.empty()) { detail += L". Stream lost: " + wide(s.stream_error); attention_ = true; }
            else if (s.stream_dropped) { detail += L", stream skipped " + std::to_wstring(s.stream_dropped); attention_ = true; }
            if (s.live && s.stream_queued_ms > 1000) {
                wchar_t behind[48];
                std::swprintf(behind, 48, L", stream %.1f s behind", s.stream_queued_ms / 1000.0);
                detail += behind;
                attention_ = true;
            }
            if (s.reconnects) detail += L", stream reconnected " + std::to_wstring(s.reconnects) + (s.reconnects == 1 ? L" time" : L" times");
            if (s.replay) detail += L", replay " + std::to_wstring(static_cast<int>(s.replay_seconds)) + L" s";
        }
        if (!note_.empty() && GetTickCount64() < note_until_) {
            detail = note_;
            attention_ = note_attention_;
        }
        set_text(kStatus, status);
        set_text(kDetail, detail);
        std::wstring live;
        if (active && s.live) {
            wchar_t rate[48];
            if (s.servers > 1)
                std::swprintf(rate, 48, L"\u25CF LIVE %u/%u  %.1f Mb/s", s.servers_live, s.servers, s.stream_kbps / 1000.0);
            else
                std::swprintf(rate, 48, L"\u25CF LIVE  %.1f Mb/s", s.stream_kbps / 1000.0);
            live = rate;
        } else if (active && s.reconnecting) {
            live = L"Reconnecting...";
        }
        set_text(kLive, live);
        // Hide the last level once recording ends; polling stops with it.
        if (!active) system_shown_ = mic_shown_ = 0;
        meter(kSystemMeter, s.system_peak, system_shown_);
        meter(kMicMeter, s.mic_peak, mic_shown_);
        if (changed) {
            InvalidateRect(get(kStatus), nullptr, TRUE);
        }
        InvalidateRect(get(kDetail), nullptr, TRUE);

        set_text(kRecord, active ? L"&Stop Recording" : L"&Start Recording");
        set_text(kPause, s.state == State::paused ? L"&Resume" : L"&Pause");
        EnableWindow(get(kRecord), s.state == State::idle || active);
        EnableWindow(get(kPause), active);
        EnableWindow(get(kSaveReplay), active && s.replay);
        const bool editable = s.state == State::idle;
        for (int id : {kSource, kSystemAudio, kMicrophone, kCursor, kClicks, kResolution, kFps, kCodec, kFormat, kStream,
                       kService, kKey, kBitrate, kService2, kReplay, kReplayLength, kChangeFolder})
            EnableWindow(get(id), editable);
        EnableWindow(get(kMicDevice), editable && checked(kMicrophone));
        // Volume and mute change during a recording that has the source.
        for (int id : {kSystemVolume, kSystemMute}) EnableWindow(get(id), editable ? checked(kSystemAudio) : active && s.system_audio);
        for (int id : {kMicVolume, kMicMute}) EnableWindow(get(id), editable ? checked(kMicrophone) : active && s.microphone);
        // Sources are fixed while recording; scenes and where they put the sources are not.
        for (int id : {kCamera, kImage, kBrowseImage, kAddDisplay, kAddWindow}) EnableWindow(get(id), editable);
        EnableWindow(get(kCameraDevice), editable && checked(kCamera));
        const bool arranging = editable || active;
        for (int id : {kScene, kNewScene, kSceneName, kSourceShown}) EnableWindow(get(id), arranging);
        EnableWindow(get(kDeleteScene), arranging && settings_.scenes.size() > 1);
        EnableWindow(get(kRemoveSource), editable && selected_.kind == rec::app::SourceRef::Kind::layer);
        EnableWindow(get(kCorner), arranging && settings_.camera);
        // A dragged camera is sized by its corners.
        EnableWindow(get(kCameraSize), arranging && settings_.camera && selected(kCorner) != choice_index(kCorners, "custom"));
        EnableWindow(get(kServer), editable);
        for (int id : {kServer2, kKey2}) EnableWindow(get(id), editable && settings_.destination2 != "none");
        EnableWindow(get(kShowLast), !settings_.last_recording.empty());

        // Preview frames are made only while the window is on screen: the recording's, or
        // before recording the idle preview's when Show preview is on.
        const bool shown = IsWindowVisible(window_) && !IsIconic(window_);
        const bool before = s.state == State::idle && shown && checked(kShowPreview);
        if (before) {
            read_settings();
            session_.idle_preview(&settings_, test_pattern_);
        } else {
            session_.idle_preview(nullptr);
        }
        const bool previewing = shown && (active || (before && s.previewing));
        if (previewing != previewing_) {
            session_.show_preview(previewing);
            if (previewing) SetTimer(window_, kPreviewTimer, 33, nullptr);
            else KillTimer(window_, kPreviewTimer);
            previewing_ = previewing;
        }
        if (!previewing && frame_.number) {
            frame_ = {};
            InvalidateRect(get(kPreview), nullptr, FALSE);
        }
        const std::wstring hint = active || s.state != State::idle ? L"Starting the preview..."
            : !checked(kShowPreview) ? L"The preview is off"
            : !s.preview_error.empty() ? L"No preview: " + wide(s.preview_error)
            : L"Starting the preview...";
        if (hint != preview_hint_) {
            preview_hint_ = hint;
            InvalidateRect(get(kPreview), nullptr, FALSE);
        }

        const bool poll = s.state != State::idle;
        if (poll && !polling_) SetTimer(window_, kPollTimer, 50, nullptr);
        if (!poll && polling_) KillTimer(window_, kPollTimer);
        polling_ = poll;
        update_tray(active ? recording_icon_ : idle_icon_,
                    active ? (s.state == State::paused ? L"Rec: paused " : L"Rec: recording ") + clock(s.seconds) +
                                 (s.live ? L", live" : L"")
                           : L"Rec");
        if (quitting_ && s.state == State::idle) {
            remember();
            DestroyWindow(window_);
        }
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    UINT taskbar_created_ = 0;
    HFONT font_ = nullptr, bold_ = nullptr, title_ = nullptr;
    IAccPropServices* access_ = nullptr;
    HICON idle_icon_ = nullptr, recording_icon_ = nullptr;
    NOTIFYICONDATAW tray_{};
    std::vector<std::pair<int, HWND>> controls_;
    int content_width_ = 0, content_height_ = 0;
    std::wstring home_;
    bool test_pattern_ = false;
    bool hotkey_ = false, replay_hotkey_ = false;
    std::wstring note_;  // A message that replaces the detail line for a few seconds.
    ULONGLONG note_until_ = 0;
    bool note_attention_ = false;
    bool polling_ = false;
    bool previewing_ = false;
    rec::app::AreaDrag drag_;
    rec::app::SourceRef dragged_;           // The source drag_ holds.
    rec::Area outline_{};                   // Where it is going.
    std::optional<rec::app::SourceRef> hover_;  // The source under the pointer in the preview.
    rec::app::SourceRef selected_;          // The source the Sources list selects.
    bool filling_ = false;                  // The app, not the person, is setting the scene's name.
    std::vector<rec::Window> menu_windows_; // What the Add Window menu offered.
    rec::PreviewFrame frame_;
    std::wstring preview_hint_;  // Shown while the preview has no frame.
    HBRUSH preview_brush_ = nullptr;
    bool quitting_ = false;
    bool attention_ = false;
    State state_ = State::idle;
    float system_shown_ = 0, mic_shown_ = 0;
    std::string settings_path_;
    Settings settings_;
    std::vector<rec::Display> displays_;
    std::vector<rec::AudioDevice> microphones_;
    std::vector<rec::Camera> cameras_;  // "default" first.
    // The Source list: displays, the saved region, "Select a region...", then open windows.
    struct Source {
        enum class Kind { display, region, pick, window } kind = Kind::display;
        std::uint32_t display = 1;
        std::string app, title;
    };
    std::vector<Source> sources_;
    Session session_{[this] { PostMessageW(window_, kSessionChanged, 0, 0); }};
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // One instance: a second launch brings the first window forward.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\rec-app");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"RecWindow", nullptr)) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    const int code = App().run(instance, show);
    CoUninitialize();
    if (single) CloseHandle(single);
    return code;
}
