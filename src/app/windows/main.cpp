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
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincred.h>

#include "app/session.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
constexpr UINT_PTR kPollTimer = 1;
constexpr int kHotkey = 1;
constexpr UINT kTrayId = 1;

enum Control : int {
    kStatus = 100, kDetail, kLive, kSystemLabel, kSystemMeter, kMicLabel, kMicMeter,
    kRecord, kPause,
    kCaptureHeader, kDisplayLabel, kDisplay, kSystemAudio, kMicrophone, kMicDevice, kCursor,
    kQualityHeader, kResolutionLabel, kResolution, kFpsLabel, kFps, kCodecLabel, kCodec, kFormatLabel, kFormat,
    kStreamHeader, kStream, kServiceLabel, kService, kServerLabel, kServer, kKeyLabel, kKey, kBitrateLabel, kBitrate,
    kFolderHeader, kFolder, kChangeFolder, kOpenFolder, kShowLast,
};

enum Command : UINT { kMenuRecord = 1000, kMenuPause, kMenuShow, kMenuFolder, kMenuQuit };

using rec::app::choice_index;
using rec::app::kCodecs;
using rec::app::kFormats;
using rec::app::kFrameRates;
using rec::app::kResolutions;
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
        }).join();

        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = &App::procedure;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"RecWindow";
        RegisterClassExW(&wc);
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
        case WM_TIMER:
            if (w == kPollTimer) refresh();
            return 0;
        case kSessionChanged:
            refresh();
            return 0;
        case WM_HOTKEY:
            if (w == kHotkey) toggle_record();
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
            return TRUE;
        case WM_ENDSESSION:
            if (w) {
                session_.stop();
                session_.wait();
                remember();
            }
            return 0;
        case WM_DESTROY:
            UnregisterHotKey(window_, kHotkey);
            Shell_NotifyIconW(NIM_DELETE, &tray_);
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
            const bool header = id == kCaptureHeader || id == kQualityHeader || id == kStreamHeader || id == kFolderHeader;
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
        const bool plain = id == kStatus || id == kDetail || id == kLive || id == kFolder;
        return add(id, WC_STATICW, text, SS_LEFT | (plain ? SS_NOPREFIX : 0));
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
        label(kSystemLabel, L"System audio level");
        add(kSystemMeter, PROGRESS_CLASSW, L"", PBS_SMOOTH);
        label(kMicLabel, L"Microphone level");
        add(kMicMeter, PROGRESS_CLASSW, L"", PBS_SMOOTH);
        button(kRecord, L"&Start Recording", BS_DEFPUSHBUTTON);
        button(kPause, L"&Pause");

        label(kCaptureHeader, L"Capture");
        label(kDisplayLabel, L"&Display");
        HWND display = combo(kDisplay);
        check(kSystemAudio, L"System &audio");
        check(kMicrophone, L"&Microphone");
        HWND mic = combo(kMicDevice);
        check(kCursor, L"Show &cursor");

        label(kQualityHeader, L"Quality");
        label(kResolutionLabel, L"&Resolution");
        HWND resolution = combo(kResolution);
        label(kFpsLabel, L"&Frame rate");
        HWND fps = combo(kFps);
        label(kCodecLabel, L"Co&dec");
        HWND codec = combo(kCodec);
        label(kFormatLabel, L"F&ormat");
        HWND format = combo(kFormat);

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

        label(kFolderHeader, L"Recordings");
        label(kFolder, L"");
        button(kChangeFolder, L"C&hange Folder...");
        button(kOpenFolder, L"Open Fo&lder");
        button(kShowLast, L"Show Last Recordin&g");

        for (std::size_t i = 0; i < displays_.size(); ++i) {
            wchar_t name[96];
            std::swprintf(name, 96, L"Display %zu (%ux%u)", i + 1, displays_[i].width, displays_[i].height);
            SendMessageW(display, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        }
        if (displays_.empty()) SendMessageW(display, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Display 1"));
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
        for (auto rate : kFrameRates) SendMessageW(fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((std::to_wstring(rate) + L" fps").c_str()));
        for (int meter : {kSystemMeter, kMicMeter}) SendMessageW(get(meter), PBM_SETRANGE32, 0, 1000);

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
        refresh();
    }

    // One column: full-width rows, and label/control rows for the settings.
    void layout() {
        const int margin = px(20), width = px(440), label_width = px(130), row = px(26), gap = px(8);
        int y = margin;
        const auto place = [&](int id, int x, int top, int w, int h) { MoveWindow(get(id), x, top, w, h, FALSE); };
        const auto line = [&](int id, int h) { place(id, margin, y, width, h); y += h + gap; };
        const auto pair = [&](int label_id, int control_id, int h) {
            place(label_id, margin, y + px(4), label_width - gap, px(20));
            // A drop-down list's height includes its open list.
            const bool list = control_id == kDisplay || control_id == kMicDevice || control_id == kResolution ||
                              control_id == kFps || control_id == kCodec || control_id == kFormat || control_id == kService;
            place(control_id, margin + label_width, y, width - label_width, list ? px(240) : h);
            y += h + gap;
        };
        const auto header = [&](int id) { y += px(10); line(id, px(20)); };

        line(kStatus, px(34));
        place(kDetail, margin, y, width - px(80), px(20));
        place(kLive, margin + width - px(70), y, px(70), px(20));
        y += px(20) + gap;
        pair(kSystemLabel, kSystemMeter, px(14));
        pair(kMicLabel, kMicMeter, px(14));
        place(kRecord, margin, y, width - px(130), px(40));
        place(kPause, margin + width - px(120), y, px(120), px(40));
        y += px(40) + gap;

        header(kCaptureHeader);
        pair(kDisplayLabel, kDisplay, row);
        line(kSystemAudio, px(22));
        place(kMicrophone, margin, y, label_width - gap, px(22));
        place(kMicDevice, margin + label_width, y, width - label_width, px(240));
        y += row + gap;
        line(kCursor, px(22));

        header(kQualityHeader);
        pair(kResolutionLabel, kResolution, row);
        pair(kFpsLabel, kFps, row);
        pair(kCodecLabel, kCodec, row);
        pair(kFormatLabel, kFormat, row);

        header(kStreamHeader);
        line(kStream, px(22));
        pair(kServiceLabel, kService, row);
        pair(kServerLabel, kServer, row);
        pair(kKeyLabel, kKey, row);
        pair(kBitrateLabel, kBitrate, row);

        header(kFolderHeader);
        line(kFolder, px(20));
        const int third = (width - 2 * gap) / 3;
        place(kChangeFolder, margin, y, third, px(30));
        place(kOpenFolder, margin + third + gap, y, third, px(30));
        place(kShowLast, margin + 2 * (third + gap), y, third, px(30));
        y += px(30) + margin;
        content_height_ = y;
        InvalidateRect(window_, nullptr, TRUE);
    }

    void fit() {
        RECT frame{0, 0, px(480), content_height_};
        AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongW(window_, GWL_STYLE)), FALSE,
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
        if (id == kDetail || id == kFolder) return GetSysColor(COLOR_GRAYTEXT);
        return GetSysColor(COLOR_WINDOWTEXT);
    }

    void select(int id, int index) { SendMessageW(get(id), CB_SETCURSEL, static_cast<WPARAM>(index), 0); }
    int selected(int id) const { return static_cast<int>(SendMessageW(get(id), CB_GETCURSEL, 0, 0)); }
    void set_check(int id, bool on) { SendMessageW(get(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
    bool checked(int id) const { return SendMessageW(get(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }

    void show_settings() {
        const auto& s = settings_;
        select(kDisplay, std::clamp<int>(static_cast<int>(s.display) - 1, 0, std::max<int>(0, static_cast<int>(displays_.size()) - 1)));
        set_check(kSystemAudio, s.system_audio);
        set_check(kMicrophone, s.microphone);
        int mic = 0;
        for (std::size_t i = 0; i < microphones_.size(); ++i)
            if (microphones_[i].id == s.microphone_device) mic = static_cast<int>(i) + 1;
        select(kMicDevice, mic);
        set_check(kCursor, s.cursor);
        select(kResolution, choice_index(kResolutions, s.resolution));
        select(kFps, static_cast<int>(std::find(kFrameRates.begin(), kFrameRates.end(), s.fps) - kFrameRates.begin()) % static_cast<int>(kFrameRates.size()));
        select(kCodec, choice_index(kCodecs, s.codec));
        select(kFormat, choice_index(kFormats, s.format));
        set_check(kStream, s.stream);
        select(kService, choice_index(kServices, s.destination));
        SetWindowTextW(get(kBitrate), std::to_wstring(s.stream_kbps).c_str());
        show_server();
        SetWindowTextW(get(kFolder), wide(s.folder).c_str());
    }

    // Presets show their fixed server; a custom server is typed in.
    void show_server() {
        const bool custom = settings_.destination == "custom";
        SetWindowTextW(get(kServer), wide(custom ? settings_.server : rec::app::server_url(settings_)).c_str());
        SendMessageW(get(kServer), EM_SETREADONLY, !custom, 0);
        SetWindowTextW(get(kKey), wide(load_key(rec::app::server_url(settings_))).c_str());
    }

    void read_settings() {
        auto& s = settings_;
        s.display = static_cast<std::uint32_t>(std::max(0, selected(kDisplay))) + 1;
        s.system_audio = checked(kSystemAudio);
        s.microphone = checked(kMicrophone);
        const int mic = selected(kMicDevice);
        s.microphone_device = mic > 0 && mic <= static_cast<int>(microphones_.size()) ? microphones_[mic - 1].id : "default";
        s.cursor = checked(kCursor);
        const auto at = [this](int id) { return static_cast<std::size_t>(std::max(0, selected(id))); };
        s.resolution = kResolutions[at(kResolution)].value;
        s.fps = kFrameRates[at(kFps)];
        s.codec = kCodecs[at(kCodec)].value;
        s.format = kFormats[at(kFormat)].value;
        s.stream = checked(kStream);
        s.destination = kServices[at(kService)].value;
        if (s.destination == "custom") s.server = narrow(text_of(get(kServer)));
        const auto kbps = std::wcstoul(text_of(get(kBitrate)).c_str(), nullptr, 10);
        s.stream_kbps = static_cast<std::uint32_t>(std::clamp<unsigned long>(kbps ? kbps : 6000, 500, 50000));
    }

    void remember() {
        read_settings();
        rec::app::save_settings(settings_path_, settings_);
    }

    void command(int id, int code) {
        switch (id) {
        case kRecord: case kMenuRecord: toggle_record(); return;
        case kPause: case kMenuPause: toggle_pause(); return;
        case kMenuShow: show_window(); return;
        case kOpenFolder: case kMenuFolder: open_folder(); return;
        case kMenuQuit: quit(); return;
        case kChangeFolder: choose_folder(); return;
        case kShowLast: show_last(); return;
        case kService:
            if (code == CBN_SELCHANGE) {
                read_settings();
                show_server();
                refresh();
            }
            return;
        case kServer:
            if (code == EN_KILLFOCUS && settings_.destination == "custom") {
                read_settings();
                SetWindowTextW(get(kKey), wide(load_key(rec::app::server_url(settings_))).c_str());
            }
            return;
        case kStream: case kSystemAudio: case kMicrophone:
            if (code == BN_CLICKED) refresh();
            return;
        default:
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
            if (auto started = session_.start(settings_, key, test_pattern_); !started) {
                MessageBoxW(window_, wide(started.error).c_str(), L"Rec could not start recording", MB_ICONWARNING);
                return;
            }
            if (settings_.stream) store_key(rec::app::server_url(settings_), key);
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
        }

        std::wstring status, detail;
        attention_ = false;
        switch (s.state) {
        case State::idle:
            status = L"Ready to record";
            if (!s.error.empty()) { detail = L"Recording failed: " + wide(s.error); attention_ = true; }
            else if (!s.stream_error.empty()) { detail = L"Stream stopped early: " + wide(s.stream_error); attention_ = true; }
            else if (!s.saved.empty()) detail = L"Saved " + wide(s.saved.substr(s.saved.find_last_of("/\\") + 1));
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
        }
        set_text(kStatus, status);
        set_text(kDetail, detail);
        set_text(kLive, active && s.live ? L"\u25CF LIVE" : L"");
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
        const bool editable = s.state == State::idle;
        for (int id : {kDisplay, kSystemAudio, kMicrophone, kCursor, kResolution, kFps, kCodec, kFormat, kStream,
                       kService, kKey, kBitrate, kChangeFolder})
            EnableWindow(get(id), editable);
        EnableWindow(get(kMicDevice), editable && checked(kMicrophone));
        EnableWindow(get(kServer), editable);
        EnableWindow(get(kShowLast), !settings_.last_recording.empty());

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
    HICON idle_icon_ = nullptr, recording_icon_ = nullptr;
    NOTIFYICONDATAW tray_{};
    std::vector<std::pair<int, HWND>> controls_;
    int content_height_ = 0;
    std::wstring home_;
    bool test_pattern_ = false;
    bool hotkey_ = false;
    bool polling_ = false;
    bool quitting_ = false;
    bool attention_ = false;
    State state_ = State::idle;
    float system_shown_ = 0, mic_shown_ = 0;
    std::string settings_path_;
    Settings settings_;
    std::vector<rec::Display> displays_;
    std::vector<rec::AudioDevice> microphones_;
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
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    const int code = App().run(instance, show);
    CoUninitialize();
    if (single) CloseHandle(single);
    return code;
}
