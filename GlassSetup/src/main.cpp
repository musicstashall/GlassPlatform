// Win32-мастер GlassPlatform. Release ставит MSI. Debug без GLASS_EMBED_MSI только имитирует прогресс.
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <objidl.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <winver.h>
#include <wrl.h>
#include <WebView2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>
#include "resource.h"
#include "InstallEngine.h"
#include "InstallLog.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace {
constexpr int kWidth = 1000, kHtmlHeight = 600, kProgressHeight = 4;
constexpr int kContentHeight = kHtmlHeight + kProgressHeight;
constexpr int kFooterHeight = 56, kHeight = kContentHeight + kFooterHeight;
constexpr UINT_PTR kInstallTimer = 1, kFadeTimer = 2;
constexpr UINT kInstallTimerMs = 16;
constexpr ULONGLONG kMarqueePeriodMs = 1400;
constexpr double kProgressSmoothTauMs = 120.0;
constexpr UINT kInitWebView = WM_APP + 1;
constexpr UINT kCheckForUpdate = WM_APP + 2;
constexpr ULONGLONG kFadeDuration = 280;
#ifndef GLASS_EMBED_MSI
constexpr ULONGLONG kDuration = 30000;
#endif
constexpr COLORREF kDark = RGB(16,18,37), kLight = RGB(247,248,252);
constexpr wchar_t kDocumentUrl[] = L"https://glassplatform.invalid/index.html";
enum class Page { Welcome, Update, License, Installing, ModulesBusy, ProcessesStuck, Complete, Error };
HINSTANCE gInstance = nullptr;
LANGID gLanguage = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

// Explicit language lookup: never rely on the OS's resource fallback order.
std::wstring Text(UINT id) {
    HRSRC resource = FindResourceExW(gInstance, RT_STRING,
        MAKEINTRESOURCEW((id >> 4) + 1), gLanguage);
    if (!resource) resource = FindResourceExW(gInstance, RT_STRING,
        MAKEINTRESOURCEW((id >> 4) + 1), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    if (!resource) return L"";
    const auto data = static_cast<const WORD*>(LockResource(LoadResource(gInstance, resource)));
    if (!data) return L"";
    const WORD* entry = data;
    for (UINT i = 0; i < (id & 15); ++i) entry += 1 + *entry;
    return std::wstring(reinterpret_cast<const wchar_t*>(entry + 1), *entry);
}
struct Bytes { const BYTE* data = nullptr; DWORD size = 0; };
Bytes Resource(UINT id, LANGID lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL)) {
    HRSRC res = FindResourceExW(gInstance, RT_RCDATA, MAKEINTRESOURCEW(id), lang);
    if (!res) return {};
    return {static_cast<const BYTE*>(LockResource(LoadResource(gInstance, res))),
            SizeofResource(gInstance, res)};
}
std::wstring Utf8(Bytes b) {
    if (!b.data || !b.size) return L"";
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(b.data), static_cast<int>(b.size), nullptr, 0);
    if (!count) return L"";
    std::wstring text(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(b.data), static_cast<int>(b.size), text.data(), count);
    if (!text.empty() && text.front() == 0xFEFF) text.erase(0, 1);
    // EDIT controls need CRLF, whereas the supplied text files use LF.
    std::wstring result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i-1] != L'\r')) result += L'\r';
        result += text[i];
    }
    return result;
}

std::wstring ReadVersionStringField(const std::wstring& path, const wchar_t* fieldName, ULONGLONG& key, bool useProductFixed) {
    key = 0;
    DWORD unused = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (!bytes) return L"";
    std::vector<BYTE> block(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, block.data())) return L"";
    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedSize = 0;
    if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixedInfo), &fixedSize) || !fixedInfo)
        return L"";
    const uint32_t ms = useProductFixed ? fixedInfo->dwProductVersionMS : fixedInfo->dwFileVersionMS;
    const uint32_t ls = useProductFixed ? fixedInfo->dwProductVersionLS : fixedInfo->dwFileVersionLS;
    key = (static_cast<ULONGLONG>(ms) << 32) | ls;
    LPWSTR text = nullptr;
    UINT textChars = 0;
    wchar_t subBlock[128]{};
    swprintf_s(subBlock, L"\\StringFileInfo\\040904b0\\%s", fieldName);
    if (VerQueryValueW(block.data(), subBlock, reinterpret_cast<void**>(&text), &textChars) && text && *text)
        return text;
    wchar_t fallback[64]{};
    swprintf_s(fallback, L"%u.%u.%u.%u",
        HIWORD(ms), LOWORD(ms), HIWORD(ls), LOWORD(ls));
    return fallback;
}

std::wstring ReadFileVersionString(const std::wstring& path, ULONGLONG& key) {
    return ReadVersionStringField(path, L"FileVersion", key, false);
}

std::wstring ReadProductVersionString(const std::wstring& path, ULONGLONG& key) {
    return ReadVersionStringField(path, L"ProductVersion", key, true);
}

std::wstring SelfProductVersion() {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return L"";
    ULONGLONG key = 0;
    return ReadProductVersionString(exe, key);
}

bool VersionOlder(const std::wstring& installed, const std::wstring& latest) {
    auto parse = [](const std::wstring& raw, int out[4]) {
        out[0] = out[1] = out[2] = out[3] = 0;
        std::wstring s = raw;
        if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s.erase(0, 1);
        int field = 0, value = 0;
        for (wchar_t ch : s) {
            if (ch == L'.') {
                if (field < 4) out[field++] = value;
                value = 0;
                if (field >= 4) break;
            } else if (ch >= L'0' && ch <= L'9') {
                value = value * 10 + (ch - L'0');
            }
        }
        if (field < 4) out[field] = value;
    };
    int a[4]{}, b[4]{};
    parse(installed, a);
    parse(latest, b);
    for (int i = 0; i < 4; ++i) {
        if (a[i] < b[i]) return true;
        if (a[i] > b[i]) return false;
    }
    return false;
}

std::string JsonStringField(const std::string& json, const char* name) {
    const std::string key = std::string("\"") + name + "\"";
    const auto pos = json.find(key);
    if (pos == std::string::npos) return {};
    auto colon = json.find(':', pos + key.size());
    if (colon == std::string::npos) return {};
    auto first = json.find('"', colon + 1);
    if (first == std::string::npos) return {};
    auto last = json.find('"', first + 1);
    if (last == std::string::npos) return {};
    return json.substr(first + 1, last - first - 1);
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return L"";
    int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return L"";
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
    return wide;
}

bool FetchLatestSetupVersion(std::wstring& latest, std::wstring& url) {
    latest.clear();
    url.clear();
    HINTERNET session = WinHttpOpen(L"GlassSetup", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 8000);
    HINTERNET connect = WinHttpConnect(session, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) { WinHttpCloseHandle(session); return false; }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET",
        L"/musicstashall/GlassPlatform/releases/latest/download/version.json",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    std::string body;
    if (ok) {
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
            body.append(chunk.data(), read);
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (body.empty()) return false;
    const std::string glass = JsonStringField(body, "glass");
    if (glass.empty()) {
        const std::string setup = JsonStringField(body, "setup");
        if (setup.empty()) return false;
        latest = Utf8ToWide(setup);
    } else {
        latest = Utf8ToWide(glass);
    }
    const std::string page = JsonStringField(body, "url");
    if (page.empty()) return false;
    url = Utf8ToWide(page);
    return true;
}

std::wstring FormatUpdateBody(const std::wstring& self, const std::wstring& latest) {
    const std::wstring pattern = Text(IDS_UPDATE_BODY);
    std::wstring out;
    out.reserve(pattern.size() + self.size() + latest.size());
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == L'{' && i + 2 < pattern.size() && pattern[i + 2] == L'}') {
            out += pattern[i + 1] == L'0' ? self : latest;
            i += 2;
            continue;
        }
        out += pattern[i];
    }
    return out;
}

std::wstring PlatformVersionFromInstalledModules() {
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return L"";
    PathRemoveFileSpecW(exe);
    const wchar_t* relative[] = {
        L"GlassCore.dll",
        L"..\\..\\x64\\Debug\\GlassCore.dll",
        L"..\\..\\x64\\Release\\GlassCore.dll",
        L"..\\..\\x86\\Debug\\GlassCore.dll",
        L"..\\..\\x86\\Release\\GlassCore.dll",
    };
    std::wstring best;
    ULONGLONG bestKey = 0;
    for (const wchar_t* item : relative) {
        wchar_t combined[MAX_PATH]{};
        if (!PathCombineW(combined, exe, item)) continue;
        wchar_t full[MAX_PATH]{};
        if (!PathCanonicalizeW(full, combined)) continue;
        ULONGLONG key = 0;
        const std::wstring text = ReadProductVersionString(full, key);
        if (text.empty()) continue;
        if (best.empty() || key > bestKey) {
            best = text;
            bestKey = key;
        }
    }
    return best;
}

// A reusable top-down DIB: no allocate/delete churn on every animation frame.
struct Surface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    int width = 0, height = 0;
    Surface() = default;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    ~Surface() { Reset(); }
    void Reset() {
        if (dc && previous) SelectObject(dc,previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc=nullptr; bitmap=nullptr; previous=nullptr; width=height=0;
    }
    bool Ensure(HDC reference, int w, int h) {
        if (dc && width==w && height==h) return true;
        Reset();
        if (w<=0 || h<=0) return false;
        dc=CreateCompatibleDC(reference);
        if (!dc) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=w; info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
        info.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;
        bitmap=CreateDIBSection(reference,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if (!bitmap) { Reset(); return false; }
        previous=SelectObject(dc,bitmap);
        width=w; height=h;
        return true;
    }
};

class Installer {
public:
    HWND window = nullptr;
    bool Create(int show) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = 0; // Fixed-size window: do not invalidate the entire client on sizing.
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = gInstance;
        wc.hIcon = LoadIconW(gInstance, MAKEINTRESOURCEW(IDI_APP));
        wc.hIconSm = wc.hIcon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr; // Client painting is committed in one buffered blit.
        wc.lpszClassName = L"GlassSetupWindow";
        if (!RegisterClassExW(&wc)) return false;
        dpi_ = GetDpiForSystem();
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                 WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        RECT bounds{0,0,Scale(kWidth),Scale(kHeight)};
        AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, dpi_);
        const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
        POINT cursor{}; GetCursorPos(&cursor);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
        const int x = static_cast<int>(mi.rcWork.left + std::max<LONG>(0, (mi.rcWork.right - mi.rcWork.left - width) / 2));
        const int y = static_cast<int>(mi.rcWork.top + std::max<LONG>(0, (mi.rcWork.bottom - mi.rcWork.top - height) / 2));
        auto title = Text(IDS_TITLE);
        window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, title.c_str(), style,
                                 x, y, width, height, nullptr, nullptr, gInstance, this);
        if (!window) return false;
        // Initial monitor DPI may differ from GetDpiForSystem(). Normalize the
        // client size before showing the window, so secondary-monitor launches
        // use exactly the same logical layout as later WM_DPICHANGED events.
        dpi_ = GetDpiForWindow(window);
        RECT actual{0,0,Scale(kWidth),Scale(kHeight)};
        AdjustWindowRectExForDpi(&actual,style,FALSE,WS_EX_CONTROLPARENT,dpi_);
        GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&mi);
        const int finalWidth = actual.right-actual.left, finalHeight = actual.bottom-actual.top;
        const int finalX = static_cast<int>(mi.rcWork.left +
            std::max<LONG>(0,(mi.rcWork.right-mi.rcWork.left-finalWidth)/2));
        const int finalY = static_cast<int>(mi.rcWork.top +
            std::max<LONG>(0,(mi.rcWork.bottom-mi.rcWork.top-finalHeight)/2));
        SetWindowPos(window,nullptr,finalX,finalY,finalWidth,finalHeight,SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(window, show);
        UpdateWindow(window); // Первый кадр до WebView: тёмный фон, слайды появятся после готовности.
        PostMessageW(window, kInitWebView, 0, 0);
        PostMessageW(window, kCheckForUpdate, 0, 0);
        return true;
    }
private:
    Page page_ = Page::Welcome;
    UINT dpi_ = 96;
    HWND back_ = nullptr, next_ = nullptr, cancel_ = nullptr;
    HWND updateDownload_ = nullptr, updateSkip_ = nullptr;
    HWND eula_ = nullptr, accept_ = nullptr, progress_ = nullptr, webHost_ = nullptr;
    HFONT font_ = nullptr, heading_ = nullptr, small_ = nullptr, title_ = nullptr;
    HBRUSH darkBrush_ = nullptr, lightBrush_ = nullptr;
    HBRUSH progressFill_ = nullptr, progressTrack_ = nullptr;
    HTHEME panelTheme_ = nullptr;
    ComPtr<IStream> bannerStream_;
    std::unique_ptr<Gdiplus::Bitmap> banner_;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> web_;
    bool accepted_ = false, closing_ = false, webReady_ = false;
    std::wstring platformVersion_;
    bool transitioning_ = false, bufferedErase_ = false;
    ULONGLONG fadeStarted_ = 0;
    Surface frame_, fadeFrame_;
    RECT webBounds_{};
    bool haveWebBounds_ = false;
    HRESULT webError_ = S_OK;
    ULONGLONG started_ = 0;
    int percent_ = 0, progressValue_ = 0, progressTarget_ = 0, displayProgress_ = 0;
    double marqueePhase_ = 0;
    ULONGLONG progressTickMs_ = 0;
    bool progressMarquee_ = false;
    bool progressFinishing_ = false;
    bool simulateInstall_ = false;
    std::wstring installStatusLine_;
    std::wstring updateLatest_;
    std::wstring updateUrl_;
    std::wstring updateBody_;
    std::wstring errorReason_;
    std::wstring holdReport_;
    InstallEngine installEngine_;
    std::wstring userDataFolder_;
    EventRegistrationToken requestToken_{}, navigationToken_{}, completeToken_{},
                           popupToken_{}, permissionToken_{};

    int Scale(int n) const { return MulDiv(n, static_cast<int>(dpi_), 96); }
    RECT Box(int x, int y, int w, int h) const {
        return {Scale(x), Scale(y), Scale(x+w), Scale(y+h)};
    }
    HWND Control(const wchar_t* cls, const std::wstring& text, DWORD style, int id, DWORD exStyle=0) {
        return CreateWindowExW(exStyle, cls, text.c_str(), WS_CHILD | style,
            0,0,0,0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), gInstance, nullptr);
    }
    void Move(HWND control, int x, int y, int w, int h) {
        if (!control) return;
        RECT current{}; GetWindowRect(control,&current);
        MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&current),2);
        RECT wanted=Box(x,y,w,h);
        if (EqualRect(&current,&wanted)) return;
        SetWindowPos(control,nullptr,wanted.left,wanted.top,wanted.right-wanted.left,
                     wanted.bottom-wanted.top,SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    }
    void CreateFonts() {
        for (HFONT* f : {&font_, &heading_, &small_, &title_}) {
            if (*f) DeleteObject(*f);
            *f = nullptr;
        }
        auto create = [this](int pixels, int weight) {
            return CreateFontW(-Scale(pixels),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                              DEFAULT_PITCH,L"Segoe UI");
        };
        font_ = create(14,FW_NORMAL); small_ = create(12,FW_NORMAL);
        heading_ = create(28,FW_NORMAL); title_ = create(39,FW_LIGHT);
        for (HWND c : {back_,next_,cancel_,eula_,accept_,updateDownload_,updateSkip_})
            if (c) SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
    }
    void OpenTheme() {
        if (panelTheme_) CloseThemeData(panelTheme_);
        panelTheme_ = OpenThemeData(window, L"Explorer::ControlPanel");
    }
    void Initialize() {
        dpi_ = GetDpiForWindow(window);
        platformVersion_ = PlatformVersionFromInstalledModules();
        darkBrush_ = CreateSolidBrush(kDark);
        lightBrush_ = CreateSolidBrush(kLight);
        progressFill_ = CreateSolidBrush(RGB(144,127,230));
        progressTrack_ = CreateSolidBrush(RGB(42,45,67));
        back_ = Control(L"BUTTON",Text(IDS_BACK),BS_PUSHBUTTON | WS_TABSTOP,IDC_BACK);
        next_ = Control(L"BUTTON",Text(IDS_NEXT),BS_DEFPUSHBUTTON | WS_TABSTOP,IDC_NEXT);
        cancel_ = Control(L"BUTTON",Text(IDS_CANCEL),BS_PUSHBUTTON | WS_TABSTOP,IDC_CANCEL);
        updateDownload_ = Control(L"BUTTON",Text(IDS_UPDATE_DOWNLOAD),BS_PUSHBUTTON | WS_TABSTOP,IDC_UPDATE_DOWNLOAD);
        updateSkip_ = Control(L"BUTTON",Text(IDS_UPDATE_SKIP),BS_DEFPUSHBUTTON | WS_TABSTOP,IDC_UPDATE_SKIP);
        eula_ = Control(L"EDIT",L"",WS_BORDER | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                         ES_AUTOVSCROLL | ES_LEFT | WS_VSCROLL,IDC_EULA);
        accept_ = Control(L"BUTTON",Text(IDS_ACCEPT),BS_AUTOCHECKBOX | WS_TABSTOP,IDC_ACCEPT);
        // Owner-drawn STATIC avoids the classic PROGRESS control's sunken border,
        // which would consume most/all of a 4 px high strip.
        progress_ = Control(L"STATIC",L"",SS_OWNERDRAW,IDC_PROGRESS);
        // A hidden native parent prevents the asynchronously created WebView
        // from ever appearing above Welcome during its first initialization.
        webHost_ = Control(L"STATIC",L"",WS_CLIPCHILDREN | WS_CLIPSIBLINGS,IDC_WEBHOST,WS_EX_CONTROLPARENT);
        SetWindowTheme(eula_,L"Explorer",nullptr);
        SetWindowTheme(accept_,L"Explorer",nullptr);
        CreateFonts();
        auto agreement = Utf8(Resource(IDR_EULA,gLanguage));
        agreement += L"\r\n\r\n────────────────────────────────\r\n" +
                     Text(IDS_GPL_APPENDIX) + L"\r\n\r\n" + Utf8(Resource(IDR_GPL));
        SendMessageW(eula_,EM_SETLIMITTEXT,1024*1024,0);
        SetWindowTextW(eula_,agreement.c_str());
        SendMessageW(eula_,EM_SETMARGINS,EC_LEFTMARGIN | EC_RIGHTMARGIN,MAKELPARAM(14,14));
        const auto image = Resource(IDR_WELCOME);
        bannerStream_.Attach(SHCreateMemStream(image.data,image.size));
        if (bannerStream_) {
            banner_.reset(Gdiplus::Bitmap::FromStream(bannerStream_.Get()));
            if (banner_ && banner_->GetLastStatus() != Gdiplus::Ok) banner_.reset();
        }
        OpenTheme();
        Layout();
        SetPage(Page::Welcome);
    }
    void Layout() {
        Move(back_,644,kContentHeight+12,96,32);
        // Complete uses the exact rectangle previously occupied by Cancel.
        const bool unlock = page_==Page::ProcessesStuck;
        Move(next_,(page_==Page::Complete || page_==Page::Error)?864:(unlock?694:750),kContentHeight+12,unlock?160:104,32);
        Move(cancel_,864,kContentHeight+12,104,32);
        Move(updateDownload_,644,kContentHeight+12,104,32);
        Move(updateSkip_,750,kContentHeight+12,104,32);
        Move(eula_,48,132,904,382);
        Move(accept_,48,540,904,30);
        Move(progress_,0,kHtmlHeight,kWidth,kProgressHeight);
        Move(webHost_,0,0,kWidth,kHtmlHeight);
        if (controller_) {
            RECT bounds=Box(0,0,kWidth,kHtmlHeight);
            if (!haveWebBounds_ || !EqualRect(&bounds,&webBounds_)) {
                controller_->put_Bounds(bounds);
                webBounds_=bounds; haveWebBounds_=true;
            }
        }
    }
    void InvalidateContent() {
        RECT content=Box(0,0,kWidth,kContentHeight);
        InvalidateRect(window,&content,FALSE);
    }
    void InvalidateFooter() {
        RECT footer=Box(0,kContentHeight,kWidth,kFooterHeight);
        InvalidateRect(window,&footer,FALSE);
    }
    void ShowContentControls() {
        ShowWindow(eula_,page_==Page::License && !transitioning_?SW_SHOWNA:SW_HIDE);
        ShowWindow(accept_,page_==Page::License && !transitioning_?SW_SHOWNA:SW_HIDE);
        const bool showProgress = page_==Page::Installing || page_==Page::Complete;
        ShowWindow(progress_, showProgress ? SW_SHOWNA : SW_HIDE);
        const bool update = page_==Page::Update && !transitioning_;
        ShowWindow(updateDownload_,update?SW_SHOWNA:SW_HIDE);
        ShowWindow(updateSkip_,update?SW_SHOWNA:SW_HIDE);
        const bool slides = SlidesVisible();
        if (controller_) controller_->put_IsVisible(slides ? TRUE : FALSE);
        ShowWindow(webHost_,slides?SW_SHOWNA:SW_HIDE);
    }
    [[nodiscard]] bool SlidesVisible() const noexcept {
        return webReady_ && !transitioning_ &&
            (page_==Page::Welcome || page_==Page::Installing);
    }
    void PushPlatformVersion() {
        if (!web_ || platformVersion_.empty()) return;
        std::wstring script = L"window.GlassSlideshow?.setPlatformVersion('";
        for (wchar_t ch : platformVersion_) {
            if (ch == L'\\' || ch == L'\'') script += L'\\';
            script += ch;
        }
        script += L"');";
        web_->ExecuteScript(script.c_str(), nullptr);
    }
    void SyncSlideshow() {
        if (!web_ || !webReady_) return;
        const bool play = page_==Page::Welcome || page_==Page::Installing;
        web_->ExecuteScript(play ? L"window.GlassSlideshow?.setPaused(false);"
                                 : L"window.GlassSlideshow?.setPaused(true);", nullptr);
        PushPlatformVersion();
    }
    void FocusPage() {
        SetFocus(page_==Page::ModulesBusy || page_==Page::ProcessesStuck ? next_ :
                 page_==Page::Update ? updateSkip_ :
                 page_==Page::License && !accepted_ ? accept_ :
                 page_==Page::Installing ? cancel_ : next_);
    }
    void PrintControl(HDC dc, HWND control) {
        RECT r{}; GetWindowRect(control,&r);
        MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);
        const int saved=SaveDC(dc);
        SetViewportOrgEx(dc,r.left,r.top,nullptr);
        IntersectClipRect(dc,0,0,r.right-r.left,r.bottom-r.top);
        // No PRF_CHECKVISIBLE: destination controls stay hidden during the fade.
        SendMessageW(control,WM_PRINT,reinterpret_cast<WPARAM>(dc),
                     PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
        RestoreDC(dc,saved);
    }
    void BeginTransition() {
        BOOL enabled=TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0);
        if (!enabled) return;
        HDC dc=GetDC(window);
        const bool allocated=fadeFrame_.Ensure(dc,Scale(kWidth),Scale(kContentHeight));
        ReleaseDC(window,dc);
        if (!allocated) return;
        // Fade in the complete destination page, including a snapshot of native controls.
        // We never try to PrintWindow the GPU-rendered WebView2 surface.
        RenderContent(fadeFrame_.dc);
        if (page_==Page::License) {
            PrintControl(fadeFrame_.dc,eula_);
            PrintControl(fadeFrame_.dc,accept_);
        }
        fadeStarted_=GetTickCount64();
        transitioning_=true;
        if (!SetTimer(window,kFadeTimer,15,nullptr)) transitioning_=false;
    }
    void EndTransition() {
        if (!transitioning_) return;
        KillTimer(window,kFadeTimer);
        transitioning_=false;
        ShowContentControls();
        EnableWindow(back_,TRUE);
        EnableWindow(next_,page_!=Page::License || accepted_);
        FocusPage();
        InvalidateContent();
    }
    void SetPage(Page page) {
        const Page oldPage=page_;
        if (transitioning_) EndTransition();
        page_=page;
        BOOL darkCaption=page_!=Page::License;
        if (FAILED(DwmSetWindowAttribute(window,20,&darkCaption,sizeof(darkCaption))))
            DwmSetWindowAttribute(window,19,&darkCaption,sizeof(darkCaption));
        ShowWindow(eula_,SW_HIDE); ShowWindow(accept_,SW_HIDE);
        Layout();
        const UINT nextId = page_==Page::License ? IDS_INSTALL :
            (page_==Page::Complete || page_==Page::Error) ? IDS_FINISH :
            page_==Page::ModulesBusy ? IDS_HOLD_RETRY :
            page_==Page::ProcessesStuck ? IDS_UNLOCK : IDS_NEXT;
        SetWindowTextW(next_,Text(nextId).c_str());
        ShowWindow(back_,page_==Page::License?SW_SHOWNA:SW_HIDE);
        ShowWindow(next_,(page_==Page::Installing || page_==Page::Update)?SW_HIDE:SW_SHOWNA);
        ShowWindow(cancel_,(page_==Page::Complete || page_==Page::Error)?SW_HIDE:SW_SHOWNA);
        EnableWindow(next_,page_!=Page::License || accepted_);
        if ((oldPage==Page::Welcome && page_==Page::License) ||
            (oldPage==Page::Installing && (page_==Page::Complete || page_==Page::Error))) BeginTransition();
        ShowContentControls();
        EnableWindow(back_,!transitioning_);
        EnableWindow(next_,!transitioning_ && (page_!=Page::License || accepted_));
        if (!transitioning_) FocusPage(); else if (page_==Page::License) SetFocus(cancel_);
        SyncSlideshow();
        // One non-erasing repaint for a real page change, never a clear-then-paint cycle.
        InvalidateRect(window,nullptr,FALSE);
    }
    static bool DefaultProgramGlassDir(bool x64, std::wstring& out) {
        if (x64) {
            wchar_t buffer[MAX_PATH]{};
            if (!GetEnvironmentVariableW(L"ProgramW6432", buffer, MAX_PATH)) return false;
            out.assign(buffer);
        } else {
            PWSTR path = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, KF_FLAG_DEFAULT, nullptr, &path)))
                return false;
            out.assign(path);
            CoTaskMemFree(path);
        }
        out += L"\\GlassPlatform";
        return true;
    }
    void ShowInstallError(const std::wstring& reason) noexcept {
        errorReason_ = reason;
        KillTimer(window, kInstallTimer);
        SetPage(Page::Error);
    }
    void StartInstallation() {
        if (!accepted_ || transitioning_) return;
        started_ = GetTickCount64();
        percent_ = 0;
        progressValue_ = 0;
        progressTarget_ = 0;
        displayProgress_ = 0;
        progressFinishing_ = false;
        progressTickMs_ = GetTickCount64();
        installStatusLine_ = Text(IDS_STATUS_EXTRACT);
        InvalidateRect(progress_, nullptr, FALSE);
        SetPage(Page::Installing);
#ifndef GLASS_EMBED_MSI
        simulateInstall_ = true;
        installStatusLine_ = Text(IDS_SIMULATION_NOTE);
        InstallLog::Line(L"Debug build: simulated installation, platform is not stopped");
        SetTimer(window, kInstallTimer, kInstallTimerMs, nullptr);
        return;
#else
        std::wstring x64Dir, x86Dir, ogDir;
        if (!DefaultProgramGlassDir(true, x64Dir) || !DefaultProgramGlassDir(false, x86Dir)) {
            InstallLog::Line(L"Install path resolution failed (Program Files)");
            ShowInstallError(Text(IDS_ERR_INSTALL_PATH));
            return;
        }
        ogDir = x64Dir + L"\\OpenGlass";
        InstallLog::Line(L"User started installation");
        InstallLog::LineFormat(L"Target x64=%s x86=%s OpenGlass=%s", x64Dir.c_str(), x86Dir.c_str(), ogDir.c_str());
        InstallEngine::PhaseLabels labels;
        labels.stop64 = Text(IDS_STATUS_STOP64);
        labels.stop86 = Text(IDS_STATUS_STOP86);
        labels.stopOg = Text(IDS_STATUS_STOPOG);
        labels.unload = Text(IDS_STATUS_UNLOAD);
        labels.start64 = Text(IDS_STATUS_START64);
        labels.start86 = Text(IDS_STATUS_START86);
        labels.startOg = Text(IDS_STATUS_STARTOG);
        labels.extract = Text(IDS_STATUS_EXTRACT);
        labels.platform64 = Text(IDS_STATUS_X64);
        labels.platform32 = Text(IDS_STATUS_X86);
        labels.openGlass = Text(IDS_STATUS_OG);
        labels.finalize = Text(IDS_STATUS_FINAL);
        installEngine_.Start(gInstance, x64Dir, x86Dir, ogDir, true, std::move(labels), nullptr);
        SetTimer(window, kInstallTimer, kInstallTimerMs, nullptr);
#endif
    }
    void CheckForNewerSetup() {
        if (page_ != Page::Welcome) return;
        InstallLog::Line(L"Checking for newer setup on GitHub…");
        std::wstring latest, url;
        if (!FetchLatestSetupVersion(latest, url)) {
            InstallLog::Line(L"Version check: fetch failed or skipped");
            return;
        }
        const std::wstring self = SelfProductVersion();
        InstallLog::LineFormat(L"Version check: local=%s remote=%s", self.c_str(), latest.c_str());
        if (self.empty() || !VersionOlder(self, latest)) {
            InstallLog::Line(L"Version check: no newer setup");
            return;
        }
        if (page_ != Page::Welcome) return;
        updateLatest_ = latest;
        updateUrl_ = url;
        updateBody_ = FormatUpdateBody(self, latest);
        InstallLog::Line(L"Version check: showing update page");
        SetPage(Page::Update);
    }
#ifndef GLASS_EMBED_MSI
    void FinishSimulatedInstall() {
        displayProgress_ = 10000;
        progressValue_ = 10000;
        percent_ = 100;
        KillTimer(window, kInstallTimer);
        SetPage(Page::Complete);
    }
    void TickSimulation() {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG elapsed = now - started_;
        const ULONGLONG dt = progressTickMs_ ? (now - progressTickMs_) : kInstallTimerMs;
        progressTickMs_ = now;
        progressMarquee_ = false;
        progressTarget_ = elapsed >= kDuration ? 10000
            : static_cast<int>(elapsed * 10000 / kDuration);
        const double alpha = std::min(1.0, static_cast<double>(dt) / kProgressSmoothTauMs);
        int step = static_cast<int>(std::lround((progressTarget_ - displayProgress_) * alpha));
        if (step < 1 && progressTarget_ > displayProgress_) step = 1;
        displayProgress_ += step;
        if (displayProgress_ > progressTarget_) displayProgress_ = progressTarget_;
        progressValue_ = displayProgress_;
        percent_ = std::min(100, displayProgress_ / 100);
        installStatusLine_ = Text(IDS_SIMULATION_NOTE);
        if (elapsed >= kDuration && displayProgress_ >= 10000) {
            FinishSimulatedInstall();
            return;
        }
        InvalidateRect(progress_, nullptr, FALSE);
        InvalidateFooter();
    }
#endif
    void Tick() {
#ifndef GLASS_EMBED_MSI
        if (simulateInstall_) {
            TickSimulation();
            return;
        }
#endif
        if (page_ != Page::Installing && page_ != Page::ModulesBusy && page_ != Page::ProcessesStuck) return;
        const auto hold = installEngine_.CurrentHold();
        if (hold == InstallEngine::HoldKind::Modules && page_ != Page::ModulesBusy) {
            holdReport_ = installEngine_.HoldReport();
            SetPage(Page::ModulesBusy);
            return;
        }
        if (hold == InstallEngine::HoldKind::Processes && page_ != Page::ProcessesStuck) {
            holdReport_ = installEngine_.HoldReport();
            SetPage(Page::ProcessesStuck);
            return;
        }
        if (hold == InstallEngine::HoldKind::None &&
            (page_ == Page::ModulesBusy || page_ == Page::ProcessesStuck)) {
            SetPage(Page::Installing);
        }
        const bool animateProgress = page_==Page::Installing || page_==Page::ModulesBusy ||
            page_==Page::ProcessesStuck;
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG dt = progressTickMs_ ? (now - progressTickMs_) : kInstallTimerMs;
        progressTickMs_ = now;
        if (animateProgress) {
            progressMarquee_ = installEngine_.IsMarquee() && !progressFinishing_;
            if (progressMarquee_) {
                // Фаза от абсолютного времени — без накопления ошибки таймера (как в имитации установки).
                marqueePhase_ = static_cast<double>((now - started_) % kMarqueePeriodMs) /
                    static_cast<double>(kMarqueePeriodMs);
            } else {
                if (progressFinishing_)
                    progressTarget_ = 10000;
                else
                    progressTarget_ = std::clamp(installEngine_.ProgressUnits(), 0, 10000);
                const double alpha = std::min(1.0, static_cast<double>(dt) / kProgressSmoothTauMs);
                displayProgress_ += static_cast<int>(std::lround(
                    (progressTarget_ - displayProgress_) * alpha));
                progressValue_ = displayProgress_;
                percent_ = std::min(100, displayProgress_ / 100);
            }
            installStatusLine_ = installEngine_.StatusLine();
        }
        if (page_ != Page::Installing) {
            InvalidateRect(progress_, nullptr, FALSE);
            if (animateProgress) InvalidateFooter();
            return;
        }
        if (installEngine_.Poll()) {
            if (installEngine_.Succeeded() && displayProgress_ < 10000) {
                progressFinishing_ = true;
                progressMarquee_ = false;
                const int gap = 10000 - displayProgress_;
                const double alpha = std::min(1.0, static_cast<double>(dt) / kProgressSmoothTauMs);
                int step = static_cast<int>(std::lround(gap * alpha));
                // lround(малый хвост) даёт 0, и страница «Готово» не открывается, пока тик не станет крупным.
                if (step < 1) step = gap;
                displayProgress_ += step;
                if (displayProgress_ > 10000) displayProgress_ = 10000;
                progressValue_ = displayProgress_;
                percent_ = std::min(100, displayProgress_ / 100);
                installStatusLine_ = installEngine_.StatusLine();
                if (displayProgress_ < 10000) {
                    InvalidateRect(progress_, nullptr, FALSE);
                    InvalidateFooter();
                    return;
                }
            }
            KillTimer(window, kInstallTimer);
            if (installEngine_.QuietClose()) {
                DestroyWindow(window);
                return;
            }
            if (!installEngine_.Succeeded()) {
                const std::wstring err = installEngine_.ErrorText();
                InstallLog::LineFormat(L"Installation failed: %s",
                    err.empty() ? L"(no detail)" : err.c_str());
                std::wstring reason = Text(IDS_ERR_INSTALL_FAILED);
                if (!err.empty()) {
                    reason += L"\r\n\r\n";
                    reason += err;
                }
                ShowInstallError(reason);
                return;
            }
            displayProgress_ = 10000;
            progressValue_ = 10000;
            percent_ = 100;
            InvalidateRect(progress_, nullptr, FALSE);
            InstallLog::Line(L"Complete page");
            SetPage(Page::Complete);
            return;
        }
        InvalidateRect(progress_, nullptr, FALSE);
        InvalidateFooter();
    }
    void RequestClose() {
        if (page_ == Page::ProcessesStuck) {
            installEngine_.SignalHold(InstallEngine::HoldAction::Cancel);
            SetPage(Page::Installing);
            return;
        }
        if (page_ == Page::ModulesBusy) {
            installEngine_.SignalHold(InstallEngine::HoldAction::Cancel);
            DestroyWindow(window);
            return;
        }
        if (page_ == Page::Installing && MessageBoxW(window,Text(IDS_CONFIRM_CANCEL).c_str(),
            Text(IDS_TITLE).c_str(),MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        DestroyWindow(window);
    }
    void WebFailure(HRESULT hr) {
        webError_ = FAILED(hr) ? hr : E_FAIL;
        webReady_ = false;
        if (controller_) controller_->put_IsVisible(FALSE);
        ShowWindow(webHost_,SW_HIDE);
        if (page_==Page::Welcome || page_==Page::Installing) InvalidateContent();
    }
    void InitializeWebView() {
        PWSTR local = nullptr;
        HRESULT folderResult = SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local);
        if (FAILED(folderResult)) { WebFailure(folderResult); return; }
        userDataFolder_ = std::wstring(local) + L"\\GlassPlatform\\InstallerWebView2";
        CoTaskMemFree(local);
        int directoryResult = SHCreateDirectoryExW(window,userDataFolder_.c_str(),nullptr);
        if (directoryResult != ERROR_SUCCESS && directoryResult != ERROR_ALREADY_EXISTS &&
            directoryResult != ERROR_FILE_EXISTS) { WebFailure(HRESULT_FROM_WIN32(directoryResult)); return; }
        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr,userDataFolder_.c_str(),nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (closing_ || !IsWindow(window)) return S_OK;
                if (FAILED(result) || !env) { WebFailure(result); return S_OK; }
                environment_ = env;
                HRESULT create = environment_->CreateCoreWebView2Controller(webHost_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT result2, ICoreWebView2Controller* ctrl) -> HRESULT {
                        if (closing_ || !IsWindow(window)) { if (ctrl) ctrl->Close(); return S_OK; }
                        if (FAILED(result2) || !ctrl) { WebFailure(result2); return S_OK; }
                        controller_ = ctrl;
                        HRESULT get = controller_->get_CoreWebView2(web_.ReleaseAndGetAddressOf());
                        if (FAILED(get) || !web_) { WebFailure(get); return S_OK; }
                        ConfigureWebView();
                        return S_OK;
                    }).Get());
                if (FAILED(create)) WebFailure(create);
                return S_OK;
            }).Get());
        if (FAILED(hr)) WebFailure(hr);
    }
    void ConfigureWebView() {
        controller_->put_IsVisible(FALSE);
        ComPtr<ICoreWebView2Controller2> coloredController;
        if (SUCCEEDED(controller_.As(&coloredController))) {
            COREWEBVIEW2_COLOR background{255,16,18,37};
            coloredController->put_DefaultBackgroundColor(background);
        }
        Layout();
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(web_->get_Settings(&settings))) {
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
            ComPtr<ICoreWebView2Settings3> extended;
            if (SUCCEEDED(settings.As(&extended))) extended->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }
        // Exact in-memory response. No extraction, HTTP server or NavigateToString size limit.
        HRESULT filter = web_->AddWebResourceRequestedFilter(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        if (FAILED(filter)) { WebFailure(filter); return; }
        HRESULT request = web_->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (closing_) return S_OK;
                ComPtr<ICoreWebView2WebResourceRequest> req;
                if (FAILED(args->get_Request(&req))) return E_FAIL;
                LPWSTR uri = nullptr; req->get_Uri(&uri);
                std::wstring url = uri ? uri : L"";
                CoTaskMemFree(uri);
                const bool allowed = url == kDocumentUrl ||
                    url == std::wstring(kDocumentUrl)+L"?lang=ru-RU" ||
                    url == std::wstring(kDocumentUrl)+L"?lang=en-US";
                ComPtr<IStream> body;
                if (allowed) {
                    auto html = Resource(IDR_HTML);
                    body.Attach(SHCreateMemStream(html.data,html.size));
                    if (!body) return E_OUTOFMEMORY;
                }
                ComPtr<ICoreWebView2WebResourceResponse> response;
                HRESULT hr = environment_->CreateWebResourceResponse(body.Get(),allowed?200:403,
                    allowed?L"OK":L"Forbidden",
                    L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
                    L"Content-Security-Policy: default-src 'none'; img-src data:; "
                    L"font-src data:; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                    L"connect-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'\r\n",
                    &response);
                return FAILED(hr) ? hr : args->put_Response(response.Get());
            }).Get(),&requestToken_);
        if (FAILED(request)) { WebFailure(request); return; }
        web_->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr; args->get_Uri(&uri);
                std::wstring url = uri ? uri : L""; CoTaskMemFree(uri);
                if (url != std::wstring(kDocumentUrl)+L"?lang=ru-RU" &&
                    url != std::wstring(kDocumentUrl)+L"?lang=en-US") args->put_Cancel(TRUE);
                return S_OK;
            }).Get(),&navigationToken_);
        web_->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                return args->put_Handled(TRUE);
            }).Get(),&popupToken_);
        web_->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            }).Get(),&permissionToken_);
        web_->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (closing_) return S_OK;
                BOOL success = FALSE; args->get_IsSuccess(&success);
                if (!success) { WebFailure(E_FAIL); return S_OK; }
                webReady_ = true; webError_ = S_OK;
                ShowContentControls();
                SyncSlideshow();
                if (page_==Page::Welcome || page_==Page::Installing) InvalidateContent();
                return S_OK;
            }).Get(),&completeToken_);
        // C++ chooses the language BEFORE navigation / the first rendered HTML frame.
        std::wstring url = std::wstring(kDocumentUrl) +
            (PRIMARYLANGID(gLanguage)==LANG_RUSSIAN ? L"?lang=ru-RU" : L"?lang=en-US");
        HRESULT nav = web_->Navigate(url.c_str());
        if (FAILED(nav)) WebFailure(nav);
    }
    void Erase(HDC dc) {
        // Invoked by WM_ERASEBKGND with the back-buffer DC, not the screen DC.
        // DTB stays in the requested message path but never flashes on screen.
        RECT footer=Box(0,kContentHeight,kWidth,kFooterHeight);
        FillRect(dc,&footer,GetSysColorBrush(COLOR_BTNFACE));
        if (panelTheme_)
            DrawThemeBackground(panelTheme_,dc,CPANEL_SMALLCOMMANDAREA,0,&footer,nullptr);
        HPEN pen = CreatePen(PS_SOLID,1,RGB(207,211,222));
        HGDIOBJ old = SelectObject(dc,pen);
        MoveToEx(dc,footer.left,footer.top,nullptr); LineTo(dc,footer.right,footer.top);
        SelectObject(dc,old); DeleteObject(pen);
    }
    void DrawLabel(HDC dc, const std::wstring& s, RECT rect, HFONT font, COLORREF color,
                   UINT format = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX) {
        HGDIOBJ old = SelectObject(dc,font);
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,color);
        DrawTextW(dc,s.c_str(),static_cast<int>(s.size()),&rect,format);
        SelectObject(dc,old);
    }
    void DrawBanner(HDC dc, bool complete) {
        if (!banner_) return;
        Gdiplus::Graphics g(dc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        Gdiplus::ImageAttributes attrs;
        Gdiplus::ColorMatrix matrix{};
        matrix.m[0][0]=matrix.m[1][1]=matrix.m[2][2]=matrix.m[4][4]=1.0f;
        matrix.m[3][3]=complete?0.25f:0.95f;
        attrs.SetColorMatrix(&matrix);
        Gdiplus::Rect dest(Scale(240),Scale(100),Scale(800),Scale(447));
        g.DrawImage(banner_.get(),dest,0,0,static_cast<INT>(banner_->GetWidth()),
                    static_cast<INT>(banner_->GetHeight()),Gdiplus::UnitPixel,&attrs);
        Gdiplus::LinearGradientBrush shade(
            Gdiplus::Point(Scale(240),0),Gdiplus::Point(Scale(590),0),
            Gdiplus::Color(255,16,18,37),Gdiplus::Color(0,16,18,37));
        g.FillRectangle(&shade,Scale(240),Scale(100),Scale(350),Scale(447));
    }
    void DrawBannerFull(HDC dc) {
        if (!banner_) return;
        Gdiplus::Graphics g(dc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        Gdiplus::Rect dest(0, 0, Scale(kWidth), Scale(kContentHeight));
        g.DrawImage(banner_.get(), dest, 0, 0, static_cast<INT>(banner_->GetWidth()),
                    static_cast<INT>(banner_->GetHeight()), Gdiplus::UnitPixel);
    }
    void RenderContent(HDC dc) {
        RECT content=Box(0,0,kWidth,kContentHeight);
        FillRect(dc,&content,page_==Page::License?lightBrush_:darkBrush_);
        const COLORREF white = RGB(245,244,252), muted = RGB(163,168,191), accent = RGB(193,180,255);
        if (page_ == Page::Complete) {
            DrawBanner(dc,true);
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(IDS_DONE_TAG),Box(48,139,400,25),small_,accent);
            DrawLabel(dc,Text(IDS_DONE_TITLE),Box(48,181,410,120),title_,white);
#ifdef GLASS_EMBED_MSI
            DrawLabel(dc,Text(IDS_DONE_BODY),Box(48,323,328,126),font_,muted);
            DrawLabel(dc,Text(IDS_DONE_NOTE),Box(48,521,430,75),small_,muted);
#else
            DrawLabel(dc,Text(IDS_DONE_BODY_DEMO),Box(48,323,328,126),font_,muted);
            DrawLabel(dc,Text(IDS_DONE_NOTE_DEMO),Box(48,521,430,75),small_,muted);
#endif
        } else if (page_ == Page::Error) {
            DrawBanner(dc,true);
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(IDS_ERROR_TAG),Box(48,139,400,25),small_,accent);
            DrawLabel(dc,Text(IDS_ERROR_TITLE),Box(48,181,410,120),title_,white);
            DrawLabel(dc,Text(IDS_ERROR_BODY),Box(48,323,328,56),font_,muted);
            DrawLabel(dc,errorReason_,Box(48,385,520,120),font_,white,DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            DrawLabel(dc,Text(IDS_ERROR_LOG_HINT),Box(48,521,430,75),small_,muted);
        } else if (page_ == Page::ModulesBusy || page_ == Page::ProcessesStuck) {
            DrawBanner(dc,true);
            const bool kill = page_ == Page::ProcessesStuck;
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(kill ? IDS_KILL_TITLE : IDS_HOLD_TITLE),Box(48,139,520,70),heading_,white);
            DrawLabel(dc,Text(kill ? IDS_KILL_BODY : IDS_HOLD_BODY),Box(48,220,460,140),font_,muted);
            DrawLabel(dc,holdReport_,Box(48,370,520,180),font_,white,DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        } else if (page_ == Page::Update) {
            DrawBannerFull(dc);
            auto shadow = [&](const std::wstring& text, RECT rect, HFONT font, UINT format) {
                RECT back = rect;
                OffsetRect(&back, Scale(1), Scale(1));
                DrawLabel(dc, text, back, font, RGB(16, 18, 37), format);
                DrawLabel(dc, text, rect, font, RGB(255, 255, 255), format);
            };
            shadow(Text(IDS_UPDATE_TITLE), Box(48,48,700,48), heading_, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            shadow(updateBody_, Box(48,112,640,180), font_, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        } else if (page_ == Page::License) {
            DrawLabel(dc,Text(IDS_EULA_TITLE),Box(48,35,904,46),heading_,RGB(31,35,53));
            DrawLabel(dc,Text(IDS_EULA_BODY),Box(48,88,904,32),font_,RGB(97,103,121));
        } else {
            if (!webReady_) {
                std::wstring status = Text(IDS_WEB_LOADING);
                if (FAILED(webError_)) {
                    wchar_t code[16]{};
                    swprintf_s(code,L"0x%08lX",static_cast<unsigned long>(webError_));
                    status = Text(IDS_WEB_ERROR) + code;
                }
                DrawLabel(dc,status,Box(80,250,840,100),font_,muted,DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
            }
        }
    }
    void RenderFooterText(HDC dc) {
        if (page_!=Page::Installing) return;
        const std::wstring line = installStatusLine_.empty() ? Text(IDS_STATUS_PREPARE) : installStatusLine_;
        const std::wstring shown = progressMarquee_ ? line : line + L"  " + std::to_wstring(percent_) + L"%";
        DrawLabel(dc, shown, Box(32,kContentHeight+8,760,23),font_,RGB(40,44,60));
    }
    void Paint() {
        PAINTSTRUCT ps{};
        HDC screen=BeginPaint(window,&ps);
        RECT client{}; GetClientRect(window,&client);
        const bool buffered=frame_.Ensure(screen,client.right,client.bottom);
        HDC dc=buffered?frame_.dc:screen;
        // Status-only updates must not repaint/decode the welcome image or HTML placeholder.
        if (ps.rcPaint.top<Scale(kContentHeight)) {
            if (transitioning_) {
                RECT content=Box(0,0,kWidth,kContentHeight);
                FillRect(dc,&content,page_==Page::License?lightBrush_:darkBrush_);
                const double t=std::min(1.0,static_cast<double>(GetTickCount64()-fadeStarted_)/kFadeDuration);
                const double eased=t*t*(3.0-2.0*t);
                BLENDFUNCTION blend{AC_SRC_OVER,0,static_cast<BYTE>(std::lround(255.0*eased)),0};
                AlphaBlend(dc,0,0,Scale(kWidth),Scale(kContentHeight),fadeFrame_.dc,
                           0,0,fadeFrame_.width,fadeFrame_.height,blend);
            } else RenderContent(dc);
        }
        bufferedErase_=true;
        SendMessageW(window,WM_ERASEBKGND,reinterpret_cast<WPARAM>(dc),0);
        bufferedErase_=false;
        RenderFooterText(dc);
        if (buffered) BitBlt(screen,ps.rcPaint.left,ps.rcPaint.top,
            ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,
            dc,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);
        EndPaint(window,&ps);
    }
    void Cleanup() {
        closing_ = true;
        KillTimer(window,kInstallTimer); KillTimer(window,kFadeTimer);
        frame_.Reset(); fadeFrame_.Reset();
        if (controller_) controller_->Close();
        web_.Reset(); controller_.Reset(); environment_.Reset();
        banner_.reset(); bannerStream_.Reset();
        if (panelTheme_) { CloseThemeData(panelTheme_); panelTheme_=nullptr; }
        for (HFONT f : {font_,heading_,small_,title_}) if (f) DeleteObject(f);
        if (darkBrush_) DeleteObject(darkBrush_);
        if (lightBrush_) DeleteObject(lightBrush_);
        if (progressFill_) DeleteObject(progressFill_);
        if (progressTrack_) DeleteObject(progressTrack_);
    }
    LRESULT Handle(UINT msg, WPARAM w, LPARAM l) {
        switch (msg) {
        case WM_CREATE: Initialize(); return 0;
        case WM_ERASEBKGND:
            if (bufferedErase_) Erase(reinterpret_cast<HDC>(w));
            return 1; // Do not clear the on-screen client before the buffered paint.
        case kInitWebView: if (!closing_) InitializeWebView(); return 0;
        case kCheckForUpdate: if (!closing_) CheckForNewerSetup(); return 0;
        case WM_PAINT: Paint(); return 0;
        case WM_SIZE: Layout(); return 0;
        case WM_MOVE: if (controller_) controller_->NotifyParentWindowPositionChanged(); return 0;
        case WM_THEMECHANGED:
            EndTransition(); OpenTheme(); InvalidateRect(window,nullptr,FALSE); return 0;
        case WM_DPICHANGED: {
            EndTransition();
            frame_.Reset(); fadeFrame_.Reset(); haveWebBounds_=false;
            dpi_ = HIWORD(w);
            auto suggested = reinterpret_cast<RECT*>(l);
            RECT size{0,0,Scale(kWidth),Scale(kHeight)};
            AdjustWindowRectExForDpi(&size,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),
                                    FALSE,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),dpi_);
            SetWindowPos(window,nullptr,suggested->left,suggested->top,size.right-size.left,
                         size.bottom-size.top,SWP_NOACTIVATE | SWP_NOZORDER);
            CreateFonts(); OpenTheme(); Layout(); InvalidateRect(window,nullptr,FALSE);
            RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE | RDW_ALLCHILDREN); return 0;
        }
        case WM_DRAWITEM: {
            const auto item=reinterpret_cast<const DRAWITEMSTRUCT*>(l);
            if (item && item->CtlID==IDC_PROGRESS) {
                FillRect(item->hDC,&item->rcItem,progressTrack_);
                if (progressMarquee_) {
                    const int width = item->rcItem.right - item->rcItem.left;
                    const int span = std::max(width / 3, 48);
                    const int travel = width + span;
                    const int left = item->rcItem.left + static_cast<int>(marqueePhase_ * travel) - span;
                    RECT chip{left, item->rcItem.top, left + span, item->rcItem.bottom};
                    IntersectRect(&chip, &chip, &item->rcItem);
                    if (chip.right > chip.left)
                        FillRect(item->hDC,&chip,progressFill_);
                } else {
                    RECT filled=item->rcItem;
                    filled.right=filled.left+MulDiv(filled.right-filled.left,progressValue_,10000);
                    if (filled.right > filled.left)
                        FillRect(item->hDC,&filled,progressFill_);
                }
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(w);
            const bool license = page_ == Page::License;
            SetTextColor(dc,license?RGB(31,35,53):RGB(245,244,252));
            SetBkColor(dc,license?kLight:kDark);
            return reinterpret_cast<LRESULT>(license?lightBrush_:darkBrush_);
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
            case IDC_ACCEPT:
                if (HIWORD(w)==BN_CLICKED && page_==Page::License) {
                    accepted_ = SendMessageW(accept_,BM_GETCHECK,0,0)==BST_CHECKED;
                    EnableWindow(next_,accepted_);
                }
                return 0;
            case IDC_UPDATE_DOWNLOAD:
                if (page_==Page::Update && !updateUrl_.empty()) {
                    InstallLog::LineFormat(L"Update download page: %s", updateUrl_.c_str());
                    ShellExecuteW(window, L"open", updateUrl_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                return 0;
            case IDC_UPDATE_SKIP:
                if (page_==Page::Update) {
                    InstallLog::Line(L"Update skipped");
                    SetPage(Page::Welcome);
                }
                return 0;
            case IDC_BACK: if (!transitioning_ && page_==Page::License) SetPage(Page::Welcome); return 0;
            case IDOK:
            case IDC_NEXT:
                if (transitioning_) return 0;
                if (page_==Page::ModulesBusy) {
                    installEngine_.SignalHold(InstallEngine::HoldAction::Retry);
                    SetPage(Page::Installing);
                    return 0;
                }
                if (page_==Page::ProcessesStuck) {
                    installEngine_.SignalHold(InstallEngine::HoldAction::Unlock);
                    SetPage(Page::Installing);
                    return 0;
                }
                if (page_==Page::Welcome) SetPage(Page::License);
                else if (page_==Page::License && accepted_) StartInstallation();
                else if (page_==Page::Complete || page_==Page::Error) RequestClose();
                return 0;
            case IDCANCEL:
            case IDC_CANCEL: RequestClose(); return 0;
            }
            break;
        case WM_TIMER:
            if (w==kInstallTimer) Tick();
            else if (w==kFadeTimer && transitioning_) {
                if (GetTickCount64()-fadeStarted_>=kFadeDuration) EndTransition();
                else InvalidateContent();
            }
            return 0;
        case WM_CLOSE: RequestClose(); return 0;
        case WM_DESTROY: Cleanup(); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window,msg,w,l);
    }
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        Installer* self = reinterpret_cast<Installer*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (msg==WM_NCCREATE) {
            self = static_cast<Installer*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window = hwnd;
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Handle(msg,w,l) : DefWindowProcW(hwnd,msg,w,l);
    }
};
// Process-lifetime state also protects pending WebView2 callbacks during window shutdown.
Installer gInstaller;
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    gInstance = instance;
    gLanguage = PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN ?
        MAKELANGID(LANG_RUSSIAN,SUBLANG_DEFAULT) : MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US);
    SetThreadUILanguage(gLanguage);
    InstallLog::Initialize();
    HRESULT com = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        InstallLog::LineFormat(L"COM init failed: 0x%08X", static_cast<unsigned>(com));
    }
    LPWSTR runtimeVersion = nullptr;
    HRESULT runtime = GetAvailableCoreWebView2BrowserVersionString(nullptr,&runtimeVersion);
    CoTaskMemFree(runtimeVersion);
    if (FAILED(runtime)) {
        InstallLog::LineFormat(L"WebView2 runtime missing: 0x%08X", static_cast<unsigned>(runtime));
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput graphicsInput;
    if (Gdiplus::GdiplusStartup(&gdiplusToken,&graphicsInput,nullptr)!=Gdiplus::Ok) {
        CoUninitialize(); return 3;
    }
    int exitCode = 0;
    if (!gInstaller.Create(show)) {
        InstallLog::Line(L"Could not create setup window");
        exitCode = 4;
    } else {
        MSG message{};
        BOOL result = 0;
        while ((result=GetMessageW(&message,nullptr,0,0))>0) {
            if (!IsDialogMessageW(gInstaller.window,&message)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
        exitCode = result==-1 ? 5 : static_cast<int>(message.wParam);
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    InstallLog::Shutdown();
    CoUninitialize();
    return exitCode;
}
