// Win32-мастер GlassPlatform. Release ставит MSI; Debug без GLASS_EMBED_MSI автоматически показывает все страницы без установки.
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
enum class Page { Welcome, Checking, Update, Conflicts, Downgrade, VersionInvalid, License, Installing, ModulesBusy, ProcessesStuck, Complete, Error };
enum class VisualMode { Slideshow, UnlockModules, UnlockProcesses };
HINSTANCE gInstance = nullptr;
HBRUSH gWindowClassBrush = nullptr;
bool gWindowClassRegistered = false;
HWND gRootWindow = nullptr;
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
    wchar_t tempPath[MAX_PATH]{};
    const DWORD length=GetTempPathW(static_cast<DWORD>(std::size(tempPath)),tempPath);
    if (!length || length>=std::size(tempPath)) {
        error=L"GetTempPathW failed ("+std::to_wstring(GetLastError())+L")";
        return false;
    }
    wchar_t unique[MAX_PATH]{};
    if (!GetTempFileNameW(tempPath,L"GPW",0,unique)) {
        error=L"GetTempFileNameW failed ("+std::to_wstring(GetLastError())+L")";
        return false;
    }
    if (!DeleteFileW(unique) || !CreateDirectoryW(unique,nullptr)) {
        error=L"Could not create a private temporary folder ("+
            std::to_wstring(GetLastError())+L")";
        DeleteFileW(unique);
        return false;
    }
    directory=unique;
    installer=directory+L"\\InstallerWebView2.exe";
    return true;
}

void RemoveWebView2TempFiles(const std::wstring& directory,
                             const std::wstring& installer) noexcept {
    if (!installer.empty()) DeleteFileW(installer.c_str());
    if (!directory.empty()) RemoveDirectoryW(directory.c_str());
}

bool DownloadWebView2Bootstrapper(const std::wstring& outputPath, std::wstring& error) {
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
    WinHttpSetTimeouts(session,10000,10000,15000,30000);
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
    DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
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
        DWORD available=0;
        if (!WinHttpQueryDataAvailable(request,&available)) {
            error=L"WinHttpQueryDataAvailable failed ("+
                std::to_wstring(GetLastError())+L")";
            ok=false;
            break;
        }
        if (!available) break;
        while (ok && available) {
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
    if (status==ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* provider=WTHelperProvDataFromStateData(trust.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        if (signer && signer->csCertChain && signer->pasCertChain && signer->pasCertChain[0].pCert) {
            wchar_t publisher[256]{};
            const DWORD chars=CertGetNameStringW(signer->pasCertChain[0].pCert,
                CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,publisher,static_cast<DWORD>(std::size(publisher)));
            microsoftPublisher=chars>1 && _wcsicmp(publisher,L"Microsoft Corporation")==0;
        }
    }
    trust.dwStateAction=WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr,&action,&trust);
    if (status!=ERROR_SUCCESS) {
        error=L"The downloaded WebView2 installer did not pass Authenticode verification ("+
            std::to_wstring(static_cast<unsigned long>(status))+L")";
        return false;
    }
    if (!microsoftPublisher) {
        error=L"The downloaded WebView2 installer is not signed by Microsoft Corporation";
        return false;
    }
    return true;
}

bool RunWebView2Bootstrapper(const std::wstring& installerPath,
                             const std::wstring& directory,
                             DWORD& exitCode, std::wstring& error) {
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
    const DWORD wait=WaitForSingleObject(process.hProcess,INFINITE);
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

bool HasWebView2RestartSwitch() {
    int argc=0;
    LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (!argv) return false;
    bool found=false;
    for (int i=1;i<argc;++i) {
        if (_wcsicmp(argv[i],L"--glass-webview2-restarted")==0) { found=true; break; }
    }
    LocalFree(argv);
    return found;
}

bool RelaunchCurrentSetup(std::wstring& error) {
    wchar_t executable[MAX_PATH]{};
    const DWORD length=GetModuleFileNameW(nullptr,executable,static_cast<DWORD>(std::size(executable)));
    if (!length || length>=std::size(executable)) {
        error=L"Could not resolve the setup executable path ("+
            std::to_wstring(GetLastError())+L")";
        return false;
    }
    std::wstring command=GetCommandLineW();
    if (command.empty()) command=L"\""+std::wstring(executable)+L"\"";
    command+=L" --glass-webview2-restarted";
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,
                        &startup,&process)) {
        error=L"Could not restart GlassSetup ("+std::to_wstring(GetLastError())+L")";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

struct RuntimeInstallContext {
    std::thread worker;
    bool success=false;
    DWORD installerExitCode=0;
    std::wstring error;
};
void SetRuntimeDialogContent(HWND dialog, UINT id) {
    const std::wstring text=Text(id);
    SendMessageW(dialog,TDM_SET_ELEMENT_TEXT,TDE_CONTENT,
                 reinterpret_cast<LPARAM>(text.c_str()));
}
HRESULT CALLBACK RuntimeInstallDialogProc(HWND dialog, UINT notification,
                                           WPARAM, LPARAM, LONG_PTR reference) {
    auto* context=reinterpret_cast<RuntimeInstallContext*>(reference);
    if (notification==TDN_CREATED && context) {
        SendMessageW(dialog,TDM_ENABLE_BUTTON,IDOK,FALSE);
        SendMessageW(dialog,TDM_SET_PROGRESS_BAR_MARQUEE,TRUE,35);
        try {
            context->worker=std::thread([context,dialog]() {
                std::wstring directory,installer,error;
                DWORD installerExitCode=0;
                bool success=false;
                try {
                    if (!CreateWebView2TempPath(directory,installer,error)) {
                        // Keep the detailed Win32 error for the final message.
                    } else {
                        SetRuntimeDialogContent(dialog,IDS_RUNTIME_DOWNLOADING);
                        if (DownloadWebView2Bootstrapper(installer,error)) {
                            if (VerifyMicrosoftSignature(installer,error)) {
                                SetRuntimeDialogContent(dialog,IDS_RUNTIME_INSTALLING_PACKAGE);
                                if (RunWebView2Bootstrapper(installer,directory,installerExitCode,error)) {
                                    SetRuntimeDialogContent(dialog,IDS_RUNTIME_VERIFYING);
                                    success=true;
                                }
                            }
                        }
                    }
                } catch (...) {
                    error=L"Unexpected error while acquiring or installing WebView2 Runtime";
                    success=false;
                }
                RemoveWebView2TempFiles(directory,installer);
                context->success=success;
                context->installerExitCode=installerExitCode;
                context->error=std::move(error);
                SendMessageW(dialog,TDM_ENABLE_BUTTON,IDOK,TRUE);
                SendMessageW(dialog,TDM_CLICK_BUTTON,IDOK,0);
            });
        } catch (...) {
            context->error=L"Could not start the WebView2 download worker";
            SendMessageW(dialog,TDM_ENABLE_BUTTON,IDOK,TRUE);
            SendMessageW(dialog,TDM_CLICK_BUTTON,IDOK,0);
        }
    }
    return S_OK;
}

bool InstallWebView2WithProgress(std::wstring& error) {
    RuntimeInstallContext context;
    const std::wstring title=Text(IDS_TITLE);
    const std::wstring instruction=Text(IDS_RUNTIME_INSTALLING_TITLE);
    const std::wstring content=Text(IDS_RUNTIME_INSTALLING);
    const std::wstring close=Text(IDS_FINISH);
    TASKDIALOG_BUTTON button{IDOK,close.c_str()};
    TASKDIALOGCONFIG config{};
    config.cbSize=sizeof(config);
    config.pszWindowTitle=title.c_str();
    config.pszMainInstruction=instruction.c_str();
    config.pszContent=content.c_str();
    config.dwFlags=TDF_SHOW_MARQUEE_PROGRESS_BAR;
    config.cButtons=1;
    config.pButtons=&button;
    config.nDefaultButton=IDOK;
    config.pfCallback=RuntimeInstallDialogProc;
    config.lpCallbackData=reinterpret_cast<LONG_PTR>(&context);
    int selected=0;
    const HRESULT dialogResult=TaskDialogIndirect(&config,&selected,nullptr,nullptr);
    if (context.worker.joinable()) context.worker.join();
    if (FAILED(dialogResult)) {
        error=L"TaskDialogIndirect failed ("+
            std::to_wstring(static_cast<unsigned long>(dialogResult))+L")";
        return false;
    }
    error=context.error;
    if (!context.success) return false;
    if (!IsWebView2RuntimeInstalled()) {
        error=L"The WebView2 bootstrapper exited with code "+
            std::to_wstring(context.installerExitCode)+
            L", but WebView2 Runtime was not detected.";
        return false;
    }
    return true;
}

int PromptToInstallWebView2() {
    const std::wstring title=Text(IDS_TITLE);
    const std::wstring instruction=Text(IDS_RUNTIME_REQUIRED_TITLE);
    const std::wstring content=Text(IDS_RUNTIME_MISSING);
    const std::wstring install=Text(IDS_RUNTIME_INSTALL);
    const std::wstring cancel=Text(IDS_CANCEL);
    const TASKDIALOG_BUTTON buttons[]={{IDYES,install.c_str()},{IDCANCEL,cancel.c_str()}};
    TASKDIALOGCONFIG config{};
    config.cbSize=sizeof(config);
    config.pszWindowTitle=title.c_str();
    config.pszMainInstruction=instruction.c_str();
    config.pszContent=content.c_str();
    config.pszMainIcon=TD_WARNING_ICON;
    config.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION;
    config.cButtons=static_cast<UINT>(std::size(buttons));
    config.pButtons=buttons;
    config.nDefaultButton=IDCANCEL;
    int selected=IDCANCEL;
    const HRESULT result=TaskDialogIndirect(&config,&selected,nullptr,nullptr);
    if (FAILED(result)) {
        const std::wstring message=instruction+L"\r\n\r\n"+content;
        return MessageBoxW(nullptr,message.c_str(),title.c_str(),
            MB_OKCANCEL|MB_ICONWARNING|MB_DEFBUTTON2)==IDOK?IDYES:IDCANCEL;
    }
    return selected;
}

enum class RuntimeGateResult { Ready, Restarted, Cancelled, Failed };
RuntimeGateResult EnsureWebView2Runtime(bool restartAttempt) {
    std::wstring version;
    if (IsWebView2RuntimeInstalled(&version)) {
        InstallLog::LineFormat(L"WebView2 Runtime detected: %s",version.c_str());
        return RuntimeGateResult::Ready;
    }
    InstallLog::Line(L"WebView2 Runtime is not installed");
    if (restartAttempt) {
        const std::wstring message=Text(IDS_RUNTIME_INSTALL_FAILED);
        MessageBoxW(nullptr,message.c_str(),Text(IDS_TITLE).c_str(),MB_OK|MB_ICONERROR);
        return RuntimeGateResult::Failed;
    }
    if (PromptToInstallWebView2()!=IDYES) {
        InstallLog::Line(L"WebView2 installation declined; exiting setup");
        return RuntimeGateResult::Cancelled;
    }
    std::wstring error;
    if (!InstallWebView2WithProgress(error)) {
        InstallLog::LineFormat(L"WebView2 installation failed: %s",error.c_str());
        std::wstring message=Text(IDS_RUNTIME_INSTALL_FAILED);
        if (!error.empty()) message+=L"\r\n\r\n"+error;
        MessageBoxW(nullptr,message.c_str(),Text(IDS_TITLE).c_str(),MB_OK|MB_ICONERROR);
        return RuntimeGateResult::Failed;
    }
    InstallLog::Line(L"WebView2 Runtime installed; restarting GlassSetup");
    if (!RelaunchCurrentSetup(error)) {
        InstallLog::LineFormat(L"GlassSetup restart failed: %s",error.c_str());
        std::wstring message=Text(IDS_RUNTIME_RESTART_FAILED);
        if (!error.empty()) message+=L"\r\n\r\n"+error;
        MessageBoxW(nullptr,message.c_str(),Text(IDS_TITLE).c_str(),MB_OK|MB_ICONERROR);
        return RuntimeGateResult::Failed;
    }
    return RuntimeGateResult::Restarted;
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

using DrawThemeBackgroundFn = HRESULT(WINAPI*)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
using DrawThemeTextFn = HRESULT(WINAPI*)(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
using DrawTextExWFn = int(WINAPI*)(HDC, LPWSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS);
PVOID g_originalDrawThemeBackground = nullptr;
PVOID g_originalDrawThemeText = nullptr;
DrawTextExWFn g_originalDrawTextExW = nullptr;
HMODULE g_uxThemeModule = nullptr;
bool g_themeDetoursInstalled = false;
LONG g_themeDetourStatus = ERROR_PROC_NOT_FOUND;
bool g_uxthemeDrawTextExImportFound = false;
LONG g_uxthemeDrawTextExImportStatus = ERROR_PROC_NOT_FOUND;
PVOID* g_uxthemeDrawTextExImportSlot = nullptr;
std::recursive_mutex g_drawThemeTextHookMutex;
unsigned g_drawTextExHookDepth = 0;
bool g_drawTextExHookOwned = false;
constexpr UINT_PTR kButtonSubclassId = 0x47505354;
thread_local HWND g_dtbCurrentButton = nullptr;

struct ThemeTextCallState {
    int part = 0;
    int state = 0;
    HWND button = nullptr;
    bool focusedBlue = false;
    bool active = false;
};
// Keep the DrawThemeText part/state on the painting thread; nested and concurrent paints must not collide.
thread_local ThemeTextCallState g_themeTextCallState{};

#ifdef _DEBUG
std::atomic_bool g_dtbAnyCallLogged{false};
std::atomic_bool g_dtbButtonCallLogged{false};
std::atomic_bool g_dtbCacheHitLogged{false};
std::atomic_uint g_drawThemeTextStateLogMask{0};
std::atomic_uint g_drawTextExStateLogMask{0};
#endif

struct ButtonSkin {
    COLORREF top;
    COLORREF bottom;
    COLORREF border;
    COLORREF highlight;
    BYTE opacity;
};

ButtonSkin ButtonSkinFor(bool focusedBlue, int state) noexcept {
    if (focusedBlue) {
        switch (state) {
        case PBS_HOT: return {RGB(62,137,207),RGB(37,86,145),RGB(132,205,255),RGB(217,243,255),255};
        case PBS_PRESSED: return {RGB(30,77,127),RGB(24,57,98),RGB(80,161,226),RGB(122,191,239),255};
        case PBS_DISABLED: return {RGB(43,58,75),RGB(31,40,56),RGB(69,89,111),RGB(84,108,132),150};
        case PBS_DEFAULTED:
        case PBS_DEFAULTED_ANIMATING: return {RGB(51,119,183),RGB(34,76,129),RGB(112,195,255),RGB(209,239,255),255};
        default: return {RGB(47,109,171),RGB(32,73,124),RGB(93,180,247),RGB(198,231,255),255};
        }
    }
    switch (state) {
    case PBS_HOT: return {RGB(58,62,89),RGB(34,37,63),RGB(132,139,179),RGB(205,207,232),255};
    case PBS_PRESSED: return {RGB(21,23,42),RGB(32,35,56),RGB(84,90,126),RGB(110,115,148),255};
    case PBS_DISABLED: return {RGB(30,32,47),RGB(23,25,39),RGB(62,65,86),RGB(75,78,98),150};
    case PBS_DEFAULTED:
    case PBS_DEFAULTED_ANIMATING: return {RGB(47,49,72),RGB(27,29,49),RGB(119,125,164),RGB(190,192,218),255};
    default: return {RGB(42,45,67),RGB(25,27,47),RGB(91,96,132),RGB(165,170,202),255};
    }
}

COLORREF MixColor(COLORREF from, COLORREF to, double amount) noexcept {
    amount = std::clamp(amount, 0.0, 1.0);
    const auto mix = [amount](BYTE a, BYTE b) {
        return static_cast<BYTE>(std::lround(a + (b - a) * amount));
    };
    return RGB(mix(GetRValue(from),GetRValue(to)),
               mix(GetGValue(from),GetGValue(to)),
               mix(GetBValue(from),GetBValue(to)));
}

bool InsideRoundedRect(double x, double y, double width, double height, double radius) noexcept {
    if (x < 0.0 || y < 0.0 || x >= width || y >= height) return false;
    const double cx = std::clamp(x, radius, width - radius);
    const double cy = std::clamp(y, radius, height - radius);
    const double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

void RasterizeButtonImage(DWORD* pixels, int width, int height, bool focusedBlue, int state) noexcept {
    if (!pixels || width <= 0 || height <= 0) return;
    const ButtonSkin skin = ButtonSkinFor(focusedBlue,state);
    constexpr int kSamples = 4;
    constexpr int kSampleCount = kSamples * kSamples;
    const double radius = std::min(7.5, std::min(width,height) / 3.0);
    const double innerRadius = std::max(1.0,radius - 1.25);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned alphaSum = 0, redSum = 0, greenSum = 0, blueSum = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const double px = x + (sx + 0.5) / kSamples;
                    const double py = y + (sy + 0.5) / kSamples;
                    if (!InsideRoundedRect(px,py,width,height,radius)) continue;
                    const bool border = !InsideRoundedRect(px - 1.25,py - 1.25,
                        width - 2.5,height - 2.5,innerRadius);
                    const double vertical = height > 1 ? py / (height - 1.0) : 0.0;
                    COLORREF color = border ? skin.border : MixColor(skin.top,skin.bottom,vertical);
                    if (!border && state != PBS_PRESSED && py < 2.0)
                        color = MixColor(color,skin.highlight,0.34);
                    const unsigned opacity = skin.opacity;
                    alphaSum += opacity;
                    redSum += GetRValue(color) * opacity / 255;
                    greenSum += GetGValue(color) * opacity / 255;
                    blueSum += GetBValue(color) * opacity / 255;
                }
            }
            const unsigned a = (alphaSum + kSampleCount / 2) / kSampleCount;
            const unsigned r = (redSum + kSampleCount / 2) / kSampleCount;
            const unsigned g = (greenSum + kSampleCount / 2) / kSampleCount;
            const unsigned b = (blueSum + kSampleCount / 2) / kSampleCount;
            // Little-endian BGRA with premultiplied RGB, ready for AC_SRC_ALPHA.
            pixels[static_cast<size_t>(y) * width + x] =
                (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

class ButtonImageCache {
    struct Entry {
        int width = 0, height = 0, state = 0;
        bool focusedBlue = false;
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
    };
    std::vector<Entry> entries_;
public:
    ButtonImageCache() = default;
    ButtonImageCache(const ButtonImageCache&) = delete;
    ButtonImageCache& operator=(const ButtonImageCache&) = delete;
    ~ButtonImageCache() { Reset(); }

    void Reset() noexcept {
        for (auto& entry : entries_) {
            if (entry.dc) {
                if (entry.previous && entry.previous != HGDI_ERROR)
                    SelectObject(entry.dc,entry.previous);
                DeleteDC(entry.dc);
            }
            if (entry.bitmap) DeleteObject(entry.bitmap);
        }
        entries_.clear();
    }

    bool Build(HDC reference, const std::vector<int>& widths, int height) noexcept {
        Reset();
        if (!reference || height <= 0 || widths.empty()) return false;
        static constexpr int states[] = {
            PBS_NORMAL,PBS_HOT,PBS_PRESSED,PBS_DISABLED,PBS_DEFAULTED,PBS_DEFAULTED_ANIMATING
        };
        try {
            entries_.reserve(widths.size()*2*std::size(states));
            for (int width : widths) {
                if (width <= 0) continue;
                if (std::any_of(entries_.begin(),entries_.end(),[&](const Entry& e) {
                    return e.width == width && e.height == height;
                })) continue;
                for (bool blue : {false,true}) {
                    for (int state : states) {
                        Entry entry;
                        entry.width = width; entry.height = height; entry.state = state;
                        entry.focusedBlue = blue;
                        entry.dc = CreateCompatibleDC(reference);
                        if (!entry.dc) { Reset(); return false; }
                        BITMAPINFO info{};
                        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        info.bmiHeader.biWidth = width;
                        info.bmiHeader.biHeight = -height;
                        info.bmiHeader.biPlanes = 1;
                        info.bmiHeader.biBitCount = 32;
                        info.bmiHeader.biCompression = BI_RGB;
                        void* bits = nullptr;
                        entry.bitmap = CreateDIBSection(reference,&info,DIB_RGB_COLORS,&bits,nullptr,0);
                        if (!entry.bitmap || !bits) {
                            if (entry.bitmap) DeleteObject(entry.bitmap);
                            DeleteDC(entry.dc);
                            Reset();
                            return false;
                        }
                        entry.previous = SelectObject(entry.dc,entry.bitmap);
                        if (!entry.previous || entry.previous == HGDI_ERROR) {
                            DeleteDC(entry.dc); DeleteObject(entry.bitmap); Reset(); return false;
                        }
                        RasterizeButtonImage(static_cast<DWORD*>(bits),width,height,blue,state);
                        entries_.push_back(entry);
                    }
                }
            }
        } catch (...) {
            Reset();
            return false;
        }
        return !entries_.empty();
    }

    size_t EntryCount() const noexcept { return entries_.size(); }

    bool Contains(int width, int height, int state, bool focusedBlue) const noexcept {
        return std::any_of(entries_.begin(),entries_.end(),[&](const Entry& entry) {
            return entry.width == width && entry.height == height && entry.state == state &&
                   entry.focusedBlue == focusedBlue;
        });
    }

    bool Draw(HDC destination, LPCRECT rect, LPCRECT clip, int state, bool focusedBlue) const noexcept {
        if (!destination || !rect || IsRectEmpty(rect)) return false;
        const int width = rect->right - rect->left;
        const int height = rect->bottom - rect->top;
        auto found = std::find_if(entries_.begin(),entries_.end(),[&](const Entry& entry) {
            return entry.width == width && entry.height == height && entry.state == state &&
                   entry.focusedBlue == focusedBlue;
        });
        if (found == entries_.end()) return false;
        RECT clipped = *rect;
        if (clip && !IntersectRect(&clipped,rect,clip)) return true;
        const int drawWidth = clipped.right - clipped.left;
        const int drawHeight = clipped.bottom - clipped.top;
        const int sourceX = clipped.left - rect->left;
        const int sourceY = clipped.top - rect->top;
        const BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        return AlphaBlend(destination,clipped.left,clipped.top,drawWidth,drawHeight,
            found->dc,sourceX,sourceY,drawWidth,drawHeight,blend) != FALSE;
    }
};

ButtonImageCache g_buttonImageCache;

void PaintFooterBackdropForButton(HDC dc, HWND button, LPCRECT rect, LPCRECT clip) noexcept {
    if (!dc || !button || !rect || !gRootWindow || GetAncestor(button,GA_ROOT) != gRootWindow)
        return;
    POINT buttonOrigin{0,0};
    MapWindowPoints(button,gRootWindow,&buttonOrigin,1);
    const UINT dpi = GetDpiForWindow(gRootWindow);
    const int footerTop = MulDiv(kContentHeight,static_cast<int>(dpi),96);
    const int footerBottom = MulDiv(kHeight,static_cast<int>(dpi),96);
    RECT paint = *rect;
    if (clip && !IntersectRect(&paint,rect,clip)) return;
    const int saved = SaveDC(dc);
    IntersectClipRect(dc,paint.left,paint.top,paint.right,paint.bottom);
    Gdiplus::Graphics graphics(dc);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    Gdiplus::LinearGradientBrush backdrop(
        Gdiplus::Point(paint.left,footerTop-buttonOrigin.y),
        Gdiplus::Point(paint.left,footerBottom-buttonOrigin.y),
        Gdiplus::Color(255,GetRValue(kFooterTop),GetGValue(kFooterTop),GetBValue(kFooterTop)),
        Gdiplus::Color(255,GetRValue(kFooterBottom),GetGValue(kFooterBottom),GetBValue(kFooterBottom)));
    graphics.FillRectangle(&backdrop,GdiInt(paint.left),GdiInt(paint.top),
        GdiInt(paint.right-paint.left),GdiInt(paint.bottom-paint.top));
    RestoreDC(dc,saved);
}

LRESULT CALLBACK GlassButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR subclassId, DWORD_PTR reference) {
    // Some BUTTON implementations repaint synchronously while handling WM_ENABLE or state changes,
    // before a separate WM_PAINT arrives. Keep the originating HWND scoped for every message.
    HWND previous = g_dtbCurrentButton;
    g_dtbCurrentButton = hwnd;
    const LRESULT result = DefSubclassProc(hwnd,message,wParam,lParam);
    g_dtbCurrentButton = previous;
    return result;
}

bool IsActiveFocusedButton(HWND button) noexcept {
    return button && IsWindow(button) && button == GetFocus() && IsWindowEnabled(button) &&
           GetAncestor(button,GA_ROOT) == gRootWindow && GetForegroundWindow() == gRootWindow;
}

COLORREF ButtonTextColorForThemeState(int part, int state, bool focusedBlue) noexcept {
    if (part != BP_PUSHBUTTON) return RGB(239,237,249);
    if (state == PBS_DISABLED) return RGB(143,147,166);
    if (focusedBlue) {
        switch (state) {
        case PBS_HOT: return RGB(255,255,255);
        case PBS_PRESSED: return RGB(229,243,255);
        case PBS_DEFAULTED:
        case PBS_DEFAULTED_ANIMATING: return RGB(250,252,255);
        default: return RGB(246,250,255);
        }
    }
    switch (state) {
    case PBS_HOT: return RGB(250,250,255);
    case PBS_PRESSED: return RGB(222,224,239);
    case PBS_DEFAULTED:
    case PBS_DEFAULTED_ANIMATING: return RGB(246,245,253);
    default: return RGB(239,237,249);
    }
}

int WINAPI HookDrawTextExW(HDC dc, LPWSTR text, int textLength, LPRECT textRect,
                           UINT format, LPDRAWTEXTPARAMS params) {
    const auto original = g_originalDrawTextExW;
    if (!original) return 0;
    const ThemeTextCallState drawState = g_themeTextCallState;
    if (!dc || !drawState.active || drawState.part != BP_PUSHBUTTON ||
        !drawState.button || !IsWindow(drawState.button))
        return original(dc,text,textLength,textRect,format,params);

    const COLORREF textColor = ButtonTextColorForThemeState(
        drawState.part,drawState.state,drawState.focusedBlue);
    const COLORREF previousTextColor = SetTextColor(dc,textColor);
#ifdef _DEBUG
    const unsigned stateIndex=static_cast<unsigned>(drawState.state)&7u;
    const unsigned logBit=1u << (stateIndex+(drawState.focusedBlue?8u:0u));
    if (!(g_drawTextExStateLogMask.fetch_or(logBit,std::memory_order_relaxed)&logBit))
        InstallLog::LineFormat(L"UxTheme DrawTextExW button text: hwnd=%p part=%d state=%d focusedBlue=%d color=0x%06X",
            drawState.button,drawState.part,drawState.state,drawState.focusedBlue?1:0,
            static_cast<unsigned>(textColor));
#endif
    const int result = original(dc,text,textLength,textRect,format,params);
    if (previousTextColor != CLR_INVALID) SetTextColor(dc,previousTextColor);
    return result;
}

// Locate UxTheme's import slot once; it is swapped only for the duration of DrawThemeText.
bool IsUser32ImportLibrary(LPCSTR moduleName) noexcept {
    if (!moduleName) return false;
    if (_stricmp(moduleName,"USER32.dll") == 0) return true;
    static constexpr char apiSetPrefix[] = "API-MS-WIN-NTUSER-";
    static constexpr char extSetPrefix[] = "EXT-MS-WIN-NTUSER-";
    return _strnicmp(moduleName,apiSetPrefix,sizeof(apiSetPrefix)-1) == 0 ||
           _strnicmp(moduleName,extSetPrefix,sizeof(extSetPrefix)-1) == 0;
}

struct UxThemeDrawTextImportScan {
    bool currentFileIsUser32 = false;
    PVOID* slot = nullptr;
    PVOID original = nullptr;
    unsigned matches = 0;
};

BOOL CALLBACK UxThemeImportFileCallback(PVOID context, HMODULE importedModule, LPCSTR importedFile) {
    (void)importedModule;
    auto* scan = static_cast<UxThemeDrawTextImportScan*>(context);
    if (!scan) return FALSE;
    scan->currentFileIsUser32 = IsUser32ImportLibrary(importedFile);
    return TRUE;
}

BOOL CALLBACK UxThemeImportFuncCallbackEx(PVOID context, DWORD ordinal, LPCSTR functionName,
                                          PVOID* iatSlot) {
    (void)ordinal;
    auto* scan = static_cast<UxThemeDrawTextImportScan*>(context);
    if (!scan || !scan->currentFileIsUser32 || !functionName || !iatSlot ||
        _stricmp(functionName,"DrawTextExW") != 0)
        return TRUE;
    ++scan->matches;
    if (!scan->slot) {
        scan->slot = iatSlot;
        scan->original = *iatSlot;
    }
    return TRUE;
}

bool LocateUxThemeDrawTextExImport() noexcept {
    if (g_uxthemeDrawTextExImportFound) return true;
    if (!g_uxThemeModule) {
        g_uxthemeDrawTextExImportStatus = ERROR_MOD_NOT_FOUND;
        return false;
    }
    UxThemeDrawTextImportScan scan;
    SetLastError(ERROR_SUCCESS);
    const BOOL enumerated = DetourEnumerateImportsEx(
        g_uxThemeModule,&scan,UxThemeImportFileCallback,UxThemeImportFuncCallbackEx);
    if (!enumerated) {
        const DWORD error = GetLastError();
        g_uxthemeDrawTextExImportStatus = error ? static_cast<LONG>(error) : ERROR_BAD_EXE_FORMAT;
        return false;
    }
    if (!scan.slot || !scan.original) {
        g_uxthemeDrawTextExImportStatus = ERROR_PROC_NOT_FOUND;
        return false;
    }
    g_uxthemeDrawTextExImportSlot = scan.slot;
    g_originalDrawTextExW = reinterpret_cast<DrawTextExWFn>(scan.original);
    g_uxthemeDrawTextExImportFound = true;
    g_uxthemeDrawTextExImportStatus = NO_ERROR;
    InstallLog::LineFormat(L"UxTheme DrawTextExW IAT target found: slot=%p original=%p matches=%u",
        g_uxthemeDrawTextExImportSlot,reinterpret_cast<PVOID>(g_originalDrawTextExW),scan.matches);
    return true;
}

bool ExchangeUxThemeDrawTextExImport(PVOID expected, PVOID replacement, LONG& status) noexcept {
    status = NO_ERROR;
    if (!g_uxthemeDrawTextExImportSlot || !expected || !replacement) {
        status = ERROR_INVALID_PARAMETER;
        return false;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(g_uxthemeDrawTextExImportSlot,sizeof(PVOID),PAGE_READWRITE,&oldProtection)) {
        status = static_cast<LONG>(GetLastError());
        return false;
    }
    const PVOID previous = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(g_uxthemeDrawTextExImportSlot),replacement,expected);
    const bool swapped = previous == expected || previous == replacement;
    DWORD ignoredProtection = 0;
    if (!VirtualProtect(g_uxthemeDrawTextExImportSlot,sizeof(PVOID),oldProtection,&ignoredProtection) &&
        status == NO_ERROR)
        status = static_cast<LONG>(GetLastError());
    if (!swapped && status == NO_ERROR) status = ERROR_BUSY;
    return swapped;
}

bool BeginDrawTextExHook() noexcept {
    if (!g_uxthemeDrawTextExImportFound || !g_uxthemeDrawTextExImportSlot ||
        !g_originalDrawTextExW) {
        g_uxthemeDrawTextExImportStatus = ERROR_PROC_NOT_FOUND;
        return false;
    }
    if (g_drawTextExHookDepth) {
        ++g_drawTextExHookDepth;
        return true;
    }
    const PVOID hook = reinterpret_cast<PVOID>(&HookDrawTextExW);
    const PVOID original = reinterpret_cast<PVOID>(g_originalDrawTextExW);
    LONG status = NO_ERROR;
    if (!ExchangeUxThemeDrawTextExImport(original,hook,status)) {
        g_uxthemeDrawTextExImportStatus = status;
        return false;
    }
    g_drawTextExHookOwned = true;
    g_drawTextExHookDepth = 1;
    g_uxthemeDrawTextExImportStatus = status;
    return true;
}

void EndDrawTextExHook() noexcept {
    if (!g_drawTextExHookDepth) return;
    if (--g_drawTextExHookDepth) return;
    if (!g_drawTextExHookOwned) return;
    const PVOID hook = reinterpret_cast<PVOID>(&HookDrawTextExW);
    const PVOID original = reinterpret_cast<PVOID>(g_originalDrawTextExW);
    LONG status = NO_ERROR;
    if (!ExchangeUxThemeDrawTextExImport(hook,original,status)) {
        g_uxthemeDrawTextExImportStatus = status;
        InstallLog::LineFormat(L"UxTheme DrawTextExW IAT restore failed: status=%ld",
                              g_uxthemeDrawTextExImportStatus);
        return;
    }
    g_drawTextExHookOwned = false;
    g_uxthemeDrawTextExImportStatus = status;
}

class ScopedThemeTextHook {
    std::unique_lock<std::recursive_mutex> lock_;
    ThemeTextCallState previous_{};
    bool active_ = false;
public:
    explicit ScopedThemeTextHook(const ThemeTextCallState& state)
        : lock_(g_drawThemeTextHookMutex), previous_(g_themeTextCallState) {
        g_themeTextCallState = state;
        active_ = BeginDrawTextExHook();
        if (!active_) g_themeTextCallState = previous_;
    }
    ScopedThemeTextHook(const ScopedThemeTextHook&) = delete;
    ScopedThemeTextHook& operator=(const ScopedThemeTextHook&) = delete;
    ~ScopedThemeTextHook() {
        if (!active_) return;
        g_themeTextCallState = previous_;
        EndDrawTextExHook();
    }
};

HRESULT WINAPI HookDrawThemeText(HTHEME theme, HDC dc, int part, int state,
                                 LPCWSTR text, int textLength, DWORD textFlags,
                                 DWORD textFlags2, LPCRECT textRect) {
    const auto original = reinterpret_cast<DrawThemeTextFn>(g_originalDrawThemeText);
    if (!original) return E_UNEXPECTED;
    HWND button = g_dtbCurrentButton;
    const bool validButton = button && IsWindow(button);
    const DWORD buttonType = validButton ?
        (static_cast<DWORD>(GetWindowLongPtrW(button,GWL_STYLE)) & BS_TYPEMASK) : 0xFFFFFFFFu;
    const bool ourPushButton = validButton && GetAncestor(button,GA_ROOT) == gRootWindow &&
        (buttonType == BS_PUSHBUTTON || buttonType == BS_DEFPUSHBUTTON);
    const bool nestedThemeText = g_themeTextCallState.active;
    if ((part != BP_PUSHBUTTON || !ourPushButton) && !nestedThemeText)
        return original(theme,dc,part,state,text,textLength,textFlags,textFlags2,textRect);

    HWND trackedButton = ourPushButton ? button : g_themeTextCallState.button;
    const ThemeTextCallState drawState{part,state,trackedButton,
                                       IsActiveFocusedButton(trackedButton),true};
#ifdef _DEBUG
    const unsigned stateIndex=static_cast<unsigned>(state)&7u;
    const unsigned logBit=1u << (stateIndex+(drawState.focusedBlue?8u:0u));
    if (!(g_drawThemeTextStateLogMask.fetch_or(logBit,std::memory_order_relaxed)&logBit))
        InstallLog::LineFormat(L"UxTheme DrawThemeText button state: hwnd=%p part=%d state=%d focusedBlue=%d",
            trackedButton,part,state,drawState.focusedBlue?1:0);
#endif
    ScopedThemeTextHook scopedHook(drawState);
    (void)scopedHook; // Keep the scoped IAT interception active across the original themed draw.
    return original(theme,dc,part,state,text,textLength,textFlags,textFlags2,textRect);
}

HRESULT WINAPI HookDrawThemeBackground(HTHEME theme, HDC dc, int part, int state,
                                       LPCRECT rect, LPCRECT clip) {
    const auto original = reinterpret_cast<DrawThemeBackgroundFn>(g_originalDrawThemeBackground);
    if (!original) return E_UNEXPECTED;
    wchar_t themeClass[MAX_PATH]{L"?"};
    const HRESULT classHr = theme ? Shared::ThemeHelper::GetThemeClass(theme,themeClass,MAX_PATH) : E_INVALIDARG;
    const bool buttonTheme = SUCCEEDED(classHr) && _wcsicmp(themeClass,L"Button") == 0;
#ifdef _DEBUG
    if (!g_dtbAnyCallLogged.exchange(true,std::memory_order_relaxed))
        InstallLog::LineFormat(L"DTB Detour reached: class=%s classHr=0x%08X part=%d state=%d",
            themeClass,static_cast<unsigned>(classHr),part,state);
    if (buttonTheme && !g_dtbButtonCallLogged.exchange(true,std::memory_order_relaxed))
        InstallLog::LineFormat(L"Button DTB found: theme=%p part=%d state=%d rect=[%d,%d,%d,%d]",
            theme,part,state,rect?rect->left:-1,rect?rect->top:-1,
            rect?rect->right:-1,rect?rect->bottom:-1);
#endif
    if (buttonTheme && part == BP_PUSHBUTTON && dc && rect && !IsRectEmpty(rect)) {
        HWND button = g_dtbCurrentButton;
        if (!button || GetAncestor(button,GA_ROOT) != gRootWindow)
            button = WindowFromDC(dc);
        const bool activeFocus = button && button == GetFocus() &&
            IsWindowEnabled(button) && GetAncestor(button,GA_ROOT) == gRootWindow &&
            GetForegroundWindow() == gRootWindow;
        if (g_buttonImageCache.Contains(rect->right-rect->left,rect->bottom-rect->top,
                                        state,activeFocus)) {
            PaintFooterBackdropForButton(dc,button,rect,clip);
            if (g_buttonImageCache.Draw(dc,rect,clip,state,activeFocus)) {
#ifdef _DEBUG
                if (!g_dtbCacheHitLogged.exchange(true,std::memory_order_relaxed))
                    InstallLog::LineFormat(L"Button DTB cached image: state=%d palette=%s size=%dx%d",
                        state,activeFocus?L"focused-blue":L"neutral",
                        rect->right-rect->left,rect->bottom-rect->top);
#endif
                return S_OK;
            }
        }
    }
    return original(theme,dc,part,state,rect,clip);
}

bool InstallThemeDetours() noexcept {
    if (!g_uxThemeModule) {
        g_uxThemeModule=LoadLibraryW(L"uxtheme.dll");
        if (!g_uxThemeModule) {
            g_themeDetourStatus=static_cast<LONG>(GetLastError());
            g_uxthemeDrawTextExImportStatus=g_themeDetourStatus;
            return false;
        }
    }
    if (!g_originalDrawThemeBackground)
        g_originalDrawThemeBackground=reinterpret_cast<PVOID>(GetProcAddress(g_uxThemeModule,"DrawThemeBackground"));
    if (!g_originalDrawThemeText)
        g_originalDrawThemeText=reinterpret_cast<PVOID>(GetProcAddress(g_uxThemeModule,"DrawThemeText"));
    if (!g_originalDrawThemeBackground || !g_originalDrawThemeText) {
        g_themeDetourStatus=static_cast<LONG>(GetLastError());
        if (g_themeDetourStatus == NO_ERROR) g_themeDetourStatus=ERROR_PROC_NOT_FOUND;
    }
    const bool importFound=LocateUxThemeDrawTextExImport();
    if (!g_themeDetoursInstalled && g_originalDrawThemeBackground && g_originalDrawThemeText) {
        DetourSetIgnoreTooSmall(TRUE);
        LONG status=DetourTransactionBegin();
        const bool transactionStarted=status==NO_ERROR;
        if (status==NO_ERROR) status=DetourUpdateThread(GetCurrentThread());
        if (status==NO_ERROR)
            status=DetourAttach(&g_originalDrawThemeBackground,
                                reinterpret_cast<PVOID>(HookDrawThemeBackground));
        if (status==NO_ERROR)
            status=DetourAttach(&g_originalDrawThemeText,
                                reinterpret_cast<PVOID>(HookDrawThemeText));
        if (status!=NO_ERROR) {
            g_themeDetourStatus=status;
            if (transactionStarted) DetourTransactionAbort();
        } else {
            status=DetourTransactionCommit();
            g_themeDetourStatus=status;
            g_themeDetoursInstalled=status==NO_ERROR;
        }
    }
    return g_themeDetoursInstalled && importFound;
}

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

bool ReadInstallRootFromRegistry(HKEY hive, const wchar_t* keyPath, REGSAM view,
                                 std::wstring& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(hive,keyPath,0,KEY_READ|view,&key) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    std::wstring value;
    const bool ok = ReadRegistryString(key,L"InstallPath",value,&type);
    RegCloseKey(key);
    if (!ok) return false;
    if (type == REG_EXPAND_SZ) {
        const DWORD needed = ExpandEnvironmentStringsW(value.c_str(),nullptr,0);
        if (!needed || needed > 32768) return false;
        std::wstring expanded(needed,L'\0');
        if (!ExpandEnvironmentStringsW(value.c_str(),expanded.data(),needed)) return false;
        expanded.resize(wcslen(expanded.c_str()));
        value=std::move(expanded);
    }
    while (!value.empty() && (value.back()==L'\\' || value.back()==L'/')) value.pop_back();
    out=std::move(value);
    return !out.empty();
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
    const REGSAM views[] = {KEY_WOW64_64KEY,KEY_WOW64_32KEY};
    for (REGSAM view : views) {
        std::wstring path;
        if (ReadInstallRootFromRegistry(HKEY_CURRENT_USER,L"Software\\GlassPlatform",view,path))
            AddInstallRoot(roots,std::move(path),true);
    }
    wchar_t program64[MAX_PATH]{};
    DWORD chars=GetEnvironmentVariableW(L"ProgramW6432",program64,MAX_PATH);
    if (chars && chars<MAX_PATH) {
        AddInstallRoot(roots,std::wstring(program64)+L"\\GlassPlatform",false);
        AddInstallRoot(roots,std::wstring(program64)+L"\\Glass",false);
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
    for (REGSAM view : {KEY_WOW64_64KEY,KEY_WOW64_32KEY}) {
        HKEY key=nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\GlassPlatform",0,
                          KEY_READ|view,&key)==ERROR_SUCCESS) {
            RegCloseKey(key);
            return true;
        }
    }
    return false;
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
        PostMessageW(window, kInitWebView, 0, 0);
        return true;
    }
private:
    Page page_ = Page::Welcome;
    UINT dpi_ = 96;
    HWND back_ = nullptr, next_ = nullptr, cancel_ = nullptr;
    HWND updateDownload_ = nullptr, updateSkip_ = nullptr;
    HWND eula_ = nullptr, accept_ = nullptr, progress_ = nullptr, webHost_ = nullptr;
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
    bool debugDemoPending_ = true, debugDemoStarted_ = false, debugDemoActive_ = false;
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
        for (HWND button : {back_,next_,cancel_,updateDownload_,updateSkip_}) {
            if (button && !SetWindowSubclass(button,GlassButtonSubclassProc,kButtonSubclassId,0))
                InstallLog::LineFormat(L"Could not subclass push button %p (%lu)",
                    button,static_cast<unsigned long>(GetLastError()));
        }
    }
    void BuildButtonImageCache() {
        HDC reference=GetDC(window);
        if (!reference) {
            InstallLog::LineFormat(L"Button image cache skipped: GetDC failed (%lu)",
                                   static_cast<unsigned long>(GetLastError()));
            return;
        }
        const std::vector<int> widths={Scale(96),Scale(104),Scale(160)};
        const bool built=g_buttonImageCache.Build(reference,widths,Scale(32));
        ReleaseDC(window,reference);
        InstallLog::LineFormat(L"Cached push-button image states %s (%zu entries, DPI %u)",
            built?L"ready":L"unavailable",built?g_buttonImageCache.EntryCount():0u,dpi_);
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
        for (HWND c : {back_,next_,cancel_,eula_,accept_,updateDownload_,updateSkip_})
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
        Layout();
        AttachButtonSubclasses();
        BuildButtonImageCache();
        SetPage(Page::Welcome);
    }
    void Layout() {
        Move(back_,644,kContentHeight+12,96,32);
        // Complete uses the exact rectangle previously occupied by Cancel.
        const bool unlock = page_==Page::ProcessesStuck;
        const bool conflictCheck = page_==Page::Conflicts;
        const bool cleanOffer = page_==Page::VersionInvalid && offerCleanInstall_;
        // «Продолжить» в Segoe UI 14px — 88 px; 132 px даёт поля по 22 px.
        const int nextWidth = (unlock || conflictCheck) ? 160 : (cleanOffer ? 132 : 104);
        const int nextX = (page_==Page::Complete || page_==Page::Error) ? 864 :
            ((unlock || conflictCheck) ? 694 : (cleanOffer ? 722 : 750));
        Move(next_,nextX,kContentHeight+12,nextWidth,32);
        Move(cancel_,864,kContentHeight+12,104,32);
        Move(updateDownload_,644,kContentHeight+12,104,32);
        Move(updateSkip_,750,kContentHeight+12,104,32);
        Move(eula_,48,142,904,285);
        Move(accept_,48,447,904,32);
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
        const bool webVisible = SlidesVisible();
        const bool nativeLicense = !webVisible && page_==Page::License && !transitioning_;
        ShowWindow(eula_,nativeLicense?SW_SHOWNA:SW_HIDE);
        ShowWindow(accept_,nativeLicense?SW_SHOWNA:SW_HIDE);
        const bool showProgress = !transitioning_ && (page_==Page::Installing || page_==Page::Complete);
        ShowWindow(progress_, showProgress ? SW_SHOWNA : SW_HIDE);
        const bool update = page_==Page::Update && !transitioning_;
        ShowWindow(updateDownload_,update?SW_SHOWNA:SW_HIDE);
        ShowWindow(updateSkip_,update?SW_SHOWNA:SW_HIDE);
        if (controller_) controller_->put_IsVisible(webVisible ? TRUE : FALSE);
        ShowWindow(webHost_,webVisible?SW_SHOWNA:SW_HIDE);
    }
    [[nodiscard]] bool SlidesVisible() const noexcept {
        // One WebView2 child stays visible for every HTML page. Page changes are
        // DOM state changes, never child-window hide/show or document navigation.
        return webReady_;
    }
    [[nodiscard]] bool DebugDemoLocksControls() const noexcept {
#ifdef _DEBUG
        return debugDemoPending_ || debugDemoActive_;
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
        case Page::Installing:
            if (visualMode_==VisualMode::UnlockModules) return L"unlock-modules";
            if (visualMode_==VisualMode::UnlockProcesses) return L"unlock-processes";
            return L"installing";
        case Page::ModulesBusy: return L"unlock-modules";
        case Page::ProcessesStuck: return L"unlock-processes";
        case Page::Complete: return L"complete";
        case Page::Error: return L"error";
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
    void SyncWebViewState() {
        if (!web_ || !webReady_ || !webBridgeReady_) return;
#ifdef GLASS_EMBED_MSI
        const UINT doneBodyId = IDS_DONE_BODY, doneNoteId = IDS_DONE_NOTE;
#else
        const UINT doneBodyId = IDS_DONE_BODY_DEMO, doneNoteId = IDS_DONE_NOTE_DEMO;
#endif
        std::wstring labels=L"{\"brand\":" + JsStringLiteral(Text(IDS_BRAND)) +
            L",\"licenseTitle\":" + JsStringLiteral(Text(IDS_EULA_TITLE)) +
            L",\"licenseIntro\":" + JsStringLiteral(Text(IDS_EULA_BODY)) +
            L",\"licenseAccept\":" + JsStringLiteral(Text(IDS_ACCEPT)) +
            L",\"updateTag\":" + JsStringLiteral(Text(IDS_UPDATE_TAG)) +
            L",\"updateTitle\":" + JsStringLiteral(Text(IDS_UPDATE_TITLE)) +
            L",\"updateReady\":" + JsStringLiteral(Text(IDS_UPDATE_READY)) +
            L",\"updateCurrentLabel\":" + JsStringLiteral(Text(IDS_UPDATE_CURRENT)) +
            L",\"updateLatestLabel\":" + JsStringLiteral(Text(IDS_UPDATE_LATEST)) +
            L",\"conflictsTag\":" + JsStringLiteral(Text(IDS_CONFLICTS_TAG)) +
            L",\"conflictsTitle\":" + JsStringLiteral(Text(IDS_CONFLICTS_TITLE)) +
            L",\"conflictsBody\":" + JsStringLiteral(Text(IDS_CONFLICTS_BODY)) +
            L",\"conflictsInstructions\":" + JsStringLiteral(Text(IDS_CONFLICTS_INSTRUCTIONS)) +
            L",\"downgradeTag\":" + JsStringLiteral(Text(IDS_DOWNGRADE_TAG)) +
            L",\"downgradeTitle\":" + JsStringLiteral(Text(IDS_DOWNGRADE_TITLE)) +
            L",\"versionInvalidTag\":" + JsStringLiteral(Text(IDS_VERSION_INVALID_TAG)) +
            L",\"versionInvalidTitle\":" + JsStringLiteral(Text(IDS_VERSION_INVALID_TITLE)) +
            L",\"downgradeNote\":" + JsStringLiteral(Text(IDS_DOWNGRADE_NOTE)) +
            L",\"versionInvalidNote\":" + JsStringLiteral(Text(offerCleanInstall_ ? IDS_VERSION_INVALID_NOTE : IDS_SETUP_VERSION_INVALID_NOTE)) +
            L",\"versionLabel\":" + JsStringLiteral(Text(IDS_INSTALLED_VERSION_LABEL)) +
            L",\"setupVersionLabel\":" + JsStringLiteral(Text(IDS_SETUP_VERSION_LABEL)) +
            L",\"doneTag\":" + JsStringLiteral(Text(IDS_DONE_TAG)) +
            L",\"doneTitle\":" + JsStringLiteral(Text(IDS_DONE_TITLE)) +
            L",\"doneBody\":" + JsStringLiteral(Text(doneBodyId)) +
            L",\"doneNote\":" + JsStringLiteral(Text(doneNoteId)) +
            L",\"doneReady\":" + JsStringLiteral(Text(IDS_DONE_READY)) +
            L",\"errorTag\":" + JsStringLiteral(Text(IDS_ERROR_TAG)) +
            L",\"errorTitle\":" + JsStringLiteral(Text(IDS_ERROR_TITLE)) +
            L",\"errorBody\":" + JsStringLiteral(Text(IDS_ERROR_BODY)) +
            L",\"errorDetails\":" + JsStringLiteral(Text(IDS_ERROR_DETAILS)) +
            L",\"errorNote\":" + JsStringLiteral(Text(IDS_ERROR_LOG_HINT)) + L"}";
        std::wstring json=L"{\"type\":\"state\",\"page\":" + JsStringLiteral(WebPageName()) +
            L",\"locale\":" + JsStringLiteral(PRIMARYLANGID(gLanguage)==LANG_RUSSIAN?L"ru":L"en") +
            L",\"accepted\":" + (accepted_?L"true":L"false") +
            L",\"eula\":" + JsStringLiteral(page_==Page::License?eulaText_:std::wstring()) +
            L",\"platformVersion\":" + JsStringLiteral(platformVersion_) +
            L",\"updateCurrent\":" + JsStringLiteral(updateCurrent_) +
            L",\"updateLatest\":" + JsStringLiteral(updateLatest_) +
            L",\"updateBody\":" + JsStringLiteral(updateBody_) +
            L",\"conflicts\":" + ConflictsJson() +
            L",\"installedGlassVersion\":" + JsStringLiteral(installedGlassVersion_) +
            L",\"setupProductVersion\":" + JsStringLiteral(setupProductVersion_) +
            L",\"blockReason\":" + JsStringLiteral(blockReason_) +
            L",\"downgradeBody\":" + JsStringLiteral(downgradeBody_) +
            L",\"errorReason\":" + JsStringLiteral(errorReason_) +
            L",\"status\":" + JsStringLiteral(installStatusLine_) +
            L",\"percent\":" + std::to_wstring(percent_) +
            L",\"progress\":" + std::to_wstring(progressValue_) +
            L",\"marquee\":" + (progressMarquee_?L"true":L"false") +
            L",\"unlockActive\":" +
                ((page_==Page::Installing &&
                  (visualMode_==VisualMode::UnlockModules || visualMode_==VisualMode::UnlockProcesses))
                    ? L"true" : L"false") +
            L",\"holdReport\":" + JsStringLiteral(holdReport_) +
            L",\"ui\":" + labels + L"}";
        const HRESULT hr=web_->PostWebMessageAsJson(json.c_str());
        if (FAILED(hr)) InstallLog::LineFormat(L"WebView2 state message failed: 0x%08X",
                                               static_cast<unsigned>(hr));
    }
    void FocusPage() {
        if (webReady_ && page_==Page::License) { SetFocus(webHost_); return; }
        SetFocus(page_==Page::ModulesBusy || page_==Page::ProcessesStuck || page_==Page::Conflicts ? next_ :
                 page_==Page::Downgrade || (page_==Page::VersionInvalid && !offerCleanInstall_) ? cancel_ :
                 page_==Page::Update ? updateSkip_ :
                 page_==Page::Checking ? cancel_ :
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
            (nativeLoading || page_==Page::Update || page_==Page::Error))
            SetTimer(window,kHeroTimer,kHeroTimerMs,nullptr);
        else
            KillTimer(window,kHeroTimer);
        BOOL darkCaption=TRUE;
        if (FAILED(DwmSetWindowAttribute(window,20,&darkCaption,sizeof(darkCaption))))
            DwmSetWindowAttribute(window,19,&darkCaption,sizeof(darkCaption));
        ShowWindow(eula_,SW_HIDE); ShowWindow(accept_,SW_HIDE);
        Layout();
        const bool cleanOffer=page_==Page::VersionInvalid && offerCleanInstall_;
        const UINT nextId = page_==Page::Conflicts ? IDS_RECHECK_CONFLICTS :
            cleanOffer ? IDS_CONTINUE :
            page_==Page::License ? IDS_INSTALL :
            (page_==Page::Complete || page_==Page::Error) ? IDS_FINISH :
            page_==Page::ModulesBusy ? IDS_HOLD_RETRY :
            page_==Page::ProcessesStuck ? IDS_UNLOCK : IDS_NEXT;
        SetWindowTextW(next_,Text(nextId).c_str());
        SetWindowTextW(cancel_,Text((page_==Page::Downgrade || (page_==Page::VersionInvalid && !offerCleanInstall_))?IDS_CLOSE:IDS_CANCEL).c_str());
        const bool blockedPage=page_==Page::Downgrade || (page_==Page::VersionInvalid && !offerCleanInstall_);
        const bool demoControlsLocked=DebugDemoLocksControls();
        const int defaultId=demoControlsLocked?IDOK:
            page_==Page::Update?IDC_UPDATE_SKIP:blockedPage?IDC_CANCEL:IDC_NEXT;
        SendMessageW(window,DM_SETDEFID,defaultId,0);
        const bool demoFinishButton =
#ifdef _DEBUG
            debugDemoActive_ && (page_==Page::Complete || page_==Page::Error);
#else
            false;
#endif
        const bool demoLocked = demoControlsLocked && !demoFinishButton;
        const bool cancelSlotTaken=page_==Page::Complete || page_==Page::Error;
        ShowWindow(back_,page_==Page::License?SW_SHOWNA:SW_HIDE);
        ShowWindow(next_,(page_==Page::Checking || page_==Page::Installing || page_==Page::Update || blockedPage)?SW_HIDE:SW_SHOWNA);
        ShowWindow(cancel_,!cancelSlotTaken && !demoControlsLocked?SW_SHOWNA:SW_HIDE);
        EnableWindow(next_,!demoLocked && (page_!=Page::License || accepted_));
        EnableWindow(updateDownload_,!demoLocked);
        EnableWindow(updateSkip_,!demoLocked);
        if (!webReady_ &&
            ((oldPage==Page::Welcome && (page_==Page::Checking || page_==Page::License)) ||
             (oldPage==Page::Checking && (page_==Page::Update || page_==Page::License)) ||
             (oldPage==Page::Installing && (page_==Page::Complete || page_==Page::Error))))
            BeginTransition();
        ShowContentControls();
        EnableWindow(back_,!transitioning_ && !DebugDemoLocksControls());
        EnableWindow(next_,!failurePending_ && !transitioning_ && !demoLocked && (page_!=Page::License || accepted_));
        if (!transitioning_) FocusPage(); else if (page_==Page::License) SetFocus(cancel_);
        SyncWebViewState();
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
        labels.rollbackFailed = Text(IDS_ERR_ROLLBACK_FAILED);
        labels.rollbackRestored = Text(IDS_ERR_ROLLBACK_RESTORED);
        labels.rollbackRemoved = Text(IDS_ERR_ROLLBACK_REMOVED);
        labels.nativeScanFailed = Text(IDS_ERR_NATIVE_SCAN);
        labels.snapshotFailed = Text(IDS_ERR_SNAPSHOT);
        labels.diskFull = Text(IDS_MSI_DISK_FULL);
        installEngine_.Start(gInstance, x64Dir, x86Dir, ogDir, true, forceCleanInstall_, std::move(labels), nullptr);
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
        if (debugDemoStarted_ || !webReady_ || !webBridgeReady_) return;
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
            Page::Conflicts,Page::Downgrade,Page::VersionInvalid,Page::License,
            Page::Installing,Page::ModulesBusy,Page::ProcessesStuck,Page::Complete,Page::Error};
        constexpr size_t count=sizeof(sequence)/sizeof(sequence[0]);
        if (!debugDemoActive_) { KillTimer(window,kDebugDemoTimer); return; }
        if (debugDemoStep_+1>=count) { FinishDebugDemo(); return; }
        ++debugDemoStep_;
        SetDebugDemoPage(sequence[debugDemoStep_]);
    }
#endif
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
        if (page_ == Page::Installing && installEngine_.TakeErrorPageRequest()) {
            ShowFailurePage(false);
            installEngine_.NotifyErrorPageShown();
            return;
        }
        if (page_ == Page::Error) {
            if (installEngine_.Poll()) {
                ShowFailurePage(true);
                return;
            }
            installStatusLine_ = installEngine_.StatusLine();
            SyncWebViewState();
            return;
        }
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
            progressReverse_ = installEngine_.IsProgressReverse();
            progressMarquee_ = installEngine_.IsMarquee() && !progressFinishing_;
            if (progressMarquee_) {
                // Фаза от абсолютного времени — без накопления ошибки таймера (как в имитации установки).
                marqueePhase_ = static_cast<double>((now - started_) % kMarqueePeriodMs) /
                    static_cast<double>(kMarqueePeriodMs);
            } else {
                if (progressFinishing_)
                    progressTarget_ = progressReverse_ ? 0 : 10000;
                else
                    progressTarget_ = std::clamp(installEngine_.ProgressUnits(), 0, 10000);
                const double alpha = std::min(1.0, static_cast<double>(dt) / kProgressSmoothTauMs);
                displayProgress_ += static_cast<int>(std::lround(
                    (progressTarget_ - displayProgress_) * alpha));
                progressValue_ = displayProgress_;
                percent_ = std::min(100, progressReverse_
                    ? (10000 - displayProgress_) / 100
                    : displayProgress_ / 100);
            }
            installStatusLine_ = installEngine_.StatusLine();
            if (page_==Page::Installing && visualMode_!=VisualMode::Slideshow) {
                if (installStatusLine_ == unlockStatusLine_) {
                    unlockStatusObserved_ = true;
                } else if (unlockStatusObserved_ && now >= unlockVisualUntil_) {
                    visualMode_ = VisualMode::Slideshow;
                    unlockStatusLine_.clear();
                    unlockVisualUntil_ = 0;
                    unlockStatusObserved_ = false;
                    SyncWebViewState();
                }
            }
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
                ShowFailurePage(true);
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
#ifdef _DEBUG
        if (debugDemoActive_ || debugDemoPending_) {
            KillTimer(window,kDebugDemoTimer);
            debugDemoActive_=false;
            debugDemoPending_=false;
            DestroyWindow(window);
            return;
        }
#endif
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
        webBridgeReady_ = false;
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
        if (message==L"ready") {
            webBridgeReady_=true;
            SyncWebViewState();
#ifdef _DEBUG
            MaybeStartDebugDemo();
#endif
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
        if (page_ == Page::Complete) {
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
        if (page_!=Page::Installing) return;
        const std::wstring line=installStatusLine_.empty()?Text(IDS_STATUS_PREPARE):installStatusLine_;
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
        if (versionCheckThread_.joinable()) versionCheckThread_.join();
        frame_.Reset(); fadeFrame_.Reset();
        if (web_ && webMessageToken_.value) web_->remove_WebMessageReceived(webMessageToken_);
        if (controller_) controller_->Close();
        web_.Reset(); controller_.Reset(); environment_.Reset();
        banner_.reset(); bannerStream_.Reset();
        for (HWND button : {back_,next_,cancel_,updateDownload_,updateSkip_})
            if (button && IsWindow(button)) RemoveWindowSubclass(button,GlassButtonSubclassProc,kButtonSubclassId);
        g_buttonImageCache.Reset();
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
            // Footer push-button captions are colored in the scoped DrawTextExW detour.
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
                    ShellExecuteW(window, L"open", updateUrl_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                return 0;
            case IDC_UPDATE_SKIP:
                if (!DebugDemoLocksControls() && page_==Page::Update) {
                    InstallLog::Line(L"Update skipped; continuing with this setup");
                    updateSkipped_ = true;
                    SetPage(Page::License);
                }
                return 0;
            case IDC_BACK:
                if (!DebugDemoLocksControls() && !transitioning_ && page_==Page::License) {
                    forceCleanInstall_=false;
                    SetPage(Page::Welcome);
                }
                return 0;
            case IDOK:
            case IDC_NEXT:
#ifdef _DEBUG
                if (debugDemoPending_) return 0;
                if (debugDemoActive_) {
                    if (page_==Page::Complete || page_==Page::Error) RequestClose();
                    return 0;
                }
#endif
                if (transitioning_) return 0;
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
                else if (page_==Page::License && accepted_) StartInstallation();
                else if (page_==Page::Complete || page_==Page::Error) RequestClose();
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
                       (page_==Page::Welcome || page_==Page::Update || page_==Page::Error)) {
                InvalidateContent();
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
            gRootWindow=hwnd;
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        const LRESULT result=self ? self->Handle(msg,w,l) : DefWindowProcW(hwnd,msg,w,l);
        if (msg==WM_NCDESTROY && gRootWindow==hwnd) gRootWindow=nullptr;
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
        const std::wstring message=Text(IDS_COM_ERROR);
        MessageBoxW(nullptr,message.c_str(),Text(IDS_TITLE).c_str(),MB_OK|MB_ICONERROR);
        InstallLog::Shutdown();
        return 4;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    const RuntimeGateResult runtimeGate=EnsureWebView2Runtime(HasWebView2RestartSwitch());
    if (runtimeGate!=RuntimeGateResult::Ready) {
        if (SUCCEEDED(com)) CoUninitialize();
        InstallLog::Shutdown();
        return runtimeGate==RuntimeGateResult::Cancelled ||
               runtimeGate==RuntimeGateResult::Restarted ? 0 : 6;
    }
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput graphicsInput;
    if (Gdiplus::GdiplusStartup(&gdiplusToken,&graphicsInput,nullptr)!=Gdiplus::Ok) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 3;
    }
    // Keep themed backgrounds, but scope DrawTextExW interception to each DrawThemeText call.
    InstallThemeDetours();
    InstallLog::LineFormat(L"UxTheme DrawThemeBackground/DrawThemeText Detours %s: status=%ld background=%p text=%p module=%p",
        g_themeDetoursInstalled?L"installed":L"FAILED",g_themeDetourStatus,
        g_originalDrawThemeBackground,g_originalDrawThemeText,g_uxThemeModule);
    InstallLog::LineFormat(L"UxTheme DrawTextExW IAT target %s: status=%ld slot=%p original=%p",
        g_uxthemeDrawTextExImportFound?L"found":L"NOT FOUND",g_uxthemeDrawTextExImportStatus,
        g_uxthemeDrawTextExImportSlot,reinterpret_cast<PVOID>(g_originalDrawTextExW));
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
    Gdiplus::GdiplusShutdown(gdiplusToken);
    InstallLog::Shutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    return exitCode;
}
