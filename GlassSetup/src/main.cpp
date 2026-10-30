// Win32-мастер GlassPlatform. Release устанавливает MSI; Debug предлагает симуляцию.
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
#include <shobjidl.h>
#include <exdisp.h>
#include <servprov.h>
#include <shldisp.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <winver.h>
#include <wintrust.h>
#include <softpub.h>
#include <wrl.h>
#include <WebView2.h>
#include <detours.h>
#include <Helpers/ThemeHelper.h>
#include "WizardTheme.h"
#include "WizardAssets.h"
#include "InstallSupport/GlassInstallMsi.h"
#include "InstallSupport/GlassInstallUtil.h"
#ifdef ShellExecute
#undef ShellExecute // IShellDispatch2::ShellExecute is a COM method, not the Win32 macro.
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string.h>
#include <memory>
#include <iterator>
#include <mutex>
#include <string>
#include <tlhelp32.h>
#include <thread>
#include <vector>
#include "resource.h"
#include "InstallEngine.h"
#include "InstallLog.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace {
// 1000×600 total client area: 540 px content, 4 px progress strip, 56 px footer.
constexpr int kWidth = 1000, kHtmlHeight = 540, kProgressHeight = 4;
constexpr int kContentHeight = kHtmlHeight + kProgressHeight;
constexpr int kFooterHeight = 56, kHeight = 600;
constexpr UINT_PTR kInstallTimer = 1, kFadeTimer = 2, kHeroTimer = 3;
constexpr UINT kInstallTimerMs = 16, kHeroTimerMs = 50;
constexpr ULONGLONG kMarqueePeriodMs = 1400;
constexpr double kProgressSmoothTauMs = 120.0;
constexpr UINT kInitWebView = WM_APP + 1;
constexpr UINT kVersionCheckComplete = WM_APP + 2;
constexpr UINT kRuntimeStatus = WM_APP + 3, kRuntimeComplete = WM_APP + 4;
constexpr ULONGLONG kFadeDuration = 280;
#ifndef GLASS_EMBED_MSI
constexpr ULONGLONG kDuration = 30000;
#endif
constexpr COLORREF kDark = RGB(16,18,37), kLicensePanel = RGB(27,29,52);
constexpr COLORREF kFooterTop = RGB(26,27,50), kFooterBottom = RGB(17,18,35);
constexpr COLORREF kButtonBase = RGB(24,25,47);
constexpr COLORREF kLicenseText = RGB(215,217,232);
constexpr wchar_t kDocumentUrl[] = L"https://glassplatform.invalid/index.html";
constexpr wchar_t kWebView2BootstrapperUrl[] = L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";
#ifdef _DEBUG
constexpr UINT_PTR kDebugDemoTimer = 4;
constexpr UINT kDebugDemoPageMs = 1800;
#endif
enum class Page { RuntimeRequired, RuntimeInstalling, RuntimeError, AlreadyRunning, Welcome, Checking, Update, Conflicts, Downgrade, VersionInvalid, License, Components, Installing, ModulesBusy, ProcessesStuck, ConfirmCancel, Cancelling, Cancelled, Launching, Complete, Error };
enum class VisualMode { Slideshow, UnlockModules, UnlockProcesses };
HINSTANCE gInstance = nullptr;
HBRUSH gWindowClassBrush = nullptr;
bool gWindowClassRegistered = false;
HWND gRootWindow = nullptr;
bool gAlreadyRunning = false;
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
    wchar_t subBlock[128]{};
    LPWSTR text=nullptr;
    UINT textChars=0;
    // Prefer the project default translation, then try every translation declared
    // in the file's version resource (including non-English localized builds).
    swprintf_s(subBlock,L"\\StringFileInfo\\040904b0\\%s",fieldName);
    if (VerQueryValueW(block.data(),subBlock,reinterpret_cast<void**>(&text),&textChars) && text && *text)
        return text;
    struct Translation { WORD language; WORD codePage; };
    Translation* translations=nullptr;
    UINT translationBytes=0;
    if (VerQueryValueW(block.data(),L"\\VarFileInfo\\Translation",
                       reinterpret_cast<void**>(&translations),&translationBytes) && translations) {
        const size_t count=translationBytes/sizeof(Translation);
        for (size_t i=0;i<count;++i) {
            swprintf_s(subBlock,L"\\StringFileInfo\\%04x%04x\\%s",
                static_cast<unsigned>(translations[i].language),
                static_cast<unsigned>(translations[i].codePage),fieldName);
            text=nullptr; textChars=0;
            if (VerQueryValueW(block.data(),subBlock,reinterpret_cast<void**>(&text),&textChars) && text && *text)
                return text;
        }
    }
    // ProductVersion is a safety gate for installed GlassPlatform and must be
    // explicitly present. Do not fabricate it from the fixed file-version tuple.
    if (useProductFixed) return L"";
    wchar_t fallback[64]{};
    swprintf_s(fallback, L"%u.%u.%u.%u",
        HIWORD(ms), LOWORD(ms), HIWORD(ls), LOWORD(ls));
    return fallback;
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

struct ParsedVersion {
    std::array<unsigned,4> fields{};
};

bool TryParseVersion(const std::wstring& raw, ParsedVersion& parsed) noexcept {
    parsed = {};
    if (raw.empty()) return false;
    size_t begin = 0, end = raw.size();
    while (begin < end && std::iswspace(raw[begin])) ++begin;
    while (end > begin && std::iswspace(raw[end-1])) --end;
    if (begin < end && (raw[begin] == L'v' || raw[begin] == L'V')) ++begin;
    if (begin >= end) return false;
    unsigned field = 0, value = 0;
    bool haveDigit = false;
    for (size_t i = begin; i < end; ++i) {
        const wchar_t ch = raw[i];
        if (ch == L'.') {
            if (!haveDigit || field >= 3) return false;
            parsed.fields[field++] = value;
            value = 0;
            haveDigit = false;
            continue;
        }
        if (ch < L'0' || ch > L'9') return false;
        const unsigned digit = static_cast<unsigned>(ch-L'0');
        if (value > (65535u-digit)/10u) return false;
        value = value*10u+digit;
        haveDigit = true;
    }
    if (!haveDigit || field < 2) return false; // At least major.minor.patch.
    parsed.fields[field] = value;
    return parsed.fields[0] != 0;
}

int CompareVersions(const ParsedVersion& left, const ParsedVersion& right) noexcept {
    for (size_t i = 0; i < left.fields.size(); ++i) {
        if (left.fields[i] < right.fields[i]) return -1;
        if (left.fields[i] > right.fields[i]) return 1;
    }
    return 0;
}

bool VersionOlder(const std::wstring& installed, const std::wstring& latest) {
    ParsedVersion a{}, b{};
    return TryParseVersion(installed,a) && TryParseVersion(latest,b) && CompareVersions(a,b) < 0;
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

bool IsAllowedReleaseUrl(const std::wstring& url) noexcept {
    URL_COMPONENTS parts{}; parts.dwStructSize=sizeof(parts);
    parts.dwSchemeLength=parts.dwHostNameLength=parts.dwUrlPathLength=static_cast<DWORD>(-1);
    parts.dwUserNameLength=parts.dwPasswordLength=static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(),0,0,&parts) || parts.nScheme!=INTERNET_SCHEME_HTTPS ||
        parts.nPort!=INTERNET_DEFAULT_HTTPS_PORT || parts.dwUserNameLength || parts.dwPasswordLength)
        return false;
    const std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
    const std::wstring path(parts.lpszUrlPath,parts.dwUrlPathLength);
    constexpr wchar_t prefix[]=L"/musicstashall/GlassPlatform/releases/";
    if (_wcsicmp(host.c_str(),L"github.com")!=0 || path.rfind(prefix,0)!=0) return false;
    // Do not allow encoded separators, backslashes, control characters or path traversal.
    if (path.find(L'%')!=std::wstring::npos || path.find(L'\\')!=std::wstring::npos ||
        path.find(L"..")!=std::wstring::npos) return false;
    for (wchar_t c:url) if (c<0x20 || c==0x7f) return false;
    return true;
}

// ShellExecute through Explorer's desktop shell, not the elevated setup token.
bool OpenReleaseInUserBrowser(const std::wstring& url) noexcept {
    if (!IsAllowedReleaseUrl(url)) return false;
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_LOCAL_SERVER,IID_PPV_ARGS(&windows)))) return false;
    VARIANT location{},root{}; VariantInit(&location); VariantInit(&root);
    location.vt=VT_I4; location.lVal=CSIDL_DESKTOP;
    long shellHwnd=0; ComPtr<IDispatch> desktop;
    if (FAILED(windows->FindWindowSW(&location,&root,SWC_DESKTOP,&shellHwnd,SWFO_NEEDDISPATCH,&desktop)) || !desktop) return false;
    ComPtr<IServiceProvider> provider;
    if (FAILED(desktop.As(&provider))) return false;
    ComPtr<IShellBrowser> browser;
    if (FAILED(provider->QueryService(SID_STopLevelBrowser,IID_PPV_ARGS(&browser)))) return false;
    ComPtr<IShellView> view;
    if (FAILED(browser->QueryActiveShellView(&view)) || !view) return false;
    ComPtr<IDispatch> background;
    if (FAILED(view->GetItemObject(SVGIO_BACKGROUND,IID_PPV_ARGS(&background)))) return false;
    ComPtr<IShellFolderViewDual> folder;
    if (FAILED(background.As(&folder))) return false;
    ComPtr<IDispatch> application;
    if (FAILED(folder->get_Application(&application)) || !application) return false;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(application.As(&shell))) return false;
    BSTR file=SysAllocString(url.c_str());
    if (!file) return false;
    VARIANT empty{},show{}; VariantInit(&empty); VariantInit(&show);
    show.vt=VT_I4; show.lVal=SW_SHOWNORMAL;
    const HRESULT hr=shell->ShellExecute(file,empty,empty,empty,show);
    SysFreeString(file);
    return SUCCEEDED(hr);
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
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    DWORD httpStatus=0, httpStatusSize=sizeof(httpStatus);
    ok = ok && WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,&httpStatus,&httpStatusSize,WINHTTP_NO_HEADER_INDEX) && httpStatus==200;
    std::string body;
    if (ok) {
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) { ok=false; break; }
            if (available == 0) break;
            if (available>256*1024 || body.size()+available>256*1024) { ok=false; break; }
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) { ok=false; break; }
            body.append(chunk.data(), read);
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!ok || body.empty()) return false;
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
    return IsAllowedReleaseUrl(url);
}

bool IsWebView2RuntimeInstalled(std::wstring* versionOut=nullptr) {
    LPWSTR version=nullptr;
    const HRESULT hr=GetAvailableCoreWebView2BrowserVersionString(nullptr,&version);
    const bool installed=SUCCEEDED(hr) && version && *version;
    if (versionOut) *versionOut=installed?version:L"";
    CoTaskMemFree(version);
    return installed;
}

bool CreateWebView2TempPath(std::wstring& directory, std::wstring& installer,
                            std::wstring& error) {
    if (!GlassInstallUtil::CreateSecureTempDirectory(L"GlassWebView2",directory)) {
        error=L"Could not create a private temporary folder ("+std::to_wstring(GetLastError())+L")";
        return false;
    }
    installer=directory+L"\\InstallerWebView2.exe";
    return true;
}

void RemoveWebView2TempFiles(const std::wstring& directory,
                             const std::wstring& installer) noexcept {
    if (!installer.empty()) DeleteFileW(installer.c_str());
    if (!directory.empty()) RemoveDirectoryW(directory.c_str());
}

bool DownloadWebView2Bootstrapper(const std::wstring& outputPath, std::wstring& error, const std::atomic_bool& cancelled) {
    if (cancelled.load()) { error=L"Cancelled"; return false; }
    URL_COMPONENTS parts{};
    parts.dwStructSize=sizeof(parts);
    parts.dwSchemeLength=static_cast<DWORD>(-1);
    parts.dwHostNameLength=static_cast<DWORD>(-1);
    parts.dwUrlPathLength=static_cast<DWORD>(-1);
    parts.dwExtraInfoLength=static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(kWebView2BootstrapperUrl,0,0,&parts) ||
        parts.nScheme!=INTERNET_SCHEME_HTTPS) {
        error=L"The Microsoft WebView2 bootstrapper URL is invalid";
        return false;
    }
    const std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
    std::wstring objectName(parts.lpszUrlPath,parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) objectName.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
    HINTERNET session=WinHttpOpen(L"GlassSetup WebView2 prerequisite",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if (!session) {
        error=L"WinHttpOpen failed ("+std::to_wstring(GetLastError())+L")";
        return false;
    }
    WinHttpSetTimeouts(session,5000,5000,8000,8000);
    HINTERNET connection=WinHttpConnect(session,host.c_str(),parts.nPort,0);
    if (!connection) {
        error=L"WinHttpConnect failed ("+std::to_wstring(GetLastError())+L")";
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request=WinHttpOpenRequest(connection,L"GET",objectName.c_str(),nullptr,
        WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    if (!request) {
        error=L"WinHttpOpenRequest failed ("+std::to_wstring(GetLastError())+L")";
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return false;
    }
    DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect));
    bool ok=WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,
        WINHTTP_NO_REQUEST_DATA,0,0,0) && WinHttpReceiveResponse(request,nullptr);
    DWORD status=0,statusSize=sizeof(status);
    if (ok) ok=WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,&status,&statusSize,WINHTTP_NO_HEADER_INDEX) &&
        status>=200 && status<300;
    DWORD expected=0,expectedSize=sizeof(expected);
    const bool hasExpected=ok && WinHttpQueryHeaders(request,
        WINHTTP_QUERY_CONTENT_LENGTH|WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,&expected,&expectedSize,WINHTTP_NO_HEADER_INDEX);
    constexpr ULONGLONG kMaximumBootstrapperBytes=64ULL*1024*1024;
    if (hasExpected && expected>kMaximumBootstrapperBytes) {
        error=L"Microsoft WebView2 bootstrapper download exceeded its size limit";
        ok=false;
    }
    HANDLE file=INVALID_HANDLE_VALUE;
    if (ok) {
        file=CreateFileW(outputPath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
                         FILE_ATTRIBUTE_TEMPORARY,nullptr);
        if (file==INVALID_HANDLE_VALUE) {
            error=L"Could not create InstallerWebView2.exe ("+
                std::to_wstring(GetLastError())+L")";
            ok=false;
        }
    }
    ULONGLONG total=0;
    std::vector<BYTE> buffer(64*1024);
    while (ok) {
        if (cancelled.load()) { error=L"Cancelled"; ok=false; break; }
        DWORD available=0;
        if (!WinHttpQueryDataAvailable(request,&available)) {
            error=L"WinHttpQueryDataAvailable failed ("+
                std::to_wstring(GetLastError())+L")";
            ok=false;
            break;
        }
        if (!available) break;
        while (ok && available) {
            if (cancelled.load()) { error=L"Cancelled"; ok=false; break; }
            const DWORD toRead=std::min<DWORD>(available,static_cast<DWORD>(buffer.size()));
            DWORD read=0;
            if (!WinHttpReadData(request,buffer.data(),toRead,&read) || !read) {
                error=L"WinHttpReadData failed ("+std::to_wstring(GetLastError())+L")";
                ok=false;
                break;
            }
            total+=read;
            if (total>kMaximumBootstrapperBytes) {
                error=L"Microsoft WebView2 bootstrapper download exceeded its size limit";
                ok=false;
                break;
            }
            DWORD offset=0;
            while (ok && offset<read) {
                DWORD written=0;
                if (!WriteFile(file,buffer.data()+offset,read-offset,&written,nullptr) || !written) {
                    error=L"Could not write InstallerWebView2.exe ("+
                        std::to_wstring(GetLastError())+L")";
                    ok=false;
                    break;
                }
                offset+=written;
            }
            available-=std::min(available,read);
        }
    }
    if (ok && hasExpected && total!=expected) {
        error=L"The Microsoft WebView2 bootstrapper download was incomplete";
        ok=false;
    }
    if (file!=INVALID_HANDLE_VALUE) {
        if (ok && !FlushFileBuffers(file)) {
            error=L"Could not flush InstallerWebView2.exe ("+
                std::to_wstring(GetLastError())+L")";
            ok=false;
        }
        CloseHandle(file);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (ok && total<100*1024) {
        error=L"The downloaded WebView2 bootstrapper is unexpectedly small";
        ok=false;
    }
    if (!ok) DeleteFileW(outputPath.c_str());
    return ok;
}

bool VerifyMicrosoftSignature(const std::wstring& path, std::wstring& error) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct=sizeof(file);
    file.pcwszFilePath=path.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct=sizeof(trust);
    trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE;
    trust.dwUnionChoice=WTD_CHOICE_FILE;
    trust.pFile=&file;
    trust.dwStateAction=WTD_STATEACTION_VERIFY;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status=WinVerifyTrust(nullptr,&action,&trust);
    bool microsoftPublisher=false;
    bool microsoftRoot=false;
    if (status==ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* provider=WTHelperProvDataFromStateData(trust.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        if (signer && signer->csCertChain && signer->pasCertChain && signer->pasCertChain[0].pCert) {
            wchar_t publisher[256]{};
            const DWORD chars=CertGetNameStringW(signer->pasCertChain[0].pCert,
                CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,publisher,static_cast<DWORD>(std::size(publisher)));
            microsoftPublisher=chars>1 && _wcsicmp(publisher,L"Microsoft Corporation")==0;
            CERT_CHAIN_POLICY_PARA policy{}; policy.cbSize=sizeof(policy);
            CERT_CHAIN_POLICY_STATUS chainStatus{}; chainStatus.cbSize=sizeof(chainStatus);
            microsoftRoot=signer->pChainContext && CertVerifyCertificateChainPolicy(
                CERT_CHAIN_POLICY_MICROSOFT_ROOT,signer->pChainContext,&policy,&chainStatus) && chainStatus.dwError==0;
        }
    }
    trust.dwStateAction=WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr,&action,&trust);
    if (status!=ERROR_SUCCESS) {
        error=L"The downloaded WebView2 installer did not pass Authenticode verification ("+
            std::to_wstring(static_cast<unsigned long>(status))+L")";
        return false;
    }
    if (!microsoftPublisher || !microsoftRoot) {
        error=L"The downloaded WebView2 installer is not signed by Microsoft Corporation with a Microsoft root";
        return false;
    }
    return true;
}

bool RunWebView2Bootstrapper(const std::wstring& installerPath,
                             const std::wstring& directory,
                             DWORD& exitCode, std::wstring& error, const std::atomic_bool& cancelled) {
    std::wstring command=L"\""+installerPath+L"\" /silent /install";
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(installerPath.c_str(),command.data(),nullptr,nullptr,FALSE,
                        CREATE_NO_WINDOW,nullptr,directory.c_str(),&startup,&process)) {
        error=L"Could not start InstallerWebView2.exe ("+
            std::to_wstring(GetLastError())+L")";
        return false;
    }
    // Bound the wait. Cancellation is honoured while the bootstrapper is running too.
    const ULONGLONG deadline=GetTickCount64()+300000;
    DWORD wait=WAIT_TIMEOUT;
    while (GetTickCount64()<deadline && !cancelled.load()) {
        wait=WaitForSingleObject(process.hProcess,200);
        if (wait!=WAIT_TIMEOUT) break;
    }
    if (wait==WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess,ERROR_CANCELLED);
        WaitForSingleObject(process.hProcess,5000);
    }
    const DWORD waitError=wait==WAIT_FAILED?GetLastError():ERROR_SUCCESS;
    const BOOL gotExit=wait==WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess,&exitCode);
    const DWORD exitError=!gotExit?GetLastError():ERROR_SUCCESS;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!gotExit) {
        error=L"Waiting for the WebView2 installer failed ("+
            std::to_wstring(waitError?waitError:exitError)+L")";
        return false;
    }
    return true;
}

bool HasCommandLineSwitch(const wchar_t* name) {
    int argc=0;
    LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (!argv) return false;
    bool found=false;
    for (int i=1;i<argc;++i) {
        if (_wcsicmp(argv[i],name)==0) { found=true; break; }
    }
    LocalFree(argv);
    return found;
}

std::wstring JsStringLiteral(const std::wstring& value) {
    std::wstring out = L"\"";
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': out += L"\\\\"; break;
        case L'\"': out += L"\\\""; break;
        case L'\b': out += L"\\b"; break;
        case L'\f': out += L"\\f"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (ch < 0x20 || ch == 0x2028 || ch == 0x2029 || ch == L'<') {
                wchar_t escaped[8]{};
                swprintf_s(escaped, L"\\u%04X", static_cast<unsigned>(ch));
                out += escaped;
            } else out += ch;
        }
    }
    out += L'\"';
    return out;
}

void AddRoundedRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r,
                        Gdiplus::REAL radius) {
    const Gdiplus::REAL d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// Disambiguate GDI+ overloads that accept either INT or REAL coordinates.
template <typename T>
constexpr INT GdiInt(T value) noexcept { return static_cast<INT>(value); }
template <typename T>
constexpr Gdiplus::REAL GdiReal(T value) noexcept { return static_cast<Gdiplus::REAL>(value); }

std::wstring FormatVersionPairText(UINT resourceId, const std::wstring& first,
                                   const std::wstring& second) {
    const std::wstring pattern=Text(resourceId);
    std::wstring out;
    out.reserve(pattern.size()+first.size()+second.size());
    for (size_t i=0;i<pattern.size();++i) {
        if (pattern[i]==L'{' && i+2<pattern.size() && pattern[i+2]==L'}' &&
            (pattern[i+1]==L'0' || pattern[i+1]==L'1')) {
            out+=pattern[i+1]==L'0'?first:second;
            i+=2;
        } else out+=pattern[i];
    }
    return out;
}

std::wstring FormatUpdateBody(const std::wstring& self, const std::wstring& latest) {
    return FormatVersionPairText(IDS_UPDATE_BODY,self,latest);
}

struct InstallRootCandidate {
    std::wstring path;
    bool fromRegistry = false;
};

struct InstalledGlassVersionInfo {
    bool detected = false;
    bool valid = false;
    bool hasUnreadableInstallation = false;
    std::wstring version;
    std::wstring modulePath;
    ParsedVersion parsed{};
};

bool ReadRegistryString(HKEY key, const wchar_t* valueName, std::wstring& value,
                        DWORD* typeOut = nullptr) {
    value.clear();
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key,valueName,nullptr,&type,nullptr,&bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t) || bytes > 64*1024)
        return false;
    std::vector<wchar_t> buffer(bytes/sizeof(wchar_t)+1,L'\0');
    if (RegQueryValueExW(key,valueName,nullptr,&type,
            reinterpret_cast<LPBYTE>(buffer.data()),&bytes) != ERROR_SUCCESS)
        return false;
    buffer.back() = L'\0';
    value.assign(buffer.data());
    if (typeOut) *typeOut=type;
    return !value.empty();
}

void AddInstallRoot(std::vector<InstallRootCandidate>& roots, std::wstring path, bool fromRegistry) {
    while (!path.empty() && (path.back()==L'\\' || path.back()==L'/')) path.pop_back();
    if (path.empty()) return;
    for (auto& root : roots) {
        if (_wcsicmp(root.path.c_str(),path.c_str())==0) {
            root.fromRegistry = root.fromRegistry || fromRegistry;
            return;
        }
    }
    roots.push_back({std::move(path),fromRegistry});
}

std::vector<InstallRootCandidate> InstalledGlassRoots() {
    std::vector<InstallRootCandidate> roots;
    for (bool x64 : {true,false}) {
        std::wstring path;
        if (GlassInstallUtil::QueryPlatformInstallPath(x64,path))
            AddInstallRoot(roots,std::move(path),true);
    }
    for (auto arch:{GlassShared::GlassPlatformTask::PlatformArch::X64,GlassShared::GlassPlatformTask::PlatformArch::X86}) {
        std::wstring path=GlassShared::GlassPlatformTask::TryGetAutorunGlassCompatInstallRoot(arch);
        if (!path.empty()) AddInstallRoot(roots,std::move(path),true);
    }
    PWSTR program64=nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX64,KF_FLAG_DEFAULT,nullptr,&program64)) && program64) {
        AddInstallRoot(roots,std::wstring(program64)+L"\\GlassPlatform",false);
        AddInstallRoot(roots,std::wstring(program64)+L"\\Glass",false);
        CoTaskMemFree(program64);
    }
    PWSTR program32=nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86,KF_FLAG_DEFAULT,nullptr,&program32)) && program32) {
        AddInstallRoot(roots,std::wstring(program32)+L"\\GlassPlatform",false);
        AddInstallRoot(roots,std::wstring(program32)+L"\\Glass",false);
        CoTaskMemFree(program32);
    }
    return roots;
}

bool GlassInstallRegistrationExists() {
    if (GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductPlatformX64) ||
        GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductPlatformX86) ||
        GlassInstallMsi::IsProductInstalled(L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5061}") ||
        GlassInstallMsi::IsProductInstalled(L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5062}")) return true;
    std::wstring path;
    return GlassInstallUtil::QueryPlatformInstallPath(true,path) || GlassInstallUtil::QueryPlatformInstallPath(false,path);
}

InstalledGlassVersionInfo FindInstalledGlassVersion() {
    InstalledGlassVersionInfo result;
    const bool registered=GlassInstallRegistrationExists();
    bool registryPathExamined=false;
    for (const auto& root : InstalledGlassRoots()) {
        registryPathExamined=registryPathExamined || root.fromRegistry;
        const std::wstring corePath=root.path+L"\\GlassCore.dll";
        const bool coreExists=GetFileAttributesW(corePath.c_str())!=INVALID_FILE_ATTRIBUTES;
        const bool markerExists=coreExists ||
            GetFileAttributesW((root.path+L"\\GlassCompat.dll").c_str())!=INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW((root.path+L"\\GlassShellHost.exe").c_str())!=INVALID_FILE_ATTRIBUTES;
        if (!coreExists && !root.fromRegistry && !markerExists) continue;
        result.detected=true;
        if (!coreExists) {
            result.hasUnreadableInstallation=true;
            continue;
        }
        if (result.modulePath.empty()) result.modulePath=corePath;
        ULONGLONG key=0;
        const std::wstring version=ReadProductVersionString(corePath,key);
        ParsedVersion parsed{};
        if (version.empty() || !TryParseVersion(version,parsed)) {
            result.hasUnreadableInstallation=true;
            continue;
        }
        if (result.version.empty() || CompareVersions(parsed,result.parsed)>0) {
            result.version=version;
            result.modulePath=corePath;
            result.parsed=parsed;
        }
    }
    if (registered && !registryPathExamined) {
        // A registered installation with a missing/invalid InstallPath is still
        // a discovered product; silently treating it as a fresh install could downgrade it.
        result.detected=true;
        result.hasUnreadableInstallation=true;
    }
    result.valid=result.detected && !result.hasUnreadableInstallation && !result.version.empty();
    return result;
}

bool RegistryKeyExists(HKEY hive, const wchar_t* keyPath, REGSAM view) {
    HKEY key=nullptr;
    const LSTATUS status=RegOpenKeyExW(hive,keyPath,0,KEY_READ|view,&key);
    if (key) RegCloseKey(key);
    return status==ERROR_SUCCESS;
}

bool RegistryDefaultIsExplorerBlurMica(HKEY hive, const wchar_t* keyPath, REGSAM view) {
    HKEY key=nullptr;
    if (RegOpenKeyExW(hive,keyPath,0,KEY_READ|view,&key)!=ERROR_SUCCESS) return false;
    std::wstring value;
    const bool read=ReadRegistryString(key,nullptr,value);
    RegCloseKey(key);
    return read && _wcsicmp(PathFindFileNameW(value.c_str()),L"ExplorerBlurMica.dll")==0;
}

bool HasExplorerBlurMicaRegistration() {
    static constexpr wchar_t extensionKey[] =
        L"Software\\Classes\\Drive\\shellex\\FolderExtensions\\{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}";
    static constexpr wchar_t serverKey[] =
        L"Software\\Classes\\CLSID\\{887D3A6A-502E-4AF5-9CE6-D515E12AFE89}\\InprocServer32";
    const HKEY hives[]={HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE};
    const REGSAM views[]={KEY_WOW64_64KEY,KEY_WOW64_32KEY};
    for (HKEY hive : hives) for (REGSAM view : views) {
        if (RegistryKeyExists(hive,extensionKey,view) ||
            RegistryDefaultIsExplorerBlurMica(hive,serverKey,view)) return true;
    }
    return false;
}

bool HasTranslucentFlyoutsAutorunTask() {
    static constexpr const wchar_t* tasks[]={
        L"TranslucentFlyouts.Win32 Autorun Task",
        L"TranslucentFlyouts.Win32 Autorun Task (x86)"
    };
    static constexpr wchar_t taskTree[]=
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tree\\";
    for (const wchar_t* task : tasks) {
        const std::wstring key=std::wstring(taskTree)+task;
        if (RegistryKeyExists(HKEY_LOCAL_MACHINE,key.c_str(),KEY_WOW64_64KEY) ||
            RegistryKeyExists(HKEY_LOCAL_MACHINE,key.c_str(),KEY_WOW64_32KEY)) return true;
    }
    return false;
}

bool StartsWithInsensitive(const std::wstring& value, const wchar_t* prefix) {
    const size_t length=wcslen(prefix);
    return value.size()>=length && _wcsnicmp(value.c_str(),prefix,length)==0;
}

bool RegistryHasTranslucentTBPackageKeys(HKEY hive, const wchar_t* basePath, REGSAM view) {
    HKEY base=nullptr;
    if (RegOpenKeyExW(hive,basePath,0,KEY_READ|view,&base)!=ERROR_SUCCESS) return false;
    bool found=false;
    for (DWORD index=0; !found; ++index) {
        wchar_t name[512]{};
        DWORD count=static_cast<DWORD>(std::size(name));
        const LSTATUS status=RegEnumKeyExW(base,index,name,&count,nullptr,nullptr,nullptr,nullptr);
        if (status==ERROR_NO_MORE_ITEMS) break;
        if (status==ERROR_MORE_DATA) continue;
        if (status!=ERROR_SUCCESS) break;
        if (StartsWithInsensitive(std::wstring(name,count),L"TranslucentTB_")) found=true;
    }
    RegCloseKey(base);
    return found;
}

bool ContainsInsensitive(const std::wstring& value, const wchar_t* needle) {
    const size_t length=wcslen(needle);
    if (length==0 || value.size()<length) return false;
    for (size_t i=0;i+length<=value.size();++i)
        if (_wcsnicmp(value.c_str()+i,needle,length)==0) return true;
    return false;
}

bool RegistryHasTranslucentTBUninstallEntry(HKEY hive, const wchar_t* basePath, REGSAM view) {
    HKEY base=nullptr;
    if (RegOpenKeyExW(hive,basePath,0,KEY_READ|view,&base)!=ERROR_SUCCESS) return false;
    bool found=false;
    for (DWORD index=0; !found; ++index) {
        wchar_t name[512]{};
        DWORD count=static_cast<DWORD>(std::size(name));
        const LSTATUS status=RegEnumKeyExW(base,index,name,&count,nullptr,nullptr,nullptr,nullptr);
        if (status==ERROR_NO_MORE_ITEMS) break;
        if (status==ERROR_MORE_DATA) continue;
        if (status!=ERROR_SUCCESS) break;
        HKEY app=nullptr;
        if (RegOpenKeyExW(base,name,0,KEY_READ,&app)!=ERROR_SUCCESS) continue;
        std::wstring displayName;
        const bool read=ReadRegistryString(app,L"DisplayName",displayName);
        RegCloseKey(app);
        if (read && ContainsInsensitive(displayName,L"TranslucentTB")) found=true;
    }
    RegCloseKey(base);
    return found;
}

bool IsTranslucentTBProcessRunning() {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snapshot==INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{}; entry.dwSize=sizeof(entry);
    bool found=false;
    if (Process32FirstW(snapshot,&entry)) do {
        if (_wcsicmp(entry.szExeFile,L"TranslucentTB.exe")==0) { found=true; break; }
    } while (Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);
    return found;
}

bool StartupShortcutTargetsTranslucentTB(const std::wstring& shortcut) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link)))) return false;
    ComPtr<IPersistFile> persist;
    if (FAILED(link.As(&persist)) || FAILED(persist->Load(shortcut.c_str(),STGM_READ))) return false;
    wchar_t target[MAX_PATH]{};
    WIN32_FIND_DATAW data{};
    if (FAILED(link->GetPath(target,MAX_PATH,&data,SLGP_RAWPATH))) return false;
    return _wcsicmp(PathFindFileNameW(target),L"TranslucentTB.exe")==0;
}

bool StartupFolderContainsTranslucentTB(REFKNOWNFOLDERID folderId) {
    PWSTR folder=nullptr;
    if (FAILED(SHGetKnownFolderPath(folderId,KF_FLAG_DEFAULT,nullptr,&folder)) || !folder) return false;
    const std::wstring directory(folder);
    const std::wstring pattern=directory+L"\\*.lnk";
    CoTaskMemFree(folder);
    WIN32_FIND_DATAW data{};
    HANDLE find=FindFirstFileW(pattern.c_str(),&data);
    if (find==INVALID_HANDLE_VALUE) return false;
    bool found=false;
    do {
        if (data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring shortcut=directory+L"\\"+data.cFileName;
        if (StartupShortcutTargetsTranslucentTB(shortcut)) { found=true; break; }
    } while (FindNextFileW(find,&data));
    FindClose(find);
    return found;
}

bool HasTranslucentTBInstallation() {
    static constexpr const wchar_t* packageRoots[]={
        L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Families",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\Applications"
    };
    static constexpr const wchar_t* uninstallRoots[]={
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
    };
    const HKEY hives[]={HKEY_CURRENT_USER,HKEY_LOCAL_MACHINE};
    const REGSAM views[]={KEY_WOW64_64KEY,KEY_WOW64_32KEY};
    for (HKEY hive:hives) for (REGSAM view:views) {
        for (const wchar_t* root:packageRoots)
            if (RegistryHasTranslucentTBPackageKeys(hive,root,view)) return true;
        for (const wchar_t* root:uninstallRoots)
            if (RegistryHasTranslucentTBUninstallEntry(hive,root,view)) return true;
    }
    return IsTranslucentTBProcessRunning() ||
           StartupFolderContainsTranslucentTB(FOLDERID_Startup) ||
           StartupFolderContainsTranslucentTB(FOLDERID_CommonStartup);
}

std::vector<std::wstring> DetectConflictingApps() {
    std::vector<std::wstring> conflicts;
    if (HasExplorerBlurMicaRegistration()) {
        conflicts.emplace_back(L"explorerblurmica");
        InstallLog::Line(L"Conflict detected: ExplorerBlurMica shell-extension registration");
    }
    if (HasTranslucentFlyoutsAutorunTask()) {
        conflicts.emplace_back(L"translucentflyouts");
        InstallLog::Line(L"Conflict detected: TranslucentFlyouts scheduled logon task");
    }
    if (HasTranslucentTBInstallation()) {
        conflicts.emplace_back(L"translucenttb");
        InstallLog::Line(L"Conflict detected: TranslucentTB package, uninstall entry, process or startup shortcut");
    }
    return conflicts;
}

std::wstring PlatformVersionFromInstalledModules() {
    const InstalledGlassVersionInfo installed=FindInstalledGlassVersion();
    if (installed.detected) return installed.valid ? installed.version : std::wstring();
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
        if (!gWindowClassBrush) gWindowClassBrush=CreateSolidBrush(kDark);
        if (!gWindowClassBrush) return false;
        wc.hbrBackground = gWindowClassBrush;
        wc.lpszClassName = L"GlassSetupWindow";
        if (!RegisterClassExW(&wc)) {
            if (GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) {
                DeleteObject(gWindowClassBrush);
                gWindowClassBrush=nullptr;
                return false;
            }
        }
        gWindowClassRegistered=true;
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
        if (!gAlreadyRunning && IsWebView2RuntimeInstalled()) PostMessageW(window, kInitWebView, 0, 0);
        return true;
    }
private:
    Page page_ = Page::Welcome;
    UINT dpi_ = 96;
    HWND back_ = nullptr, next_ = nullptr, cancel_ = nullptr;
    HWND updateDownload_ = nullptr, updateSkip_ = nullptr;
    HWND eula_ = nullptr, accept_ = nullptr, progress_ = nullptr, webHost_ = nullptr;
    HWND component64_ = nullptr, component32_ = nullptr, componentSettings_ = nullptr, componentOpenGlass_ = nullptr;
    HFONT font_ = nullptr, heading_ = nullptr, small_ = nullptr, title_ = nullptr;
    HBRUSH darkBrush_ = nullptr, licenseBrush_ = nullptr, buttonBrush_ = nullptr;
    HBRUSH footerBrush_ = nullptr, footerRuleBrush_ = nullptr;
    HBRUSH progressFill_ = nullptr, progressTrack_ = nullptr;
    ComPtr<IStream> bannerStream_;
    std::unique_ptr<Gdiplus::Bitmap> banner_;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> web_;
    bool accepted_ = false, closing_ = false, webReady_ = false, webBridgeReady_ = false;
    bool webInitSent_ = false;
    std::wstring lastWebState_;
    InstallEngine::ComponentSelection selection_{};
    InstallEngine::ComponentSelection installedSelection_{};
    ULONGLONG lastWebPush_=0;
    Page beforeCancel_ = Page::Installing;
    ULONGLONG launchStarted_ = 0;
    std::thread runtimeThread_;
    std::atomic_bool runtimeCancel_{false};
    std::mutex runtimeMutex_;
    std::wstring runtimeStatus_, runtimeError_;
    bool runtimeSuccess_ = false;
    std::wstring cancelledDetail_;
    std::wstring platformVersion_;
    std::wstring eulaText_;
    std::vector<std::wstring> conflicts_;
    std::wstring installedGlassVersion_;
    std::wstring setupProductVersion_;
    std::wstring blockReason_;
    std::wstring downgradeBody_;
    VisualMode visualMode_ = VisualMode::Slideshow;
    std::wstring unlockStatusLine_;
    ULONGLONG unlockVisualUntil_ = 0;
    bool unlockStatusObserved_ = false;
    struct VersionCheckResult {
        bool completed = false;
        bool fetched = false;
        bool newer = false;
        std::wstring latest;
        std::wstring url;
        std::wstring self;
    };
    std::mutex versionCheckMutex_;
    std::thread versionCheckThread_;
    VersionCheckResult versionCheckResult_;
    bool versionCheckStarted_ = false, versionCheckHandled_ = false;
    bool updateAvailable_ = false, updateSkipped_ = false;
    bool offerCleanInstall_ = false;
    bool forceCleanInstall_ = false;
    bool failurePending_ = false;
    bool transitioning_ = false, bufferedErase_ = false;
    ULONGLONG fadeStarted_ = 0;
    Surface frame_, fadeFrame_;
    RECT webBounds_{};
    bool haveWebBounds_ = false;
    HRESULT webError_ = S_OK;
    ULONGLONG started_ = 0;
    int percent_ = 0, progressValue_ = 0, progressTarget_ = 0, displayProgress_ = 0;
    bool progressReverse_ = false;
    double marqueePhase_ = 0;
    ULONGLONG progressTickMs_ = 0;
    bool progressMarquee_ = false;
    bool progressFinishing_ = false;
    bool simulateInstall_ = false;
#ifdef _DEBUG
    bool debugDemoPending_ = false, debugDemoStarted_ = false, debugDemoActive_ = false;
    size_t debugDemoStep_ = 0;
#endif
    std::wstring installStatusLine_;
    std::wstring updateCurrent_;
    std::wstring updateLatest_;
    std::wstring updateUrl_;
    std::wstring updateBody_;
    std::wstring errorReason_;
    std::wstring holdReport_;
    InstallEngine installEngine_;
    std::wstring userDataFolder_;
    EventRegistrationToken requestToken_{}, navigationToken_{}, completeToken_{},
                           popupToken_{}, permissionToken_{}, webMessageToken_{};

    int Scale(int n) const { return MulDiv(n, static_cast<int>(dpi_), 96); }
    RECT Box(int x, int y, int w, int h) const {
        return {Scale(x), Scale(y), Scale(x+w), Scale(y+h)};
    }
    HWND Control(const wchar_t* cls, const std::wstring& text, DWORD style, int id, DWORD exStyle=0) {
        return CreateWindowExW(exStyle, cls, text.c_str(), WS_CHILD | style,
            0,0,0,0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), gInstance, nullptr);
    }
    void AttachButtonSubclasses() {
        for (HWND button : {back_,next_,cancel_,updateDownload_,updateSkip_})
            WizardTheme::AttachButtonScope(button);
    }
    void BuildButtonImageCache() {
        HDC reference=GetDC(window);
        if (!reference) return;
        const std::vector<int> widths={Scale(96),Scale(104),Scale(132),Scale(160),Scale(184),Scale(190),Scale(230)};
        WizardTheme::PrewarmButtonCache(reference,widths,Scale(32));
        ReleaseDC(window,reference);
    }
    void InvalidatePushButtons() {
        for (HWND button:{back_,next_,cancel_,updateDownload_,updateSkip_})
            if (button && IsWindow(button)) InvalidateRect(button,nullptr,FALSE);
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
        for (HWND c : {back_,next_,cancel_,eula_,accept_,updateDownload_,updateSkip_,component64_,component32_,componentSettings_,componentOpenGlass_})
            if (c) SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
    }
    void Initialize() {
        dpi_ = GetDpiForWindow(window);
        platformVersion_ = SelfProductVersion();
        if (platformVersion_.empty())
            platformVersion_ = PlatformVersionFromInstalledModules();
        darkBrush_ = CreateSolidBrush(kDark);
        licenseBrush_ = CreateSolidBrush(kLicensePanel);
        buttonBrush_ = CreateSolidBrush(kButtonBase);
        footerBrush_ = CreateSolidBrush(kFooterTop);
        footerRuleBrush_ = CreateSolidBrush(RGB(74,71,109));
        progressFill_ = CreateSolidBrush(RGB(144,127,230));
        progressTrack_ = CreateSolidBrush(RGB(42,45,67));
        back_ = Control(L"BUTTON",Text(IDS_BACK),BS_PUSHBUTTON | WS_TABSTOP,IDC_BACK);
        next_ = Control(L"BUTTON",Text(IDS_NEXT),BS_PUSHBUTTON | WS_TABSTOP,IDC_NEXT);
        cancel_ = Control(L"BUTTON",Text(IDS_CANCEL),BS_PUSHBUTTON | WS_TABSTOP,IDC_CANCEL);
        updateDownload_ = Control(L"BUTTON",Text(IDS_UPDATE_DOWNLOAD),BS_PUSHBUTTON | WS_TABSTOP,IDC_UPDATE_DOWNLOAD);
        updateSkip_ = Control(L"BUTTON",Text(IDS_UPDATE_SKIP),BS_PUSHBUTTON | WS_TABSTOP,IDC_UPDATE_SKIP);
        eula_ = Control(L"EDIT",L"",WS_BORDER | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                         ES_AUTOVSCROLL | ES_LEFT | WS_VSCROLL,IDC_EULA);
        accept_ = Control(L"BUTTON",Text(IDS_ACCEPT),BS_AUTOCHECKBOX | WS_TABSTOP,IDC_ACCEPT);
        component64_ = Control(L"BUTTON",Text(IDS_COMP_P64_TITLE),BS_AUTOCHECKBOX | WS_TABSTOP,IDC_COMPONENT64);
        component32_ = Control(L"BUTTON",Text(IDS_COMP_P32_TITLE),BS_AUTOCHECKBOX | WS_TABSTOP,IDC_COMPONENT32);
        componentSettings_ = Control(L"BUTTON",Text(IDS_COMP_GS_TITLE),BS_AUTOCHECKBOX | WS_TABSTOP,IDC_COMPONENT_SETTINGS);
        componentOpenGlass_ = Control(L"BUTTON",Text(IDS_COMP_OG_TITLE),BS_AUTOCHECKBOX | WS_TABSTOP,IDC_COMPONENT_OG);
        // Owner-drawn STATIC avoids the classic PROGRESS control's sunken border,
        // which would consume most/all of a 4 px high strip.
        progress_ = Control(L"STATIC",L"",SS_OWNERDRAW,IDC_PROGRESS);
        // A hidden native parent prevents the asynchronously created WebView
        // from ever appearing above Welcome during its first initialization.
        webHost_ = Control(L"STATIC",L"",WS_CLIPCHILDREN | WS_CLIPSIBLINGS,IDC_WEBHOST,WS_EX_CONTROLPARENT);
        SetWindowTheme(eula_,L"",nullptr);
        SendMessageW(window,DM_SETDEFID,IDC_NEXT,0);
        CreateFonts();
        eulaText_ = Utf8(Resource(IDR_EULA,gLanguage));
        eulaText_ += L"\r\n\r\n────────────────────────────────\r\n" +
                     Text(IDS_GPL_APPENDIX) + L"\r\n\r\n" + Utf8(Resource(IDR_GPL));
        SendMessageW(eula_,EM_SETLIMITTEXT,1024*1024,0);
        SetWindowTextW(eula_,eulaText_.c_str());
        SendMessageW(eula_,EM_SETMARGINS,EC_LEFTMARGIN | EC_RIGHTMARGIN,MAKELPARAM(14,14));
        const auto image = Resource(IDR_WELCOME);
        bannerStream_.Attach(SHCreateMemStream(image.data,image.size));
        if (bannerStream_) {
            banner_.reset(Gdiplus::Bitmap::FromStream(bannerStream_.Get()));
            if (banner_ && banner_->GetLastStatus() != Gdiplus::Ok) banner_.reset();
        }
        const bool os64=GlassInstallUtil::IsOs64Bit();
        installedSelection_.platform64=GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductPlatformX64);
        installedSelection_.platform32=GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductPlatformX86);
        installedSelection_.glassSettings=GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSettingsX64) ||
            GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductSettingsX86);
        installedSelection_.openGlass=GlassInstallMsi::IsProductInstalled(GlassInstallMsi::kProductOpenGlass);
        const bool anyInstalled=installedSelection_.platform64 || installedSelection_.platform32 ||
            installedSelection_.glassSettings || installedSelection_.openGlass;
        selection_.platform64=os64;
        selection_.platform32=!os64 || !anyInstalled || installedSelection_.platform32;
        selection_.glassSettings=!anyInstalled || installedSelection_.glassSettings;
        selection_.openGlass=os64 && (!anyInstalled || installedSelection_.openGlass);
#ifdef _DEBUG
        debugDemoPending_=HasCommandLineSwitch(L"--demo-tour");
#endif
        Layout();
        AttachButtonSubclasses();
        BuildButtonImageCache();
        SetPage(gAlreadyRunning ? Page::AlreadyRunning :
            (IsWebView2RuntimeInstalled() ? Page::Welcome : Page::RuntimeRequired));
    }
    void Layout() {
        Move(back_,644,kContentHeight+12,96,32);
        // Complete uses the exact rectangle previously occupied by Cancel.
        const bool unlock = page_==Page::ProcessesStuck;
        const bool conflictCheck = page_==Page::Conflicts;
        const bool cleanOffer = page_==Page::VersionInvalid && offerCleanInstall_;
        // «Продолжить» в Segoe UI 14px — 88 px; 132 px даёт поля по 22 px.
        const bool confirm=page_==Page::ConfirmCancel;
        const bool terminal=page_==Page::Complete || page_==Page::Cancelled || page_==Page::Error ||
            page_==Page::RuntimeError || page_==Page::AlreadyRunning;
        if (confirm) Move(back_,568,kContentHeight+12,190,32);
        const int nextWidth=confirm?190:page_==Page::RuntimeRequired?230:(unlock || conflictCheck)?160:cleanOffer?132:104;
        const int nextX=confirm?778:page_==Page::RuntimeRequired?624:terminal?864:
            (unlock || conflictCheck)?694:cleanOffer?722:750;
        Move(next_,nextX,kContentHeight+12,nextWidth,32);
        Move(cancel_,864,kContentHeight+12,104,32);
        Move(updateDownload_,644,kContentHeight+12,104,32);
        Move(updateSkip_,750,kContentHeight+12,104,32);
        Move(eula_,48,142,904,285);
        Move(accept_,48,447,904,32);
        Move(progress_,0,kHtmlHeight,kWidth,kProgressHeight);
        Move(webHost_,0,0,kWidth,kHtmlHeight);
        Move(component64_,60,185,400,32); Move(component32_,510,185,400,32);
        Move(componentSettings_,60,275,400,32); Move(componentOpenGlass_,510,275,400,32);
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
        const bool webVisible = SlidesVisible();
        const bool nativeLicense = !webVisible && page_==Page::License && !transitioning_;
        ShowWindow(eula_,nativeLicense?SW_SHOWNA:SW_HIDE);
        ShowWindow(accept_,nativeLicense?SW_SHOWNA:SW_HIDE);
        const bool showProgress = !transitioning_ && (page_==Page::Installing || page_==Page::Cancelling || page_==Page::Launching || page_==Page::RuntimeInstalling || page_==Page::Complete);
        ShowWindow(progress_, showProgress ? SW_SHOWNA : SW_HIDE);
        const bool update = page_==Page::Update && !transitioning_;
        ShowWindow(updateDownload_,update?SW_SHOWNA:SW_HIDE);
        ShowWindow(updateSkip_,update?SW_SHOWNA:SW_HIDE);
        if (controller_) controller_->put_IsVisible(webVisible ? TRUE : FALSE);
        ShowWindow(webHost_,webVisible?SW_SHOWNA:SW_HIDE);
        const bool nativeComponents=!webVisible && page_==Page::Components && !transitioning_;
        const bool os64=GlassInstallUtil::IsOs64Bit();
        ShowWindow(component64_,nativeComponents && os64?SW_SHOWNA:SW_HIDE);
        ShowWindow(component32_,nativeComponents?SW_SHOWNA:SW_HIDE);
        ShowWindow(componentSettings_,nativeComponents?SW_SHOWNA:SW_HIDE);
        ShowWindow(componentOpenGlass_,nativeComponents && os64?SW_SHOWNA:SW_HIDE);
        if (nativeComponents) {
            SendMessageW(component64_,BM_SETCHECK,selection_.platform64?BST_CHECKED:BST_UNCHECKED,0);
            SendMessageW(component32_,BM_SETCHECK,selection_.platform32?BST_CHECKED:BST_UNCHECKED,0);
            SendMessageW(componentSettings_,BM_SETCHECK,selection_.glassSettings?BST_CHECKED:BST_UNCHECKED,0);
            SendMessageW(componentOpenGlass_,BM_SETCHECK,selection_.openGlass?BST_CHECKED:BST_UNCHECKED,0);
            EnableWindow(component64_,FALSE); EnableWindow(component32_,os64?TRUE:FALSE);
        }
    }
    [[nodiscard]] bool SlidesVisible() const noexcept {
        // One WebView2 child stays visible for every HTML page. Page changes are
        // DOM state changes, never child-window hide/show or document navigation.
        return webReady_;
    }
    [[nodiscard]] bool DebugDemoLocksControls() const noexcept {
#ifdef _DEBUG
        return debugDemoActive_ || (debugDemoPending_ && page_==Page::Welcome);
#else
        return false;
#endif
    }
    [[nodiscard]] const wchar_t* WebPageName() const noexcept {
        switch (page_) {
        case Page::Welcome: return L"welcome";
        case Page::Checking: return L"checking";
        case Page::Update: return L"update";
        case Page::Conflicts: return L"conflicts";
        case Page::Downgrade: return L"downgrade";
        case Page::VersionInvalid: return L"version-invalid";
        case Page::License: return L"license";
        case Page::Components: return L"components";
        case Page::ConfirmCancel: return L"confirm-cancel";
        case Page::Cancelling: return L"cancelling";
        case Page::Cancelled: return L"cancelled";
        case Page::Launching: return L"launching";
        case Page::Installing:
            if (visualMode_==VisualMode::UnlockModules) return L"unlock-modules";
            if (visualMode_==VisualMode::UnlockProcesses) return L"unlock-processes";
            return L"installing";
        case Page::ModulesBusy: return L"unlock-modules";
        case Page::ProcessesStuck: return L"unlock-processes";
        case Page::Complete: return L"complete";
        case Page::Error: return L"error";
        case Page::RuntimeRequired: case Page::RuntimeInstalling: case Page::RuntimeError: case Page::AlreadyRunning: return L"loading";
        }
        return L"welcome";
    }
    std::wstring ConflictsJson() const {
        std::wstring json=L"[";
        bool first=true;
        for (const auto& id:conflicts_) {
            UINT nameId=0, detailId=0;
            if (id==L"explorerblurmica") {
                nameId=IDS_CONFLICT_EXPLORER_NAME;
                detailId=IDS_CONFLICT_EXPLORER_DETAIL;
            } else if (id==L"translucentflyouts") {
                nameId=IDS_CONFLICT_FLYOUTS_NAME;
                detailId=IDS_CONFLICT_FLYOUTS_DETAIL;
            } else if (id==L"translucenttb") {
                nameId=IDS_CONFLICT_TB_NAME;
                detailId=IDS_CONFLICT_TB_DETAIL;
            }
            if (!nameId) continue;
            if (!first) json+=L",";
            first=false;
            json+=L"{\"name\":"+JsStringLiteral(Text(nameId))+
                  L",\"detail\":"+JsStringLiteral(Text(detailId))+L"}";
        }
        json+=L"]";
        return json;
    }
    std::wstring ComponentsJson() const {
        const bool os64=GlassInstallUtil::IsOs64Bit();
        std::wstring json=L"[";
        auto item=[&](const wchar_t* id, UINT title, UINT desc, bool checked, bool locked, bool hidden, bool installed) {
            if (json.size()>1) json+=L",";
            const std::wstring badge=locked ? Text(IDS_COMP_MANDATORY) :
                installed ? (PRIMARYLANGID(gLanguage)==LANG_RUSSIAN ? L"УСТАНОВЛЕНО" : L"INSTALLED") : L"";
            json+=L"{\"id\":"+JsStringLiteral(id)+L",\"title\":"+JsStringLiteral(Text(title))+
                L",\"description\":"+JsStringLiteral(Text(desc))+L",\"badge\":"+JsStringLiteral(badge)+
                L",\"checked\":"+(checked?L"true":L"false")+L",\"locked\":"+(locked?L"true":L"false")+
                L",\"hidden\":"+(hidden?L"true":L"false")+L"}";
        };
        item(L"x64",IDS_COMP_P64_TITLE,IDS_COMP_P64_DESC,selection_.platform64,os64,!os64,installedSelection_.platform64);
        item(L"x86",IDS_COMP_P32_TITLE,IDS_COMP_P32_DESC,selection_.platform32,!os64,false,installedSelection_.platform32);
        item(L"settings",IDS_COMP_GS_TITLE,IDS_COMP_GS_DESC,selection_.glassSettings,false,false,installedSelection_.glassSettings);
        item(L"openglass",IDS_COMP_OG_TITLE,IDS_COMP_OG_DESC,selection_.openGlass,false,!os64,installedSelection_.openGlass);
        return json+L"]";
    }
    std::wstring LaunchCardsJson() const {
        const bool ru=PRIMARYLANGID(gLanguage)==LANG_RUSSIAN;
        std::wstring json=L"[";
        auto add=[&](const wchar_t* title,int threshold) {
            if (json.size()>1) json+=L",";
            const bool ready=progressValue_>=threshold;
            const wchar_t* label=ready?(ru?L"ГОТОВО":L"READY"):(ru?L"ЗАПУСК":L"STARTING");
            json+=L"{\"title\":"+JsStringLiteral(title)+L",\"sub\":"+JsStringLiteral(label)+
                L",\"ready\":"+(ready?L"true":L"false")+L"}";
        };
        if (selection_.platform64) add(L"GLASS x64",9500);
        if (selection_.platform32) add(L"GLASS x86",9750);
        if (selection_.openGlass) add(L"OPENGLASS",9900);
        else if (selection_.glassSettings) add(L"SETTINGS",9900);
        return json+L"]";
    }
    void SyncWebViewState() {
        if (!web_ || !webReady_ || !webBridgeReady_) return;
        if (!webInitSent_) {
#ifdef GLASS_EMBED_MSI
            const UINT doneBodyId=IDS_DONE_BODY, doneNoteId=IDS_DONE_NOTE;
#else
            const UINT doneBodyId=IDS_DONE_BODY_DEMO, doneNoteId=IDS_DONE_NOTE_DEMO;
#endif
            std::wstring labels=L"{\"brand\":"+JsStringLiteral(Text(IDS_BRAND))+
                L",\"licenseTitle\":"+JsStringLiteral(Text(IDS_EULA_TITLE))+
                L",\"licenseIntro\":"+JsStringLiteral(Text(IDS_EULA_BODY))+
                L",\"licenseAccept\":"+JsStringLiteral(Text(IDS_ACCEPT))+
                L",\"updateTag\":"+JsStringLiteral(Text(IDS_UPDATE_TAG))+
                L",\"updateTitle\":"+JsStringLiteral(Text(IDS_UPDATE_TITLE))+
                L",\"updateReady\":"+JsStringLiteral(Text(IDS_UPDATE_READY))+
                L",\"updateCurrentLabel\":"+JsStringLiteral(Text(IDS_UPDATE_CURRENT))+
                L",\"updateLatestLabel\":"+JsStringLiteral(Text(IDS_UPDATE_LATEST))+
                L",\"conflictsTag\":"+JsStringLiteral(Text(IDS_CONFLICTS_TAG))+
                L",\"conflictsTitle\":"+JsStringLiteral(Text(IDS_CONFLICTS_TITLE))+
                L",\"conflictsBody\":"+JsStringLiteral(Text(IDS_CONFLICTS_BODY))+
                L",\"conflictsInstructions\":"+JsStringLiteral(Text(IDS_CONFLICTS_INSTRUCTIONS))+
                L",\"downgradeTag\":"+JsStringLiteral(Text(IDS_DOWNGRADE_TAG))+
                L",\"downgradeTitle\":"+JsStringLiteral(Text(IDS_DOWNGRADE_TITLE))+
                L",\"versionInvalidTag\":"+JsStringLiteral(Text(IDS_VERSION_INVALID_TAG))+
                L",\"versionInvalidTitle\":"+JsStringLiteral(Text(IDS_VERSION_INVALID_TITLE))+
                L",\"downgradeNote\":"+JsStringLiteral(Text(IDS_DOWNGRADE_NOTE))+
                L",\"versionLabel\":"+JsStringLiteral(Text(IDS_INSTALLED_VERSION_LABEL))+
                L",\"setupVersionLabel\":"+JsStringLiteral(Text(IDS_SETUP_VERSION_LABEL))+
                L",\"doneTag\":"+JsStringLiteral(Text(IDS_DONE_TAG))+
                L",\"doneTitle\":"+JsStringLiteral(Text(IDS_DONE_TITLE))+
                L",\"doneBody\":"+JsStringLiteral(Text(doneBodyId))+
                L",\"doneNote\":"+JsStringLiteral(Text(doneNoteId))+
                L",\"doneReady\":"+JsStringLiteral(Text(IDS_DONE_READY))+
                L",\"errorTag\":"+JsStringLiteral(Text(IDS_ERROR_TAG))+
                L",\"errorTitle\":"+JsStringLiteral(Text(IDS_ERROR_TITLE))+
                L",\"errorBody\":"+JsStringLiteral(Text(IDS_ERROR_BODY))+
                L",\"errorDetails\":"+JsStringLiteral(Text(IDS_ERROR_DETAILS))+
                L",\"errorNote\":"+JsStringLiteral(Text(IDS_ERROR_LOG_HINT))+
                L",\"compTag\":"+JsStringLiteral(Text(IDS_COMP_TAG))+
                L",\"compTitle\":"+JsStringLiteral(Text(IDS_COMP_TITLE))+
                L",\"compIntro\":"+JsStringLiteral(Text(IDS_COMP_INTRO))+
                L",\"confirmTag\":"+JsStringLiteral(Text(IDS_CONFIRM_CANCEL_TAG))+
                L",\"confirmTitle\":"+JsStringLiteral(Text(IDS_CONFIRM_CANCEL_TITLE))+
                L",\"confirmBody\":"+JsStringLiteral(Text(IDS_CONFIRM_CANCEL_BODY))+
                L",\"confirmNote\":"+JsStringLiteral(Text(IDS_CONFIRM_CANCEL_NOTE))+
                L",\"cancelledTag\":"+JsStringLiteral(Text(IDS_CANCELLED_TAG))+
                L",\"cancelledTitle\":"+JsStringLiteral(Text(IDS_CANCELLED_TITLE))+
                L",\"cancelledBody\":"+JsStringLiteral(Text(IDS_CANCELLED_BODY))+
                L",\"launchTag\":"+JsStringLiteral(Text(IDS_LAUNCH_TAG))+
                L",\"launchTitle\":"+JsStringLiteral(Text(IDS_LAUNCH_TITLE))+
                L",\"launchBody\":"+JsStringLiteral(Text(IDS_LAUNCH_BODY))+
                L",\"launchSubstatus\":"+JsStringLiteral(Text(IDS_LAUNCH_SUBSTATUS))+L"}";
            const std::wstring init=L"{\"type\":\"init\",\"locale\":"+
                JsStringLiteral(PRIMARYLANGID(gLanguage)==LANG_RUSSIAN?L"ru":L"en")+
                L",\"eula\":"+JsStringLiteral(eulaText_)+L",\"ui\":"+labels+L"}";
            if (FAILED(web_->PostWebMessageAsJson(init.c_str()))) return;
            webInitSent_=true;
        }
        std::wstring json=L"{\"type\":\"state\",\"page\":"+JsStringLiteral(WebPageName())+
            L",\"accepted\":"+(accepted_?L"true":L"false")+
            L",\"platformVersion\":"+JsStringLiteral(platformVersion_)+
            L",\"updateCurrent\":"+JsStringLiteral(updateCurrent_)+
            L",\"updateLatest\":"+JsStringLiteral(updateLatest_)+
            L",\"updateBody\":"+JsStringLiteral(updateBody_)+
            L",\"conflicts\":"+ConflictsJson()+
            L",\"installedGlassVersion\":"+JsStringLiteral(installedGlassVersion_)+
            L",\"setupProductVersion\":"+JsStringLiteral(setupProductVersion_)+
            L",\"versionInvalidNote\":"+JsStringLiteral(Text(offerCleanInstall_?IDS_VERSION_INVALID_NOTE:IDS_SETUP_VERSION_INVALID_NOTE))+
            L",\"blockReason\":"+JsStringLiteral(blockReason_)+
            L",\"downgradeBody\":"+JsStringLiteral(downgradeBody_)+
            L",\"errorReason\":"+JsStringLiteral(errorReason_)+
            L",\"cancelledDetail\":"+JsStringLiteral(cancelledDetail_)+
            L",\"status\":"+JsStringLiteral(installStatusLine_)+
            L",\"percent\":"+std::to_wstring(percent_)+
            L",\"progress\":"+std::to_wstring(progressValue_)+
            L",\"marquee\":"+(progressMarquee_?L"true":L"false")+
            L",\"unlockActive\":"+((page_==Page::Installing && visualMode_!=VisualMode::Slideshow)?L"true":L"false")+
            L",\"holdReport\":"+JsStringLiteral(holdReport_)+
            L",\"components\":"+ComponentsJson()+
            L",\"componentsWarning\":"+JsStringLiteral(selection_.glassSettings?L"":Text(IDS_COMP_GS_WARNING))+
            L",\"launchCards\":"+LaunchCardsJson()+L"}";
        if (json==lastWebState_) return;
        const HRESULT hr=web_->PostWebMessageAsJson(json.c_str());
        if (SUCCEEDED(hr)) lastWebState_=std::move(json);
        else InstallLog::LineFormat(L"WebView2 state message failed: 0x%08X",static_cast<unsigned>(hr));
    }
    void FocusPage() {
        if (page_==Page::ConfirmCancel) { SetFocus(back_); return; }
        if (webReady_ && (page_==Page::License || page_==Page::Components)) { SetFocus(webHost_); return; }
        SetFocus(page_==Page::ModulesBusy || page_==Page::ProcessesStuck || page_==Page::Conflicts ? next_ :
                 page_==Page::Downgrade || (page_==Page::VersionInvalid && !offerCleanInstall_) ? cancel_ :
                 page_==Page::Update ? updateSkip_ :
                 page_==Page::Checking ? cancel_ :
                 page_==Page::License && !accepted_ ? accept_ :
                 page_==Page::Installing || page_==Page::RuntimeInstalling ? cancel_ : next_);
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
        EnableWindow(next_,!failurePending_ && (page_!=Page::License || accepted_));
        FocusPage();
        InvalidateContent();
    }
    void SetPage(Page page) {
        const Page oldPage=page_;
        if (transitioning_) EndTransition();
        page_=page;
        BOOL animationsEnabled=TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animationsEnabled,0);
        const bool nativeLoading=page_==Page::Welcome && !webReady_ && SUCCEEDED(webError_);
        if (animationsEnabled && !webReady_ &&
            (nativeLoading || page_==Page::Update || page_==Page::Error || page_==Page::RuntimeInstalling))
            SetTimer(window,kHeroTimer,kHeroTimerMs,nullptr);
        else KillTimer(window,kHeroTimer);
        WizardTheme::ApplyDarkWindowChrome(window);
        const bool closeBlocked=page_==Page::Cancelling || page_==Page::Launching ||
            (page_==Page::Error && failurePending_);
        WizardTheme::SetSystemCloseEnabled(window,!closeBlocked);
        ShowWindow(eula_,SW_HIDE); ShowWindow(accept_,SW_HIDE);
        Layout();
        const bool cleanOffer=page_==Page::VersionInvalid && offerCleanInstall_;
        const bool blockedPage=page_==Page::Downgrade || (page_==Page::VersionInvalid && !offerCleanInstall_);
        const bool demoControlsLocked=DebugDemoLocksControls();
        const bool terminal=page_==Page::Complete || page_==Page::Cancelled || page_==Page::Error ||
            page_==Page::RuntimeError || page_==Page::AlreadyRunning;
        const bool inProgress=page_==Page::Checking || page_==Page::Installing || page_==Page::Cancelling ||
            page_==Page::Launching || page_==Page::RuntimeInstalling || page_==Page::Update;
        const UINT nextId = page_==Page::RuntimeRequired?IDS_RUNTIME_INSTALL:
            page_==Page::ConfirmCancel?IDS_CONFIRM_ABORT:
            page_==Page::Conflicts?IDS_RECHECK_CONFLICTS:
            cleanOffer?IDS_CONTINUE:
            page_==Page::Components?IDS_INSTALL:
            terminal?IDS_FINISH:
            page_==Page::ModulesBusy?IDS_HOLD_RETRY:
            page_==Page::ProcessesStuck?IDS_UNLOCK:IDS_NEXT;
        SetWindowTextW(next_,Text(nextId).c_str());
        SetWindowTextW(back_,Text(page_==Page::ConfirmCancel?IDS_CONFIRM_RESUME:IDS_BACK).c_str());
        SetWindowTextW(cancel_,Text(blockedPage?IDS_CLOSE:IDS_CANCEL).c_str());
        const bool confirm=page_==Page::ConfirmCancel;
        const bool backVisible=page_==Page::License || page_==Page::Components || confirm;
        const bool nextVisible=!inProgress && !blockedPage;
        const bool cancelVisible=!terminal && !confirm && page_!=Page::Cancelling && page_!=Page::Launching;
        const bool demoLocked=demoControlsLocked && !terminal;
        ShowWindow(back_,backVisible?SW_SHOWNA:SW_HIDE);
        ShowWindow(next_,nextVisible?SW_SHOWNA:SW_HIDE);
        ShowWindow(cancel_,cancelVisible && !demoLocked?SW_SHOWNA:SW_HIDE);
        EnableWindow(next_,!failurePending_ && !demoLocked && (page_!=Page::License || accepted_));
        EnableWindow(back_,!demoLocked);
        EnableWindow(cancel_,!demoLocked && !(page_==Page::RuntimeInstalling && runtimeCancel_.load()));
        EnableWindow(updateDownload_,!demoLocked);
        EnableWindow(updateSkip_,!demoLocked);
        const int defaultId=confirm?IDC_BACK:page_==Page::Update?IDC_UPDATE_SKIP:blockedPage?IDC_CANCEL:IDC_NEXT;
        SendMessageW(window,DM_SETDEFID,defaultId,0);
        if (!webReady_ && ((oldPage==Page::Welcome && (page_==Page::Checking || page_==Page::License)) ||
            (oldPage==Page::Checking && (page_==Page::Update || page_==Page::License)))) BeginTransition();
        ShowContentControls();
        if (!transitioning_) FocusPage();
        SyncWebViewState();
        InvalidateRect(window,nullptr,FALSE);
    }
    static bool DefaultProgramGlassDir(bool x64, std::wstring& out) {
        if (x64) {
            PWSTR path=nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX64,KF_FLAG_DEFAULT,nullptr,&path)) || !path) return false;
            out.assign(path);
            CoTaskMemFree(path);
        } else {
            PWSTR path = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, KF_FLAG_DEFAULT, nullptr, &path)) || !path) {
                if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &path)) || !path)
                    return false;
            }
            out.assign(path);
            CoTaskMemFree(path);
        }
        out += L"\\GlassPlatform";
        return true;
    }
    void StartRuntimeInstallation() {
        if (runtimeThread_.joinable() || page_!=Page::RuntimeRequired) return;
        runtimeCancel_.store(false);
        runtimeSuccess_=false;
        runtimeError_.clear();
        runtimeStatus_=Text(IDS_RUNTIME_DOWNLOADING);
        progressMarquee_=true;
        SetPage(Page::RuntimeInstalling);
        const HWND target=window;
        try {
            runtimeThread_=std::thread([this,target]() {
                std::wstring directory,installer,error;
                DWORD exitCode=0;
                bool success=false;
                auto status=[target](UINT id) { PostMessageW(target,kRuntimeStatus,id,0); };
                try {
                    if (CreateWebView2TempPath(directory,installer,error) &&
                        DownloadWebView2Bootstrapper(installer,error,runtimeCancel_) &&
                        !runtimeCancel_.load() && VerifyMicrosoftSignature(installer,error)) {
                        status(IDS_RUNTIME_INSTALLING_PACKAGE);
                        if (RunWebView2Bootstrapper(installer,directory,exitCode,error,runtimeCancel_) &&
                            !runtimeCancel_.load()) {
                            status(IDS_RUNTIME_VERIFYING);
                            success=IsWebView2RuntimeInstalled();
                            if (!success) error=L"WebView2 Runtime was not detected after installer exit "+std::to_wstring(exitCode);
                        }
                    }
                } catch (...) { error=L"Unexpected error while installing WebView2 Runtime"; }
                RemoveWebView2TempFiles(directory,installer);
                {
                    std::lock_guard<std::mutex> lock(runtimeMutex_);
                    runtimeError_=std::move(error);
                    runtimeSuccess_=success;
                }
                PostMessageW(target,kRuntimeComplete,0,0);
            });
        } catch (...) {
            runtimeError_=L"Could not create the WebView2 installation worker";
            SetPage(Page::RuntimeError);
        }
    }
    void CompleteRuntimeInstallation() {
        if (runtimeThread_.joinable()) runtimeThread_.join();
        if (runtimeCancel_.load()) { DestroyWindow(window); return; }
        bool success=false;
        {
            std::lock_guard<std::mutex> lock(runtimeMutex_);
            success=runtimeSuccess_;
        }
        progressMarquee_=false;
        if (!success) {
            InstallLog::LineFormat(L"WebView2 setup failed: %s",runtimeError_.c_str());
            if (runtimeError_.empty()) runtimeError_=Text(IDS_RUNTIME_INSTALL_FAILED);
            SetPage(Page::RuntimeError);
            return;
        }
        InstallLog::Line(L"WebView2 Runtime installed");
        SetPage(Page::Welcome);
        InitializeWebView();
    }
    void ShowInstallError(const std::wstring& reason) noexcept {
        errorReason_ = reason;
        KillTimer(window, kInstallTimer);
        SetPage(Page::Error);
    }
    std::wstring DescribeFailure() const noexcept {
        const unsigned code = installEngine_.MsiExitCode();
        const std::wstring msi = installEngine_.MsiMessage();
        const std::wstring detail = installEngine_.FailureDetail();
        const std::wstring note = installEngine_.RollbackNote();
        UINT id = 0;
        switch (code) {
        case 112: id = IDS_MSI_DISK_FULL; break;
        case 1602: id = IDS_MSI_USER_CANCEL; break;
        case 1603: id = IDS_MSI_FATAL; break;
        case 1618: id = IDS_MSI_BUSY; break;
        case 1619: id = IDS_MSI_OPEN_FAILED; break;
        case 1620: id = IDS_MSI_INVALID; break;
        case 1625: id = IDS_MSI_POLICY; break;
        case 1632: id = IDS_MSI_TEMP; break;
        case 1638: id = IDS_MSI_VERSION; break;
        case 1639: id = IDS_MSI_CMDLINE; break;
        default: break;
        }
        const bool mapped = id != 0;
        std::wstring text;
        if (mapped)
            text = Text(id);
        else if (!msi.empty())
            text = msi;
        else if (!detail.empty())
            text = detail;
        else
            text = installEngine_.ErrorText();
        if ((mapped || !msi.empty() || !detail.empty()) && !note.empty()) {
            if (!text.empty()) text += L"\r\n\r\n";
            text += note;
        }
        return text;
    }
    void ShowFailurePage(bool finished) noexcept {
        const std::wstring detail = DescribeFailure();
        std::wstring reason = Text(IDS_ERR_INSTALL_FAILED);
        if (!detail.empty()) {
            reason += L"\r\n\r\n";
            reason += detail;
        }
        errorReason_ = reason;
        failurePending_ = !finished;
        if (finished) {
            InstallLog::LineFormat(L"Installation failed: %s",
                detail.empty() ? L"(no detail)" : detail.c_str());
            installStatusLine_.clear();
            KillTimer(window, kInstallTimer);
        } else {
            InstallLog::Line(L"Error page shown before rollback");
        }
        if (page_ != Page::Error) SetPage(Page::Error);
        else SyncWebViewState();
        WizardTheme::SetSystemCloseEnabled(window,finished);
        if (next_) EnableWindow(next_, finished ? TRUE : FALSE);
    }
    void StartInstallation() {
        if (!accepted_ || transitioning_) return;
        if (!RunPreflightChecks()) return;
        started_ = GetTickCount64();
        percent_ = 0;
        progressValue_ = 0;
        progressTarget_ = 0;
        displayProgress_ = 0;
        progressFinishing_ = false;
        progressTickMs_ = GetTickCount64();
        visualMode_ = VisualMode::Slideshow;
        unlockStatusLine_.clear();
        unlockVisualUntil_ = 0;
        unlockStatusObserved_ = false;
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
        const bool os64=GlassInstallUtil::IsOs64Bit();
        const auto resolveRoot=[&](bool x64,std::wstring& out) {
            if (GlassInstallUtil::QueryPlatformInstallPath(x64,out)) return true;
            out=GlassShared::GlassPlatformTask::TryGetAutorunGlassCompatInstallRoot(
                x64?GlassShared::GlassPlatformTask::PlatformArch::X64:GlassShared::GlassPlatformTask::PlatformArch::X86);
            if (!out.empty()) return true;
            if (!DefaultProgramGlassDir(x64,out)) return false;
            const wchar_t* code=x64?GlassInstallMsi::kProductPlatformX64:GlassInstallMsi::kProductPlatformX86;
            if (GlassInstallMsi::IsProductInstalled(code) && GetFileAttributesW((out+L"\\GlassCompat.dll").c_str())==INVALID_FILE_ATTRIBUTES) {
                InstallLog::Line(L"Refusing to upgrade a registered product at an unverified guessed path");
                return false;
            }
            return true;
        };
        if ((!os64 || resolveRoot(true,x64Dir)) && resolveRoot(false,x86Dir)) {
            // Resolved existing install roots or their default Program Files locations.
        } else {
            InstallLog::Line(L"Install path resolution failed (Program Files)");
            ShowInstallError(Text(IDS_ERR_INSTALL_PATH));
            return;
        }
        if (os64 && !GlassInstallUtil::QueryOpenGlassInstallPath(ogDir)) ogDir=x64Dir+L"\\OpenGlass";
        InstallLog::Line(L"User started installation");
        InstallLog::LineFormat(L"Target x64=%s x86=%s OpenGlass=%s", x64Dir.c_str(), x86Dir.c_str(), ogDir.c_str());
        InstallEngine::PhaseLabels labels;
        labels.stop64 = Text(IDS_STATUS_STOP64);
        labels.stop86 = Text(IDS_STATUS_STOP86);
        labels.stopOg = Text(IDS_STATUS_STOPOG);
        labels.unload = Text(IDS_STATUS_UNLOAD);
        labels.forceUnload = Text(IDS_STATUS_FORCE_UNLOAD);
        labels.unlockProcesses = Text(IDS_STATUS_UNLOCK_PROCESSES);
        labels.start64 = Text(IDS_STATUS_START64);
        labels.start86 = Text(IDS_STATUS_START86);
        labels.startOg = Text(IDS_STATUS_STARTOG);
        labels.extract = Text(IDS_STATUS_EXTRACT);
        labels.platform64 = Text(IDS_STATUS_X64);
        labels.platform32 = Text(IDS_STATUS_X86);
        labels.openGlass = Text(IDS_STATUS_OG);
        labels.finalize = Text(IDS_STATUS_FINAL);
        labels.backup = Text(IDS_STATUS_BACKUP);
        labels.glassSettings64 = Text(IDS_STATUS_GS64);
        labels.glassSettings86 = Text(IDS_STATUS_GS86);
        labels.rollback = Text(IDS_STATUS_ROLLBACK);
        labels.purge = Text(IDS_STATUS_PURGE);
        labels.purgeFailed = Text(IDS_ERR_PURGE_FAILED);
        labels.platformDidNotStart = Text(IDS_ERR_PLATFORM_START);
        labels.shellRegisterFailed = Text(IDS_ERR_SHELL_REGISTER);
        labels.rollbackFailed = Text(IDS_ERR_ROLLBACK_FAILED);
        labels.rollbackRestored = Text(IDS_ERR_ROLLBACK_RESTORED);
        labels.rollbackRemoved = Text(IDS_ERR_ROLLBACK_REMOVED);
        labels.nativeScanFailed = Text(IDS_ERR_NATIVE_SCAN);
        labels.snapshotFailed = Text(IDS_ERR_SNAPSHOT);
        labels.diskFull = Text(IDS_MSI_DISK_FULL);
        installEngine_.Start(gInstance, x64Dir, x86Dir, ogDir, selection_, forceCleanInstall_, std::move(labels), nullptr);
        SetTimer(window, kInstallTimer, kInstallTimerMs, nullptr);
#endif
    }
    bool RunPreflightChecks() {
        conflicts_=DetectConflictingApps();
        if (!conflicts_.empty()) {
            blockReason_.clear();
            downgradeBody_.clear();
            InstallLog::Line(L"Preflight blocked: stop or remove conflicting applications, then recheck");
            SetPage(Page::Conflicts);
            return false;
        }
        const InstalledGlassVersionInfo installed=FindInstalledGlassVersion();
        installedGlassVersion_=installed.version;
        setupProductVersion_=SelfProductVersion();
        ParsedVersion setupParsed{};
        const bool setupVersionValid=TryParseVersion(setupProductVersion_,setupParsed);
        if (installed.detected && !installed.valid) {
            if (!forceCleanInstall_) {
                offerCleanInstall_=true;
                blockReason_=Text(IDS_VERSION_INVALID_BODY);
                downgradeBody_.clear();
                InstallLog::LineFormat(L"Preflight: integrity failure, offering clean reinstall (%s)",
                    installed.modulePath.empty()?L"installation path not resolved":installed.modulePath.c_str());
                SetPage(Page::VersionInvalid);
                return false;
            }
            InstallLog::Line(L"Preflight: clean reinstall accepted after integrity failure");
        } else {
            offerCleanInstall_=false;
        }
        if (installed.detected && !setupVersionValid) {
            offerCleanInstall_=false;
            blockReason_=Text(IDS_SETUP_VERSION_INVALID_BODY);
            downgradeBody_.clear();
            InstallLog::Line(L"Preflight blocked: this setup's ProductVersion is missing or invalid; safe comparison is impossible");
            SetPage(Page::VersionInvalid);
            return false;
        }
        if (installed.detected && CompareVersions(installed.parsed,setupParsed)>0) {
            downgradeBody_=FormatVersionPairText(IDS_DOWNGRADE_BODY,
                setupProductVersion_,installed.version);
            blockReason_.clear();
            InstallLog::LineFormat(L"Preflight blocked: refusing downgrade from GlassPlatform %s to Setup %s",
                installed.version.c_str(),setupProductVersion_.c_str());
            SetPage(Page::Downgrade);
            return false;
        }
        blockReason_.clear();
        downgradeBody_.clear();
        return true;
    }
    void ContinueVersionCheck() {
        if (versionCheckStarted_) {
            if (!versionCheckHandled_) SetPage(Page::Checking);
            else if (updateAvailable_ && !updateSkipped_) SetPage(Page::Update);
            else SetPage(Page::License);
            return;
        }
        versionCheckStarted_ = true;
        InstallLog::Line(L"Checking for newer setup on GitHub…");
        SetPage(Page::Checking);
        const HWND target = window;
        try {
            versionCheckThread_ = std::thread([this, target]() {
                VersionCheckResult result;
                try {
                    result.fetched = FetchLatestSetupVersion(result.latest, result.url);
                    if (result.fetched) {
                        result.self = SelfProductVersion();
                        result.newer = !result.self.empty() && VersionOlder(result.self, result.latest);
                    }
                } catch (...) {
                    result.fetched = false;
                    result.newer = false;
                }
                result.completed = true;
                {
                    std::lock_guard<std::mutex> lock(versionCheckMutex_);
                    versionCheckResult_ = std::move(result);
                }
                PostMessageW(target, kVersionCheckComplete, 0, 0);
            });
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(versionCheckMutex_);
                versionCheckResult_ = VersionCheckResult{};
                versionCheckResult_.completed = true;
            }
            PostMessageW(target, kVersionCheckComplete, 0, 0);
        }
    }
    void StartVersionCheck() {
        if (page_!=Page::Welcome) return;
        if (!RunPreflightChecks()) return;
        ContinueVersionCheck();
    }
    void RecheckConflicts() {
        if (page_!=Page::Conflicts || transitioning_) return;
        if (!RunPreflightChecks()) return;
        InstallLog::Line(L"Conflict recheck passed; resuming setup version check");
        ContinueVersionCheck();
    }
    void CompleteVersionCheck() {
        if (page_ != Page::Checking) return;
        VersionCheckResult result;
        {
            std::lock_guard<std::mutex> lock(versionCheckMutex_);
            if (!versionCheckResult_.completed) return;
            result = versionCheckResult_;
        }
        versionCheckHandled_ = true;
        if (!result.fetched) {
            InstallLog::Line(L"Version check: fetch failed or skipped; continuing with this setup");
            updateAvailable_ = false;
            SetPage(Page::License);
            return;
        }
        InstallLog::LineFormat(L"Version check: local=%s remote=%s",
            result.self.c_str(), result.latest.c_str());
        if (result.self.empty() || !result.newer) {
            InstallLog::Line(L"Version check: no newer setup; continuing with this setup");
            updateAvailable_ = false;
            SetPage(Page::License);
            return;
        }
        updateAvailable_ = true;
        updateCurrent_ = result.self;
        updateLatest_ = result.latest;
        updateUrl_ = result.url;
        updateBody_ = FormatUpdateBody(result.self, result.latest);
        InstallLog::Line(L"Version check: showing update page");
        SetPage(Page::Update);
    }
#ifdef _DEBUG
    void MaybeStartDebugDemo() {
        if (!debugDemoPending_ || debugDemoStarted_ || !webReady_ || !webBridgeReady_) return;
        debugDemoStarted_=true;
        debugDemoPending_=false;
        debugDemoActive_=true;
        debugDemoStep_=0;
        accepted_=false;
        SendMessageW(accept_,BM_SETCHECK,BST_UNCHECKED,0);
        updateAvailable_=false;
        updateSkipped_=false;
        simulateInstall_=false;
        InstallLog::Line(L"Debug page tour started; no update or installation actions will run");
        SetPage(Page::Welcome);
        SetTimer(window,kDebugDemoTimer,kDebugDemoPageMs,nullptr);
    }
    void SetDebugDemoPage(Page page) {
        switch (page) {
        case Page::Update:
            updateCurrent_=SelfProductVersion();
            if (updateCurrent_.empty()) updateCurrent_=L"1.0.0.0";
            updateLatest_=L"1.0.1.0";
            updateUrl_=L"https://github.com/musicstashall/GlassPlatform/releases/latest";
            updateBody_=FormatUpdateBody(updateCurrent_,updateLatest_);
            updateAvailable_=true;
            break;
        case Page::Conflicts:
            conflicts_={L"explorerblurmica",L"translucentflyouts",L"translucenttb"};
            blockReason_.clear();
            break;
        case Page::Downgrade:
            conflicts_.clear();
            installedGlassVersion_=L"3.0.0.0";
            setupProductVersion_=L"2.0.0.0";
            downgradeBody_=FormatVersionPairText(IDS_DOWNGRADE_BODY,setupProductVersion_,installedGlassVersion_);
            blockReason_.clear();
            break;
        case Page::VersionInvalid:
            conflicts_.clear();
            installedGlassVersion_.clear();
            setupProductVersion_=SelfProductVersion();
            offerCleanInstall_=true;
            blockReason_=Text(IDS_VERSION_INVALID_BODY);
            downgradeBody_.clear();
            break;
        case Page::License:
            accepted_=false;
            SendMessageW(accept_,BM_SETCHECK,BST_UNCHECKED,0);
            break;
        case Page::Installing:
            simulateInstall_=false;
            visualMode_=VisualMode::Slideshow;
            progressMarquee_=false;
            progressFinishing_=false;
            percent_=43;
            progressValue_=4300;
            displayProgress_=4300;
            progressTarget_=4300;
            installStatusLine_=Text(IDS_SIMULATION_NOTE);
            break;
        case Page::ModulesBusy:
            holdReport_=L"GlassSettings.exe (PID 4120): GlassCore.dll, WndCompat.dll\r\n"
                        L"explorer.exe (PID 2080): ExplorerCompat.dll, TaskbarCompat.dll";
            break;
        case Page::ProcessesStuck:
            holdReport_=L"explorer.exe (PID 2080): ExplorerCompat.dll, TaskbarCompat.dll\r\n"
                        L"GlassSettings.exe (PID 4120): GlassCore.dll, WndCompat.dll";
            break;
        case Page::Complete:
            percent_=100;
            progressValue_=10000;
            displayProgress_=10000;
            progressTarget_=10000;
            progressMarquee_=false;
            break;
        case Page::Error:
            errorReason_=Text(IDS_DEBUG_ERROR_REASON);
            break;
        default:
            break;
        }
        SetPage(page);
    }
    void FinishDebugDemo() {
        KillTimer(window,kDebugDemoTimer);
        debugDemoActive_=false;
        debugDemoPending_=false;
        accepted_=false;
        SendMessageW(accept_,BM_SETCHECK,BST_UNCHECKED,0);
        simulateInstall_=false;
        updateAvailable_=false;
        updateSkipped_=false;
        offerCleanInstall_=false;
        forceCleanInstall_=false;
        errorReason_.clear();
        holdReport_.clear();
        conflicts_.clear();
        installedGlassVersion_.clear();
        setupProductVersion_.clear();
        blockReason_.clear();
        downgradeBody_.clear();
        progressMarquee_=false;
        percent_=0;
        progressValue_=0;
        displayProgress_=0;
        progressTarget_=0;
        installStatusLine_.clear();
        visualMode_=VisualMode::Slideshow;
        InstallLog::Line(L"Debug page tour finished; returned to Welcome");
        SetPage(Page::Welcome);
    }
    void TickDebugDemo() {
        static constexpr Page sequence[]={Page::Welcome,Page::Checking,Page::Update,
            Page::Conflicts,Page::Downgrade,Page::VersionInvalid,Page::License,Page::Components,
            Page::Installing,Page::ModulesBusy,Page::ProcessesStuck,Page::ConfirmCancel,Page::Cancelling,Page::Cancelled,Page::Launching,Page::Complete,Page::Error};
        constexpr size_t count=sizeof(sequence)/sizeof(sequence[0]);
        if (!debugDemoActive_) { KillTimer(window,kDebugDemoTimer); return; }
        if (debugDemoStep_+1>=count) { FinishDebugDemo(); return; }
        ++debugDemoStep_;
        SetDebugDemoPage(sequence[debugDemoStep_]);
    }
#endif
#ifndef GLASS_EMBED_MSI
    void TickSimulation() {
        const ULONGLONG now=GetTickCount64();
        const ULONGLONG elapsed=now-started_;
        if (elapsed>=kDuration-5000 && page_==Page::ConfirmCancel) return;
        if (elapsed>=kDuration-5000 && page_!=Page::Launching) {
            launchStarted_=now;
            installStatusLine_=Text(IDS_LAUNCH_TITLE);
            SetPage(Page::Launching);
        }
        progressMarquee_=false;
        progressValue_=elapsed>=kDuration ? 10000 : static_cast<int>(elapsed*10000/kDuration);
        displayProgress_=progressValue_; percent_=progressValue_/100;
        if (elapsed>=kDuration && launchStarted_ && now-launchStarted_>=5000) {
            simulateInstall_=false; KillTimer(window,kInstallTimer); SetPage(Page::Complete); return;
        }
        InvalidateRect(progress_,nullptr,FALSE); InvalidateFooter(); SyncWebViewState();
    }
#endif
    void Tick() {
#ifndef GLASS_EMBED_MSI
        if (simulateInstall_) { TickSimulation(); return; }
#endif
        if (installEngine_.TakeErrorPageRequest()) {
            ShowFailurePage(false);
            installEngine_.NotifyErrorPageShown();
        }
        if (page_==Page::Error) {
            if (installEngine_.Poll()) { ShowFailurePage(true); return; }
            installStatusLine_=installEngine_.StatusLine();
            SyncWebViewState();
            return;
        }
        if (page_!=Page::Installing && page_!=Page::ModulesBusy && page_!=Page::ProcessesStuck &&
            page_!=Page::ConfirmCancel && page_!=Page::Cancelling && page_!=Page::Launching) return;

        const ULONGLONG now=GetTickCount64();
        const bool finished=installEngine_.Poll();
        if (finished && installEngine_.WasCancelled()) {
            KillTimer(window,kInstallTimer);
            cancelledDetail_=installEngine_.RollbackNote();
            if (installEngine_.RollbackSucceeded()) SetPage(Page::Cancelled);
            else ShowFailurePage(true);
            return;
        }
        if (finished && !installEngine_.Succeeded()) { ShowFailurePage(true); return; }
        if (installEngine_.IsLaunchingPhase() || (finished && installEngine_.Succeeded())) {
            if (!launchStarted_) launchStarted_=now;
            if (page_!=Page::Launching) SetPage(Page::Launching);
        } else if (page_!=Page::ConfirmCancel && page_!=Page::Cancelling) {
            const auto hold=installEngine_.CurrentHold();
            if (hold==InstallEngine::HoldKind::Modules && page_!=Page::ModulesBusy) {
                holdReport_=installEngine_.HoldReport(); SetPage(Page::ModulesBusy);
            } else if (hold==InstallEngine::HoldKind::Processes && page_!=Page::ProcessesStuck) {
                holdReport_=installEngine_.HoldReport(); SetPage(Page::ProcessesStuck);
            } else if (hold==InstallEngine::HoldKind::None &&
                       (page_==Page::ModulesBusy || page_==Page::ProcessesStuck)) SetPage(Page::Installing);
        }

        const ULONGLONG dt=progressTickMs_ ? now-progressTickMs_ : kInstallTimerMs;
        progressTickMs_=now;
        progressReverse_=installEngine_.IsProgressReverse();
        progressMarquee_=installEngine_.IsMarquee();
        if (progressMarquee_) {
            marqueePhase_=static_cast<double>((now-started_)%kMarqueePeriodMs)/kMarqueePeriodMs;
        } else {
            progressTarget_=std::clamp(installEngine_.ProgressUnits(),0,10000);
            if (page_==Page::Launching && launchStarted_) {
                const int cap=8800+static_cast<int>(std::min<ULONGLONG>(5000,now-launchStarted_)*1200/5000);
                progressTarget_=std::min(progressTarget_,cap);
            }
            const double alpha=std::min(1.0,static_cast<double>(dt)/kProgressSmoothTauMs);
            int step=static_cast<int>(std::lround((progressTarget_-displayProgress_)*alpha));
            if (!step && progressTarget_!=displayProgress_) step=progressTarget_>displayProgress_?1:-1;
            displayProgress_+=step;
            progressValue_=std::clamp(displayProgress_,0,10000);
            percent_=progressReverse_?(10000-progressValue_)/100:progressValue_/100;
        }
        installStatusLine_=installEngine_.StatusLine();
        if (finished && installEngine_.Succeeded() && launchStarted_ && now-launchStarted_>=5000) {
            KillTimer(window,kInstallTimer); progressValue_=displayProgress_=10000; percent_=100;
            SetPage(Page::Complete); return;
        }
        InvalidateRect(progress_,nullptr,FALSE); InvalidateFooter();
        if (now-lastWebPush_>=100) { lastWebPush_=now; SyncWebViewState(); }
    }
    void RequestClose() {
#ifdef _DEBUG
        if (debugDemoActive_ || (debugDemoPending_ && page_==Page::Welcome)) {
            KillTimer(window,kDebugDemoTimer); debugDemoActive_=false; debugDemoPending_=false;
            DestroyWindow(window); return;
        }
#endif
        if (page_==Page::RuntimeInstalling) {
            runtimeCancel_.store(true); EnableWindow(cancel_,FALSE);
            runtimeStatus_=PRIMARYLANGID(gLanguage)==LANG_RUSSIAN?L"Отмена, дождитесь завершения…":L"Cancelling; please wait…";
            InvalidateContent(); InvalidateFooter(); return;
        }
        if (page_==Page::Cancelling || page_==Page::Launching || page_==Page::ConfirmCancel ||
            (page_==Page::Error && failurePending_)) return;
        if (page_==Page::Installing || page_==Page::ModulesBusy || page_==Page::ProcessesStuck) {
#ifndef GLASS_EMBED_MSI
            if (simulateInstall_) { beforeCancel_=page_; SetPage(Page::ConfirmCancel); return; }
#endif
            if (installEngine_.PauseForCancelDecision()) {
                beforeCancel_=page_; SetPage(Page::ConfirmCancel);
            }
            return;
        }
        DestroyWindow(window);
    }
    void WebFailure(HRESULT hr) {
        webError_ = FAILED(hr) ? hr : E_FAIL;
        webReady_ = false;
        webBridgeReady_ = false; webInitSent_=false; lastWebState_.clear();
#ifdef _DEBUG
        if (debugDemoPending_) {
            debugDemoPending_=false;
            EnableWindow(back_,TRUE);
            EnableWindow(next_,page_!=Page::License || accepted_);
            EnableWindow(updateDownload_,TRUE);
            EnableWindow(updateSkip_,TRUE);
        }
#endif
        if (controller_) controller_->put_IsVisible(FALSE);
        ShowWindow(webHost_,SW_HIDE);
        if (page_==Page::Welcome) KillTimer(window,kHeroTimer);
        ShowContentControls(); // Reveal native EULA and the native fallback page.
        InvalidateContent();
    }
    HRESULT HandleWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
        if (closing_ || !args) return S_OK;
        LPWSTR source=nullptr;
        if (FAILED(args->get_Source(&source))) return S_OK;
        const std::wstring url=source?source:L"";
        CoTaskMemFree(source);
        if (url!=std::wstring(kDocumentUrl)+L"?lang=ru-RU" &&
            url!=std::wstring(kDocumentUrl)+L"?lang=en-US") return S_OK;
        LPWSTR raw=nullptr;
        const HRESULT get=args->TryGetWebMessageAsString(&raw);
        if (FAILED(get) || !raw) { CoTaskMemFree(raw); return S_OK; }
        const std::wstring message(raw);
        CoTaskMemFree(raw);
        if (message.size()>64) return S_OK;
        if (message==L"cancel") { RequestClose(); return S_OK; }
        if (message==L"ready") {
            webBridgeReady_=true;
            SyncWebViewState();
#ifdef _DEBUG
            MaybeStartDebugDemo();
#endif
            return S_OK;
        }
        if (page_==Page::Components && !DebugDemoLocksControls()) {
            const bool os64=GlassInstallUtil::IsOs64Bit();
            if (message==L"component:x86:1" || message==L"component:x86:0") selection_.platform32=!os64 || message.back()==L'1';
            else if (message==L"component:settings:1" || message==L"component:settings:0") selection_.glassSettings=message.back()==L'1';
            else if (message==L"component:openglass:1" || message==L"component:openglass:0") selection_.openGlass=os64 && message.back()==L'1';
            selection_.platform64=os64;
            SyncWebViewState();
            return S_OK;
        }
        if (page_==Page::License && !DebugDemoLocksControls() &&
            (message==L"accept:1" || message==L"accept:0")) {
            accepted_=message==L"accept:1";
            SendMessageW(accept_,BM_SETCHECK,accepted_?BST_CHECKED:BST_UNCHECKED,0);
            EnableWindow(next_,accepted_);
            SyncWebViewState();
        }
        return S_OK;
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
                const auto asset=WizardAssets::Resolve(url);
                const bool allowed=asset.id!=0;
                ComPtr<IStream> body;
                if (allowed) {
                    const auto blob=Resource(asset.id);
                    if (!blob.data || !blob.size) return E_FAIL;
                    body.Attach(SHCreateMemStream(blob.data,blob.size));
                    if (!body) return E_OUTOFMEMORY;
                }
                std::wstring headers=L"Content-Type: ";
                headers+=allowed?asset.contentType:L"text/plain; charset=utf-8";
                headers+=L"\r\nCache-Control: no-store\r\n";
                if (asset.document) headers+=
                    L"Content-Security-Policy: default-src 'none'; img-src https://glassplatform.invalid data:; "
                    L"font-src https://glassplatform.invalid; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                    L"connect-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'\r\n";
                ComPtr<ICoreWebView2WebResourceResponse> response;
                HRESULT hr=environment_->CreateWebResourceResponse(body.Get(),allowed?200:403,
                    allowed?L"OK":L"Forbidden",headers.c_str(),&response);
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
        HRESULT messageResult=web_->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                return HandleWebMessage(args);
            }).Get(),&webMessageToken_);
        if (FAILED(messageResult)) { WebFailure(messageResult); return; }
        web_->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (closing_) return S_OK;
                BOOL success = FALSE; args->get_IsSuccess(&success);
                if (!success) { WebFailure(E_FAIL); return S_OK; }
                KillTimer(window,kFadeTimer);
                transitioning_=false;
                webReady_ = true; webError_ = S_OK;
                KillTimer(window,kHeroTimer);
                ShowContentControls();
                SyncWebViewState();
#ifdef _DEBUG
                MaybeStartDebugDemo();
#endif
                InvalidateContent();
                return S_OK;
            }).Get(),&completeToken_);
        // C++ chooses the language BEFORE navigation / the first rendered HTML frame.
        std::wstring url = std::wstring(kDocumentUrl) +
            (PRIMARYLANGID(gLanguage)==LANG_RUSSIAN ? L"?lang=ru-RU" : L"?lang=en-US");
        HRESULT nav = web_->Navigate(url.c_str());
        if (FAILED(nav)) WebFailure(nav);
    }
    void Erase(HDC dc) {
        // Paint the footer into the existing back buffer: no system-grey Win32 strip.
        RECT footer=Box(0,kContentHeight,kWidth,kFooterHeight);
        FillRect(dc,&footer,footerBrush_);
        Gdiplus::Graphics g(dc);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        Gdiplus::LinearGradientBrush fill(
            Gdiplus::Point(footer.left,footer.top),Gdiplus::Point(footer.left,footer.bottom),
            Gdiplus::Color(255,GetRValue(kFooterTop),GetGValue(kFooterTop),GetBValue(kFooterTop)),
            Gdiplus::Color(255,GetRValue(kFooterBottom),GetGValue(kFooterBottom),GetBValue(kFooterBottom)));
        g.FillRectangle(&fill,GdiInt(footer.left),GdiInt(footer.top),
            GdiInt(footer.right-footer.left),GdiInt(footer.bottom-footer.top));
        RECT rule{footer.left,footer.top,footer.right,footer.top+Scale(1)};
        FillRect(dc,&rule,footerRuleBrush_);
    }
    void DrawLabel(HDC dc, const std::wstring& s, RECT rect, HFONT font, COLORREF color,
                   UINT format = DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX) {
        HGDIOBJ old = SelectObject(dc,font);
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,color);
        DrawTextW(dc,s.c_str(),static_cast<int>(s.size()),&rect,format);
        SelectObject(dc,old);
    }
    void DrawBrandMark(HDC dc, int x, int y) {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::RectF a(static_cast<Gdiplus::REAL>(Scale(x)),static_cast<Gdiplus::REAL>(Scale(y)),
                         static_cast<Gdiplus::REAL>(Scale(16)),static_cast<Gdiplus::REAL>(Scale(20)));
        Gdiplus::RectF b(static_cast<Gdiplus::REAL>(Scale(x+8)),static_cast<Gdiplus::REAL>(Scale(y+5)),
                         static_cast<Gdiplus::REAL>(Scale(16)),static_cast<Gdiplus::REAL>(Scale(20)));
        Gdiplus::GraphicsPath first,second;
        AddRoundedRectPath(first,a,static_cast<Gdiplus::REAL>(Scale(4)));
        AddRoundedRectPath(second,b,static_cast<Gdiplus::REAL>(Scale(4)));
        Gdiplus::LinearGradientBrush fillA(a,Gdiplus::Color(105,177,232,255),
            Gdiplus::Color(72,141,110,255),Gdiplus::LinearGradientModeForwardDiagonal);
        Gdiplus::LinearGradientBrush fillB(b,Gdiplus::Color(115,232,193,255),
            Gdiplus::Color(64,117,156,255),Gdiplus::LinearGradientModeForwardDiagonal);
        Gdiplus::Pen outline(Gdiplus::Color(150,210,233,255),static_cast<Gdiplus::REAL>(Scale(1)));
        g.FillPath(&fillA,&first); g.DrawPath(&outline,&first);
        g.FillPath(&fillB,&second); g.DrawPath(&outline,&second);
    }
    void DrawSuccessIllustration(HDC dc) {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::REAL cx=static_cast<Gdiplus::REAL>(Scale(790));
        const Gdiplus::REAL cy=static_cast<Gdiplus::REAL>(Scale(260));
        const Gdiplus::REAL outer=static_cast<Gdiplus::REAL>(Scale(165));
        Gdiplus::SolidBrush glow(Gdiplus::Color(24,129,112,213));
        g.FillEllipse(&glow,GdiReal(cx-outer),GdiReal(cy-outer),GdiReal(outer*2),GdiReal(outer*2));
        Gdiplus::Pen orbit(Gdiplus::Color(72,193,180,255),static_cast<Gdiplus::REAL>(Scale(1)));
        orbit.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawEllipse(&orbit,GdiReal(cx-outer),GdiReal(cy-outer),GdiReal(outer*2),GdiReal(outer*2));
        Gdiplus::Pen innerOrbit(Gdiplus::Color(50,167,221,223),static_cast<Gdiplus::REAL>(Scale(1)));
        g.DrawEllipse(&innerOrbit,GdiReal(cx-outer*0.78f),GdiReal(cy-outer*0.78f),GdiReal(outer*1.56f),GdiReal(outer*1.56f));
        Gdiplus::SolidBrush disc(Gdiplus::Color(220,23,25,47));
        g.FillEllipse(&disc,GdiReal(cx-Scale(82)),GdiReal(cy-Scale(82)),GdiReal(Scale(164)),GdiReal(Scale(164)));
        Gdiplus::Pen ring(Gdiplus::Color(220,200,190,255),static_cast<Gdiplus::REAL>(Scale(2)));
        g.DrawEllipse(&ring,GdiReal(cx-Scale(82)),GdiReal(cy-Scale(82)),GdiReal(Scale(164)),GdiReal(Scale(164)));
        Gdiplus::Pen check(Gdiplus::Color(255,226,255,245),static_cast<Gdiplus::REAL>(Scale(8)));
        check.SetStartCap(Gdiplus::LineCapRound); check.SetEndCap(Gdiplus::LineCapRound);
        check.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::PointF points[3] = {
            {cx-static_cast<Gdiplus::REAL>(Scale(37)),cy+static_cast<Gdiplus::REAL>(Scale(1))},
            {cx-static_cast<Gdiplus::REAL>(Scale(9)),cy+static_cast<Gdiplus::REAL>(Scale(28))},
            {cx+static_cast<Gdiplus::REAL>(Scale(43)),cy-static_cast<Gdiplus::REAL>(Scale(34))}
        };
        g.DrawLines(&check,points,3);
        Gdiplus::SolidBrush spark(Gdiplus::Color(225,167,221,223));
        g.FillEllipse(&spark,GdiReal(cx+Scale(116)),GdiReal(cy-Scale(83)),GdiReal(Scale(7)),GdiReal(Scale(7)));
        g.FillEllipse(&spark,GdiReal(cx-Scale(128)),GdiReal(cy+Scale(85)),GdiReal(Scale(5)),GdiReal(Scale(5)));
    }
    void DrawLeftTextMask(HDC dc, int width) {
        Gdiplus::Graphics g(dc);
        Gdiplus::LinearGradientBrush shade(
            Gdiplus::Point(0,0),Gdiplus::Point(Scale(width),0),
            Gdiplus::Color(255,16,18,37),Gdiplus::Color(0,16,18,37));
        g.FillRectangle(&shade,GdiInt(0),GdiInt(0),GdiInt(Scale(width)),GdiInt(Scale(kHtmlHeight)));
    }
    void DrawHeroPanel(HDC dc, int x, int y, int width, int height, COLORREF accentColor) {
        RECT r=Box(x,y,width,height);
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::RectF bounds(static_cast<Gdiplus::REAL>(r.left),static_cast<Gdiplus::REAL>(r.top),
            static_cast<Gdiplus::REAL>(r.right-r.left),static_cast<Gdiplus::REAL>(r.bottom-r.top));
        Gdiplus::GraphicsPath path;
        AddRoundedRectPath(path,bounds,static_cast<Gdiplus::REAL>(Scale(18)));
        Gdiplus::LinearGradientBrush fill(bounds,Gdiplus::Color(247,21,23,45),
            Gdiplus::Color(243,15,17,34),Gdiplus::LinearGradientModeVertical);
        g.FillPath(&fill,&path);
        const Gdiplus::Color accent(190,GetRValue(accentColor),GetGValue(accentColor),GetBValue(accentColor));
        Gdiplus::Pen border(Gdiplus::Color(92,GetRValue(accentColor),GetGValue(accentColor),GetBValue(accentColor)),
            static_cast<Gdiplus::REAL>(Scale(1)));
        g.DrawPath(&border,&path);
        Gdiplus::Pen rail(accent,static_cast<Gdiplus::REAL>(Scale(2)));
        rail.SetStartCap(Gdiplus::LineCapRound); rail.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&rail,GdiInt(r.left+Scale(2)),GdiInt(r.top+Scale(26)),GdiInt(r.left+Scale(2)),GdiInt(r.bottom-Scale(26)));
        Gdiplus::Pen highlight(Gdiplus::Color(32,255,255,255),static_cast<Gdiplus::REAL>(Scale(1)));
        g.DrawLine(&highlight,GdiInt(r.left+Scale(20)),GdiInt(r.top+Scale(1)),GdiInt(r.right-Scale(20)),GdiInt(r.top+Scale(1)));
    }
    void DrawVersionCard(HDC dc, int x, int y, int width, int height,
                         const std::wstring& label, const std::wstring& version, COLORREF tint) {
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::RectF r(static_cast<Gdiplus::REAL>(Scale(x)),static_cast<Gdiplus::REAL>(Scale(y)),
                static_cast<Gdiplus::REAL>(Scale(width)),static_cast<Gdiplus::REAL>(Scale(height)));
            Gdiplus::GraphicsPath path;
            AddRoundedRectPath(path,r,static_cast<Gdiplus::REAL>(Scale(14)));
            Gdiplus::LinearGradientBrush fill(r,Gdiplus::Color(238,31,32,57),
                Gdiplus::Color(226,20,22,43),Gdiplus::LinearGradientModeVertical);
            Gdiplus::Pen border(Gdiplus::Color(138,GetRValue(tint),GetGValue(tint),GetBValue(tint)),
                static_cast<Gdiplus::REAL>(Scale(1)));
            g.FillPath(&fill,&path); g.DrawPath(&border,&path);
        }
        const RECT labelRect=Box(x+13,y+13,width-26,18);
        const RECT versionRect=Box(x+13,y+35,width-26,height-43);
        DrawLabel(dc,label,labelRect,small_,RGB(169,174,199),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        const std::wstring displayVersion=version.empty()?std::wstring(L"—"):version;
        DrawLabel(dc,displayVersion,versionRect,heading_,RGB(247,245,255),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    void DrawUpdateComparison(HDC dc) {
        const ULONGLONG now=GetTickCount64();
        const double phase=static_cast<double>(now%4200)/4200.0;
        const double wave=0.5+0.5*std::sin(phase*6.283185307179586);
        const int currentX=426, latestX=732, cardY=164, cardW=220, cardH=218;
        const Gdiplus::REAL glowX=static_cast<Gdiplus::REAL>(Scale(latestX+cardW/2));
        const Gdiplus::REAL glowY=static_cast<Gdiplus::REAL>(Scale(cardY+cardH/2));
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush glow(Gdiplus::Color(static_cast<BYTE>(16+11*wave),133,116,211));
            g.FillEllipse(&glow,GdiReal(glowX-Scale(112)),GdiReal(glowY-Scale(112)),
                GdiReal(Scale(224)),GdiReal(Scale(224)));
        }
        DrawVersionCard(dc,currentX,cardY,cardW,cardH,
            Text(IDS_UPDATE_CURRENT),updateCurrent_,RGB(161,164,195));
        DrawVersionCard(dc,latestX,cardY,cardW,cardH,
            Text(IDS_UPDATE_LATEST),updateLatest_,RGB(193,180,255));
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const Gdiplus::REAL left=static_cast<Gdiplus::REAL>(Scale(666));
            const Gdiplus::REAL top=static_cast<Gdiplus::REAL>(Scale(249));
            const Gdiplus::REAL diameter=static_cast<Gdiplus::REAL>(Scale(46));
            Gdiplus::SolidBrush halo(Gdiplus::Color(static_cast<BYTE>(22+15*wave),145,127,222));
            g.FillEllipse(&halo,GdiReal(left-Scale(9)),GdiReal(top-Scale(9)),
                GdiReal(diameter+Scale(18)),GdiReal(diameter+Scale(18)));
            Gdiplus::SolidBrush disc(Gdiplus::Color(238,32,31,54));
            g.FillEllipse(&disc,GdiReal(left),GdiReal(top),GdiReal(diameter),GdiReal(diameter));
            Gdiplus::Pen border(Gdiplus::Color(170,202,190,255),static_cast<Gdiplus::REAL>(Scale(1)));
            g.DrawEllipse(&border,GdiReal(left),GdiReal(top),GdiReal(diameter),GdiReal(diameter));
            Gdiplus::Pen arrow(Gdiplus::Color(245,225,219,255),static_cast<Gdiplus::REAL>(Scale(2)));
            arrow.SetStartCap(Gdiplus::LineCapRound); arrow.SetEndCap(Gdiplus::LineCapRound);
            const Gdiplus::REAL x1=static_cast<Gdiplus::REAL>(Scale(678));
            const Gdiplus::REAL x2=static_cast<Gdiplus::REAL>(Scale(699));
            const Gdiplus::REAL y=static_cast<Gdiplus::REAL>(Scale(272));
            g.DrawLine(&arrow,x1,y,x2,y);
            g.DrawLine(&arrow,GdiReal(x2-Scale(7)),GdiReal(y-Scale(7)),x2,y);
            g.DrawLine(&arrow,GdiReal(x2-Scale(7)),GdiReal(y+Scale(7)),x2,y);
        }
    }
    void DrawFailureIllustration(HDC dc) {
        const ULONGLONG now=GetTickCount64();
        const double phase=static_cast<double>(now%6100)/6100.0;
        const double wave=0.5+0.5*std::sin(phase*6.283185307179586);
        const Gdiplus::REAL cx=static_cast<Gdiplus::REAL>(Scale(795));
        const Gdiplus::REAL cy=static_cast<Gdiplus::REAL>(Scale(261));
        const Gdiplus::REAL outer=static_cast<Gdiplus::REAL>(Scale(165));
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::SolidBrush glow(Gdiplus::Color(static_cast<BYTE>(20+14*wave),229,80,111));
        g.FillEllipse(&glow,GdiReal(cx-outer),GdiReal(cy-outer),GdiReal(outer*2),GdiReal(outer*2));
        Gdiplus::Pen orbit(Gdiplus::Color(92,255,127,151),static_cast<Gdiplus::REAL>(Scale(1)));
        orbit.SetDashStyle(Gdiplus::DashStyleDash);
        g.DrawEllipse(&orbit,GdiReal(cx-outer),GdiReal(cy-outer),GdiReal(outer*2),GdiReal(outer*2));
        Gdiplus::Pen innerOrbit(Gdiplus::Color(54,220,153,169),static_cast<Gdiplus::REAL>(Scale(1)));
        g.DrawEllipse(&innerOrbit,GdiReal(cx-outer*0.78f),GdiReal(cy-outer*0.78f),GdiReal(outer*1.56f),GdiReal(outer*1.56f));
        const double angle=phase*6.283185307179586+0.6;
        const Gdiplus::REAL px=cx+static_cast<Gdiplus::REAL>(std::cos(angle))*outer;
        const Gdiplus::REAL py=cy+static_cast<Gdiplus::REAL>(std::sin(angle))*outer*0.72f;
        Gdiplus::SolidBrush orbitGlow(Gdiplus::Color(58,255,127,151));
        Gdiplus::SolidBrush orbitDot(Gdiplus::Color(250,255,197,207));
        g.FillEllipse(&orbitGlow,GdiReal(px-Scale(11)),GdiReal(py-Scale(11)),GdiReal(Scale(22)),GdiReal(Scale(22)));
        g.FillEllipse(&orbitDot,GdiReal(px-Scale(3)),GdiReal(py-Scale(3)),GdiReal(Scale(6)),GdiReal(Scale(6)));
        Gdiplus::SolidBrush disc(Gdiplus::Color(228,31,23,47));
        g.FillEllipse(&disc,GdiReal(cx-Scale(82)),GdiReal(cy-Scale(82)),GdiReal(Scale(164)),GdiReal(Scale(164)));
        Gdiplus::Pen ring(Gdiplus::Color(230,255,127,151),static_cast<Gdiplus::REAL>(Scale(2)));
        g.DrawEllipse(&ring,GdiReal(cx-Scale(82)),GdiReal(cy-Scale(82)),GdiReal(Scale(164)),GdiReal(Scale(164)));
        Gdiplus::Pen cross(Gdiplus::Color(255,255,220,226),static_cast<Gdiplus::REAL>(Scale(8)));
        cross.SetStartCap(Gdiplus::LineCapRound); cross.SetEndCap(Gdiplus::LineCapRound);
        cross.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawLine(&cross,GdiReal(cx-Scale(30)),GdiReal(cy-Scale(30)),GdiReal(cx+Scale(30)),GdiReal(cy+Scale(30)));
        g.DrawLine(&cross,GdiReal(cx+Scale(30)),GdiReal(cy-Scale(30)),GdiReal(cx-Scale(30)),GdiReal(cy+Scale(30)));
        Gdiplus::SolidBrush spark(Gdiplus::Color(232,255,171,185));
        g.FillEllipse(&spark,GdiReal(cx+Scale(119)),GdiReal(cy-Scale(73)),GdiReal(Scale(7)),GdiReal(Scale(7)));
        g.FillEllipse(&spark,GdiReal(cx-Scale(127)),GdiReal(cy+Scale(81)),GdiReal(Scale(5)),GdiReal(Scale(5)));
    }
    void DrawLoadingScreen(HDC dc) {
        const double phase=static_cast<double>(GetTickCount64()%1200)/1200.0;
        const Gdiplus::REAL x=static_cast<Gdiplus::REAL>(Scale(473));
        const Gdiplus::REAL y=static_cast<Gdiplus::REAL>(Scale(243));
        const Gdiplus::REAL diameter=static_cast<Gdiplus::REAL>(Scale(54));
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen track(Gdiplus::Color(140,77,79,111),static_cast<Gdiplus::REAL>(Scale(3)));
        Gdiplus::Pen arc(Gdiplus::Color(255,193,180,255),static_cast<Gdiplus::REAL>(Scale(3)));
        arc.SetStartCap(Gdiplus::LineCapRound); arc.SetEndCap(Gdiplus::LineCapRound);
        g.DrawEllipse(&track,x,y,diameter,diameter);
        g.DrawArc(&arc,x,y,diameter,diameter,GdiReal(-90.0+phase*360.0),GdiReal(268.0));
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
        Gdiplus::Rect dest(Scale(240),Scale(100),Scale(800),Scale(440));
        g.DrawImage(banner_.get(),dest,0,0,static_cast<INT>(banner_->GetWidth()),
                    static_cast<INT>(banner_->GetHeight()),Gdiplus::UnitPixel,&attrs);
        Gdiplus::LinearGradientBrush shade(
            Gdiplus::Point(Scale(240),0),Gdiplus::Point(Scale(590),0),
            Gdiplus::Color(255,16,18,37),Gdiplus::Color(0,16,18,37));
        g.FillRectangle(&shade,GdiInt(Scale(240)),GdiInt(Scale(100)),GdiInt(Scale(350)),GdiInt(Scale(440)));
    }
    void DrawBannerFull(HDC dc) {
        if (!banner_) return;
        Gdiplus::Graphics g(dc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        Gdiplus::Rect dest(0, 0, Scale(kWidth), Scale(kHtmlHeight));
        g.DrawImage(banner_.get(), dest, 0, 0, static_cast<INT>(banner_->GetWidth()),
                    static_cast<INT>(banner_->GetHeight()), Gdiplus::UnitPixel);
    }
    void RenderContent(HDC dc) {
        RECT content=Box(0,0,kWidth,kContentHeight);
        FillRect(dc,&content,darkBrush_);
        const COLORREF white=RGB(245,244,252), muted=RGB(163,168,191), accent=RGB(193,180,255);
        if (page_==Page::RuntimeRequired || page_==Page::RuntimeInstalling || page_==Page::RuntimeError || page_==Page::AlreadyRunning) {
            DrawBanner(dc,true);
            DrawBrandMark(dc,48,29);
            DrawLabel(dc,Text(IDS_BRAND),Box(82,27,340,30),font_,white,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            const UINT titleId=page_==Page::RuntimeRequired?IDS_RUNTIME_REQUIRED_TITLE:
                page_==Page::RuntimeInstalling?IDS_RUNTIME_INSTALLING_TITLE:
                page_==Page::AlreadyRunning?IDS_ALREADY_RUNNING_TITLE:IDS_RUNTIME_INSTALL_FAILED;
            const std::wstring body=page_==Page::RuntimeRequired?Text(IDS_RUNTIME_MISSING):
                page_==Page::RuntimeInstalling?runtimeStatus_:
                page_==Page::AlreadyRunning?Text(IDS_ALREADY_RUNNING_BODY):runtimeError_;
            DrawLabel(dc,Text(titleId),Box(48,140,620,110),heading_,white);
            DrawLabel(dc,body,Box(48,250,620,190),font_,muted);
        } else if (page_==Page::Components && !webReady_) {
            DrawBanner(dc,true);
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(IDS_COMP_TITLE),Box(48,90,700,50),heading_,white);
            DrawLabel(dc,Text(IDS_COMP_INTRO),Box(48,145,840,36),font_,muted);
            if (!selection_.glassSettings) DrawLabel(dc,Text(IDS_COMP_GS_WARNING),Box(48,390,840,90),font_,white);
        } else if (page_==Page::ConfirmCancel || page_==Page::Cancelled || page_==Page::Cancelling || page_==Page::Launching) {
            DrawBanner(dc,true);
            const UINT title=page_==Page::ConfirmCancel?IDS_CONFIRM_CANCEL_TITLE:page_==Page::Cancelled?IDS_CANCELLED_TITLE:
                page_==Page::Launching?IDS_LAUNCH_TITLE:IDS_STATUS_ROLLBACK;
            const UINT body=page_==Page::ConfirmCancel?IDS_CONFIRM_CANCEL_BODY:page_==Page::Cancelled?IDS_CANCELLED_BODY:
                page_==Page::Launching?IDS_LAUNCH_BODY:IDS_CONFIRM_CANCEL_NOTE;
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(title),Box(48,150,540,75),heading_,white);
            DrawLabel(dc,Text(body),Box(48,235,520,140),font_,muted);
            if (page_==Page::Cancelled && !cancelledDetail_.empty()) DrawLabel(dc,cancelledDetail_,Box(48,380,520,80),font_,white);
            DrawSuccessIllustration(dc);
        } else if (page_ == Page::Complete) {
            DrawBanner(dc,true);
            DrawHeroPanel(dc,32,120,558,408,RGB(139,201,218));
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(IDS_DONE_TAG),Box(48,139,400,25),small_,RGB(190,226,218));
            DrawLabel(dc,Text(IDS_DONE_TITLE),Box(48,177,440,104),title_,white);
#ifdef GLASS_EMBED_MSI
            DrawLabel(dc,Text(IDS_DONE_BODY),Box(48,301,390,78),font_,muted);
            DrawLabel(dc,Text(IDS_DONE_NOTE),Box(48,474,450,52),small_,muted);
#else
            DrawLabel(dc,Text(IDS_DONE_BODY_DEMO),Box(48,301,390,90),font_,muted);
            DrawLabel(dc,Text(IDS_DONE_NOTE_DEMO),Box(48,474,450,52),small_,muted);
#endif
            DrawBrandMark(dc,48,408);
            DrawLabel(dc,Text(IDS_DONE_READY),Box(80,408,260,26),small_,RGB(205,235,225),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            DrawSuccessIllustration(dc);
        } else if (page_ == Page::Error) {
            DrawBanner(dc,true);
            DrawHeroPanel(dc,32,120,558,408,RGB(255,112,140));
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(IDS_ERROR_TAG),Box(48,139,400,25),small_,RGB(255,142,163));
            DrawLabel(dc,Text(IDS_ERROR_TITLE),Box(48,177,440,104),title_,white);
            DrawLabel(dc,Text(IDS_ERROR_BODY),Box(48,301,390,54),font_,muted);
            DrawLabel(dc,errorReason_,Box(48,362,520,92),font_,white,DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            DrawLabel(dc,Text(IDS_ERROR_LOG_HINT),Box(48,476,450,50),small_,muted);
            DrawFailureIllustration(dc);
        } else if (page_ == Page::ModulesBusy || page_ == Page::ProcessesStuck) {
            DrawBanner(dc,true);
            const bool kill=page_==Page::ProcessesStuck;
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(kill?IDS_KILL_TITLE:IDS_HOLD_TITLE),Box(48,139,520,70),heading_,white);
            DrawLabel(dc,Text(kill?IDS_KILL_BODY:IDS_HOLD_BODY),Box(48,220,460,136),font_,muted);
            DrawLabel(dc,holdReport_,Box(48,365,520,165),font_,white,DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        } else if (page_ == Page::Update) {
            DrawBannerFull(dc);
            DrawLeftTextMask(dc,480);
            DrawBrandMark(dc,48,29);
            DrawLabel(dc,Text(IDS_BRAND),Box(82,27,340,30),font_,white,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DrawLabel(dc,Text(IDS_UPDATE_TAG),Box(48,137,340,24),small_,accent,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            DrawLabel(dc,Text(IDS_UPDATE_TITLE),Box(48,171,344,100),title_,white,
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            DrawLabel(dc,updateBody_,Box(48,286,340,142),font_,muted,
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            DrawUpdateComparison(dc);
        } else if (page_ == Page::Checking) {
            DrawBanner(dc,true);
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(IDS_CHECKING_TITLE),Box(48,137,520,72),heading_,white);
            DrawLabel(dc,Text(IDS_CHECKING_BODY),Box(48,226,470,128),font_,muted);
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen track(Gdiplus::Color(140,92,87,121),static_cast<Gdiplus::REAL>(Scale(2)));
            Gdiplus::Pen active(Gdiplus::Color(255,193,180,255),static_cast<Gdiplus::REAL>(Scale(2)));
            const int x=Scale(49),y=Scale(392),d=Scale(24);
            g.DrawEllipse(&track,GdiInt(x),GdiInt(y),GdiInt(d),GdiInt(d));
            g.DrawArc(&active,GdiInt(x),GdiInt(y),GdiInt(d),GdiInt(d),GdiReal(-90),GdiReal(265));
        } else if (page_ == Page::License) {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush glow(Gdiplus::Color(16,112,91,174));
            g.FillEllipse(&glow,GdiInt(Scale(610)),GdiInt(Scale(-110)),GdiInt(Scale(510)),GdiInt(Scale(410)));
            DrawBrandMark(dc,48,21);
            DrawLabel(dc,Text(IDS_BRAND),Box(82,20,350,26),font_,white,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DrawLabel(dc,Text(IDS_EULA_TITLE),Box(48,63,904,36),heading_,white);
            DrawLabel(dc,Text(IDS_EULA_BODY),Box(48,104,904,26),font_,muted);
            RECT panel=Box(47,141,906,287);
            Gdiplus::GraphicsPath path;
            AddRoundedRectPath(path,Gdiplus::RectF(static_cast<Gdiplus::REAL>(panel.left),
                static_cast<Gdiplus::REAL>(panel.top),static_cast<Gdiplus::REAL>(panel.right-panel.left),
                static_cast<Gdiplus::REAL>(panel.bottom-panel.top)),static_cast<Gdiplus::REAL>(Scale(10)));
            Gdiplus::Pen border(Gdiplus::Color(78,193,180,255),static_cast<Gdiplus::REAL>(Scale(1)));
            g.DrawPath(&border,&path);
        } else if (page_==Page::Conflicts) {
            DrawBanner(dc,true);
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(IDS_CONFLICTS_TAG),Box(48,111,560,20),small_,RGB(255,183,197));
            DrawLabel(dc,Text(IDS_CONFLICTS_TITLE),Box(48,139,585,43),heading_,white);
            DrawLabel(dc,Text(IDS_CONFLICTS_BODY),Box(48,188,555,48),font_,muted);
            int rowY=245;
            for (const auto& conflict:conflicts_) {
                UINT nameId=conflict==L"explorerblurmica"?IDS_CONFLICT_EXPLORER_NAME:
                    conflict==L"translucentflyouts"?IDS_CONFLICT_FLYOUTS_NAME:IDS_CONFLICT_TB_NAME;
                UINT detailId=conflict==L"explorerblurmica"?IDS_CONFLICT_EXPLORER_DETAIL:
                    conflict==L"translucentflyouts"?IDS_CONFLICT_FLYOUTS_DETAIL:IDS_CONFLICT_TB_DETAIL;
                DrawLabel(dc,Text(nameId),Box(60,rowY,520,22),font_,white,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
                DrawLabel(dc,Text(detailId),Box(60,rowY+22,520,38),small_,muted,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX);
                rowY+=64;
            }
            DrawLabel(dc,Text(IDS_CONFLICTS_INSTRUCTIONS),Box(48,443,558,78),small_,white,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX);
        } else if (page_==Page::Downgrade) {
            DrawBanner(dc,true);
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(IDS_DOWNGRADE_TAG),Box(48,121,520,20),small_,RGB(255,183,197));
            DrawLabel(dc,Text(IDS_DOWNGRADE_TITLE),Box(48,151,550,78),heading_,white,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX);
            DrawLabel(dc,downgradeBody_,Box(48,238,520,126),font_,muted,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX);
            DrawLabel(dc,Text(IDS_INSTALLED_VERSION_LABEL)+L": "+installedGlassVersion_,Box(48,382,520,30),font_,white);
            DrawLabel(dc,Text(IDS_SETUP_VERSION_LABEL)+L": "+setupProductVersion_,Box(48,420,520,30),font_,white);
            DrawLabel(dc,Text(IDS_DOWNGRADE_NOTE),Box(48,489,520,34),small_,muted);
        } else if (page_==Page::VersionInvalid) {
            DrawBanner(dc,true);
            DrawLabel(dc,Text(IDS_BRAND),Box(48,34,300,35),heading_,white);
            DrawLabel(dc,Text(IDS_VERSION_INVALID_TAG),Box(48,121,520,20),small_,RGB(255,183,197));
            DrawLabel(dc,Text(IDS_VERSION_INVALID_TITLE),Box(48,151,550,78),heading_,white,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX);
            DrawLabel(dc,blockReason_,Box(48,238,520,126),font_,muted,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX);
            DrawLabel(dc,Text(IDS_INSTALLED_VERSION_LABEL)+L": "+(installedGlassVersion_.empty()?L"—":installedGlassVersion_),Box(48,382,520,30),font_,white);
            DrawLabel(dc,Text(IDS_SETUP_VERSION_LABEL)+L": "+(setupProductVersion_.empty()?L"—":setupProductVersion_),Box(48,420,520,30),font_,white);
            DrawLabel(dc,Text(offerCleanInstall_?IDS_VERSION_INVALID_NOTE:IDS_SETUP_VERSION_INVALID_NOTE),Box(48,489,520,34),small_,muted);
        } else if (page_==Page::Welcome && !webReady_ && SUCCEEDED(webError_)) {
            DrawLoadingScreen(dc);
        } else if (!webReady_ && FAILED(webError_)) {
            wchar_t code[16]{};
            swprintf_s(code,L"0x%08lX",static_cast<unsigned long>(webError_));
            const std::wstring status=Text(IDS_WEB_ERROR)+code;
            DrawLabel(dc,status,Box(80,220,840,100),font_,muted,DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        }
    }
    void RenderFooterText(HDC dc) {
        if (page_!=Page::Installing && page_!=Page::Cancelling && page_!=Page::Launching && page_!=Page::RuntimeInstalling) return;
        const std::wstring line=page_==Page::RuntimeInstalling?runtimeStatus_:installStatusLine_.empty()?Text(IDS_STATUS_PREPARE):installStatusLine_;
        const std::wstring shown=progressMarquee_?line:line+L"  "+std::to_wstring(percent_)+L"%";
        RECT dot=Box(32,kContentHeight+22,8,8);
        HBRUSH dotBrush=CreateSolidBrush(RGB(183,168,255));
        HGDIOBJ oldBrush=SelectObject(dc,dotBrush);
        HGDIOBJ oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
        Ellipse(dc,dot.left,dot.top,dot.right,dot.bottom);
        SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(dotBrush);
        DrawLabel(dc,shown,Box(50,kContentHeight+10,560,34),font_,RGB(231,229,246),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
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
                FillRect(dc,&content,darkBrush_);
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
        KillTimer(window,kInstallTimer); KillTimer(window,kFadeTimer); KillTimer(window,kHeroTimer);
#ifdef _DEBUG
        KillTimer(window,kDebugDemoTimer);
#endif
        runtimeCancel_.store(true);
        if (runtimeThread_.joinable()) runtimeThread_.join();
        if (versionCheckThread_.joinable()) versionCheckThread_.join();
        frame_.Reset(); fadeFrame_.Reset();
        if (web_ && webMessageToken_.value) web_->remove_WebMessageReceived(webMessageToken_);
        if (controller_) controller_->Close();
        web_.Reset(); controller_.Reset(); environment_.Reset();
        banner_.reset(); bannerStream_.Reset();
        for (HWND button : {back_,next_,cancel_,updateDownload_,updateSkip_})
            if (button && IsWindow(button)) WizardTheme::DetachButtonScope(button);
        WizardTheme::ResetButtonCache();
        for (HFONT f : {font_,heading_,small_,title_}) if (f) DeleteObject(f);
        if (darkBrush_) DeleteObject(darkBrush_);
        if (licenseBrush_) DeleteObject(licenseBrush_);
        if (buttonBrush_) DeleteObject(buttonBrush_);
        if (footerBrush_) DeleteObject(footerBrush_);
        if (footerRuleBrush_) DeleteObject(footerRuleBrush_);
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
        case kVersionCheckComplete: if (!closing_) CompleteVersionCheck(); return 0;
        case kRuntimeStatus: {
            std::lock_guard<std::mutex> lock(runtimeMutex_); runtimeStatus_=Text(static_cast<UINT>(w));
            InvalidateContent(); InvalidateFooter(); return 0;
        }
        case kRuntimeComplete: CompleteRuntimeInstallation(); return 0;
        case WM_PAINT: Paint(); return 0;
        case WM_SIZE: Layout(); return 0;
        case WM_MOVE: if (controller_) controller_->NotifyParentWindowPositionChanged(); return 0;
        case WM_ACTIVATE:
        case WM_ACTIVATEAPP:
        case WM_SETFOCUS:
            InvalidatePushButtons();
            break;
        case WM_THEMECHANGED:
            EndTransition(); InvalidateRect(window,nullptr,FALSE); return 0;
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
            CreateFonts(); Layout(); BuildButtonImageCache(); InvalidateRect(window,nullptr,FALSE);
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
                    const double phase = progressReverse_ ? (1.0 - marqueePhase_) : marqueePhase_;
                    const int left = item->rcItem.left + static_cast<int>(phase * travel) - span;
                    RECT chip{left, item->rcItem.top, left + span, item->rcItem.bottom};
                    IntersectRect(&chip, &chip, &item->rcItem);
                    if (chip.right > chip.left)
                        FillRect(item->hDC,&chip,progressFill_);
                } else {
                    RECT filled=item->rcItem;
                    const int bar = MulDiv(filled.right-filled.left,progressValue_,10000);
                    if (progressReverse_) {
                        filled.left = filled.right - bar;
                    } else {
                        filled.right = filled.left + bar;
                    }
                    if (filled.right > filled.left)
                        FillRect(item->hDC,&filled,progressFill_);
                }
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORBTN: {
            HDC dc=reinterpret_cast<HDC>(w);
            HWND button=reinterpret_cast<HWND>(l);
            const bool footerButton=button==back_ || button==next_ || button==cancel_ ||
                                    button==updateDownload_ || button==updateSkip_;
            SetBkMode(dc,TRANSPARENT);
            SetBkColor(dc,footerButton?kButtonBase:kDark);
            // Footer captions are drawn by the DrawThemeTextEx detour.
            if (!footerButton) SetTextColor(dc,RGB(239,237,249));
            // The cache is alpha-composited over the exact footer gradient.
            return reinterpret_cast<LRESULT>(footerButton?buttonBrush_:darkBrush_);
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC dc=reinterpret_cast<HDC>(w);
            const bool license=page_==Page::License && reinterpret_cast<HWND>(l)==eula_;
            SetTextColor(dc,license?kLicenseText:RGB(245,244,252));
            SetBkColor(dc,license?kLicensePanel:kDark);
            return reinterpret_cast<LRESULT>(license?licenseBrush_:darkBrush_);
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
            case IDC_ACCEPT:
                if (HIWORD(w)==BN_CLICKED && page_==Page::License && !DebugDemoLocksControls()) {
                    accepted_ = SendMessageW(accept_,BM_GETCHECK,0,0)==BST_CHECKED;
                    EnableWindow(next_,accepted_);
                    InvalidateRect(accept_,nullptr,FALSE);
                    SyncWebViewState();
                }
                return 0;
            case IDC_UPDATE_DOWNLOAD:
                if (!DebugDemoLocksControls() && page_==Page::Update && !updateUrl_.empty()) {
                    InstallLog::LineFormat(L"Update download page: %s", updateUrl_.c_str());
                    if (!OpenReleaseInUserBrowser(updateUrl_)) InstallLog::Line(L"Could not open release page as desktop user");
                }
                return 0;
            case IDC_UPDATE_SKIP:
                if (!DebugDemoLocksControls() && page_==Page::Update) {
                    InstallLog::Line(L"Update skipped; continuing with this setup");
                    updateSkipped_ = true;
                    SetPage(Page::License);
                }
                return 0;
            case IDC_COMPONENT64: case IDC_COMPONENT32: case IDC_COMPONENT_SETTINGS: case IDC_COMPONENT_OG:
                if (page_==Page::Components && HIWORD(w)==BN_CLICKED && !webReady_) {
                    const bool os64=GlassInstallUtil::IsOs64Bit();
                    selection_.platform64=os64;
                    selection_.platform32=!os64 || SendMessageW(component32_,BM_GETCHECK,0,0)==BST_CHECKED;
                    selection_.glassSettings=SendMessageW(componentSettings_,BM_GETCHECK,0,0)==BST_CHECKED;
                    selection_.openGlass=os64 && SendMessageW(componentOpenGlass_,BM_GETCHECK,0,0)==BST_CHECKED;
                    InvalidateContent();
                }
                return 0;
            case IDC_BACK:
                if (!DebugDemoLocksControls() && page_==Page::ConfirmCancel) { installEngine_.ResumeAfterCancelDecision(); SetPage(beforeCancel_); return 0; }
                if (!DebugDemoLocksControls() && page_==Page::Components) { SetPage(Page::License); return 0; }
                if (!DebugDemoLocksControls() && !transitioning_ && page_==Page::License) {
                    forceCleanInstall_=false;
                    SetPage(Page::Welcome);
                }
                return 0;
            case IDOK:
            case IDC_NEXT:
#ifdef _DEBUG
                if (debugDemoPending_ && page_==Page::Welcome) return 0;
                if (debugDemoActive_) {
                    if (page_==Page::Complete || page_==Page::Error) RequestClose();
                    return 0;
                }
#endif
                if (transitioning_) return 0;
                if (page_==Page::RuntimeRequired) { StartRuntimeInstallation(); return 0; }
                if (page_==Page::ConfirmCancel) {
#ifndef GLASS_EMBED_MSI
                    if (simulateInstall_) { simulateInstall_=false; cancelledDetail_=Text(IDS_ERR_ROLLBACK_REMOVED); KillTimer(window,kInstallTimer); SetPage(Page::Cancelled); return 0; }
#endif
                    installEngine_.RequestCancel();
                    if (installEngine_.IsCancelRequested()) SetPage(Page::Cancelling);
                    return 0;
                }
                if (page_==Page::Conflicts) { RecheckConflicts(); return 0; }
                if (page_==Page::Downgrade) return 0;
                if (page_==Page::VersionInvalid) {
                    if (!offerCleanInstall_) return 0;
                    forceCleanInstall_=true;
                    InstallLog::Line(L"User accepted clean reinstall after integrity failure");
                    ContinueVersionCheck();
                    return 0;
                }
                if (w==IDOK && page_==Page::Update) {
                    InstallLog::Line(L"Update skipped by keyboard; continuing with this setup");
                    updateSkipped_ = true;
                    SetPage(Page::License);
                    return 0;
                }
                if (page_==Page::ModulesBusy) {
                    visualMode_ = VisualMode::UnlockModules;
                    unlockStatusLine_ = Text(IDS_STATUS_FORCE_UNLOAD);
                    unlockVisualUntil_ = GetTickCount64() + 700;
                    unlockStatusObserved_ = true;
                    installEngine_.SetStatus(unlockStatusLine_,-1);
                    installStatusLine_ = unlockStatusLine_;
                    installEngine_.SignalHold(InstallEngine::HoldAction::Retry);
                    SetPage(Page::Installing);
                    return 0;
                }
                if (page_==Page::ProcessesStuck) {
                    visualMode_ = VisualMode::UnlockProcesses;
                    unlockStatusLine_ = Text(IDS_STATUS_UNLOCK_PROCESSES);
                    unlockVisualUntil_ = GetTickCount64() + 700;
                    unlockStatusObserved_ = true;
                    installEngine_.SetStatus(unlockStatusLine_,-1);
                    installStatusLine_ = unlockStatusLine_;
                    installEngine_.SignalHold(InstallEngine::HoldAction::Unlock);
                    SetPage(Page::Installing);
                    return 0;
                }
                if (page_==Page::Welcome) StartVersionCheck();
                else if (page_==Page::License && accepted_) SetPage(Page::Components);
                else if (page_==Page::Components) StartInstallation();
                else if (page_==Page::Complete || page_==Page::Cancelled || page_==Page::Error || page_==Page::RuntimeError || page_==Page::AlreadyRunning) RequestClose();
                return 0;
            case IDCANCEL:
            case IDC_CANCEL: RequestClose(); return 0;
            }
            break;
        case WM_TIMER:
            if (w==kInstallTimer) Tick();
#ifdef _DEBUG
            else if (w==kDebugDemoTimer) TickDebugDemo();
#endif
            else if (w==kFadeTimer && transitioning_) {
                if (GetTickCount64()-fadeStarted_>=kFadeDuration) EndTransition();
                else InvalidateContent();
            } else if (w==kHeroTimer && !webReady_ &&
                       (page_==Page::Welcome || page_==Page::Update || page_==Page::Error || page_==Page::RuntimeInstalling)) {
                if (page_==Page::RuntimeInstalling) { progressMarquee_=true; marqueePhase_=static_cast<double>(GetTickCount64()%kMarqueePeriodMs)/kMarqueePeriodMs; InvalidateRect(progress_,nullptr,FALSE); }
                InvalidateContent();
            }
            return 0;
        case WM_SYSCOMMAND: if ((w & 0xfff0)==SC_CLOSE) { RequestClose(); return 0; } break;
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
            gRootWindow=hwnd;
            WizardTheme::SetRootWindow(hwnd,kContentHeight,kHeight,kFooterTop,kFooterBottom);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        const LRESULT result=self ? self->Handle(msg,w,l) : DefWindowProcW(hwnd,msg,w,l);
        if (msg==WM_NCDESTROY && gRootWindow==hwnd) { gRootWindow=nullptr; WizardTheme::SetRootWindow(nullptr); }
        return result;
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
        InstallLog::Shutdown();
        return 4;
    }
    HANDLE instanceMutex=CreateMutexW(nullptr,FALSE,L"Local\\GlassSetup.Instance.Mutex");
    if (!instanceMutex) { InstallLog::Line(L"CreateMutex failed"); CoUninitialize(); InstallLog::Shutdown(); return 4; }
    gAlreadyRunning=GetLastError()==ERROR_ALREADY_EXISTS;
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput graphicsInput;
    if (Gdiplus::GdiplusStartup(&gdiplusToken,&graphicsInput,nullptr)!=Gdiplus::Ok) {
        CloseHandle(instanceMutex);
        if (SUCCEEDED(com)) CoUninitialize();
        InstallLog::Shutdown();
        return 3;
    }
    const bool themed=WizardTheme::InstallDetours();
    InstallLog::Line(themed ? L"Theme Detours installed" : L"Theme Detours unavailable");
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
    if (gWindowClassRegistered) {
        UnregisterClassW(L"GlassSetupWindow",gInstance);
        gWindowClassRegistered=false;
    }
    if (gWindowClassBrush) {
        DeleteObject(gWindowClassBrush);
        gWindowClassBrush=nullptr;
    }
    WizardTheme::UninstallDetours();
    GlassInstallUtil::CleanupEmbeddedUnlocker();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    CloseHandle(instanceMutex);
    InstallLog::Shutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    return exitCode;
}
