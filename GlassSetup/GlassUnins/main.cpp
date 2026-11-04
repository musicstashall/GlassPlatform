// GlassPlatform selective uninstaller. The original executable relaunches from an
// administrator-only temporary directory before any MSI is removed.
#include "InstallSupport/GlassInstallHost.h"
#include "InstallSupport/GlassInstallLog.h"
#include "InstallSupport/GlassInstallMsi.h"
#include "InstallSupport/GlassInstallRemoval.h"
#include "InstallSupport/GlassInstallUnlock.h"
#include "InstallSupport/GlassInstallUtil.h"
#include "InstallSupport/GlassInstallWatch.h"
#include "WizardTheme.h"
#include "WizardAssets.h"
#include "GlassProducts.h"
#include <GlassShellMenuConstants.h>
#include <RegistrySanity.h>

#include <windows.h>
#include <commctrl.h>
#include "GdiPlusCompat.h"
#include <msi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace GP = GlassShared::GlassPlatformTask;
namespace Msi = GlassInstallMsi;

namespace {
constexpr int kWidth=1000, kHtmlHeight=540, kContentHeight=544, kHeight=600;
constexpr COLORREF kDark=RGB(16,18,37), kFooterTop=RGB(26,27,50), kFooterBottom=RGB(17,18,35);
constexpr UINT kWebInit=WM_APP+1, kWorkerStatus=WM_APP+2, kWorkerHold=WM_APP+3, kWorkerFinished=WM_APP+4;
constexpr int kSelectAll=1100;
constexpr UINT_PTR kTimer=11;
HINSTANCE gInstance=nullptr;
LANGID gLanguage=MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US);

bool IsRu() noexcept { return PRIMARYLANGID(gLanguage)==LANG_RUSSIAN; }
const wchar_t* T(const wchar_t* ru, const wchar_t* en) noexcept { return IsRu()?ru:en; }
std::wstring Q(const std::wstring& text) { return L"\""+WizardTheme::JsonEscape(text)+L"\""; }

void LogLine(const std::wstring& line) noexcept {
    const std::wstring directory=GlassInstallLog::SecureLogDirectory();
    if (directory.empty()) return;
    const std::wstring path=directory+L"\\GlassUnins.log";
    HANDLE file=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return;
    const std::string utf8=WizardTheme::WideToUtf8(line+L"\r\n");
    DWORD written=0;
    WriteFile(file,utf8.data(),static_cast<DWORD>(utf8.size()),&written,nullptr);
    CloseHandle(file);
}

struct ResourceData { const BYTE* data=nullptr; DWORD size=0; };
ResourceData Resource(UINT id) noexcept {
    const HRSRC r=FindResourceExW(gInstance,RT_RCDATA,MAKEINTRESOURCEW(id),MAKELANGID(LANG_NEUTRAL,SUBLANG_NEUTRAL));
    if (!r) return {};
    HGLOBAL h=LoadResource(gInstance,r);
    return {static_cast<const BYTE*>(LockResource(h)),SizeofResource(gInstance,r)};
}

bool SameCode(const std::wstring& code,const wchar_t* expected) noexcept {
    return expected && _wcsicmp(code.c_str(),expected)==0;
}
bool IsKnownCode(const std::wstring& code) noexcept {
    return SameCode(code,Msi::kProductPlatformX64) || SameCode(code,Msi::kProductPlatformX86) ||
           SameCode(code,Msi::kProductSettingsX64) || SameCode(code,Msi::kProductSettingsX86) ||
           SameCode(code,Msi::kProductSecureUxThemeX64) || SameCode(code,Msi::kProductSecureUxThemeX86) ||
           SameCode(code,Msi::kProductOpenGlass);
}

struct Arguments {
    bool fromTemp=false;
    bool silent=false;
    int launchError=0;
    std::wstring originalDir;
    std::wstring product;
};
Arguments ParseArguments() {
    Arguments a;
    int count=0;
    LPWSTR* args=CommandLineToArgvW(GetCommandLineW(),&count);
    if (!args) return a;
    for (int i=1;i<count;++i) {
        if (_wcsicmp(args[i],L"/fromtemp")==0 && i+1<count) { a.fromTemp=true; a.originalDir=args[++i]; }
        else if (_wcsicmp(args[i],L"--product")==0 && i+1<count) a.product=args[++i];
        else if (_wcsicmp(args[i],L"--silent")==0) a.silent=true;
    }
    LocalFree(args);
    return a;
}

std::wstring ProductFromDat(const std::wstring& dir) {
    const std::wstring path=GlassInstallUtil::TrimTrailingSlashes(dir)+L"\\unins000.dat";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file,&size) || size.QuadPart<=0 || size.QuadPart>8192) { CloseHandle(file); return {}; }
    std::string bytes(static_cast<size_t>(size.QuadPart),'\0');
    DWORD read=0;
    const BOOL ok=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr);
    CloseHandle(file);
    if (!ok || !read) return {};
    bytes.resize(read);
    if (bytes.size()>=3 && static_cast<unsigned char>(bytes[0])==0xef &&
        static_cast<unsigned char>(bytes[1])==0xbb && static_cast<unsigned char>(bytes[2])==0xbf)
        bytes.erase(0,3);
    const std::string key="PRODUCT_CODE=";
    auto pos=bytes.find(key);
    while (pos!=std::string::npos && pos!=0 && bytes[pos-1]!='\n' && bytes[pos-1]!='\r')
        pos=bytes.find(key,pos+1);
    if (pos==std::string::npos) return {};
    std::string value=bytes.substr(pos+key.size());
    auto eol=value.find_first_of("\r\n");
    if (eol!=std::string::npos) value.resize(eol);
    while (!value.empty() && (value.back()==' ' || value.back()=='\t')) value.pop_back();
    if (value.empty()) return {};
    if (value.front()!='{') value.insert(value.begin(),'{');
    if (value.back()!='}') value.push_back('}');
    return WizardTheme::Utf8ToWide(value);
}

// Временная копия деинсталлятора живёт в %WINDIR%\Temp\GlassUnins-<guid>. Каталог
// создаётся GlassInstallUtil::CreateSecureTempDirectory, поэтому проверка «мы внутри
// своей копии» — это строгое совпадение префикса и отсутствие вложенности: чужие
// каталоги, %TEMP% пользователя и Program Files не удаляются никогда.
bool IsPrivateTempDirectory(const std::wstring& dir) noexcept {
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows,MAX_PATH)) return false;
    const std::wstring prefix=std::wstring(windows)+L"\\Temp\\GlassUnins-";
    if (dir.size()<=prefix.size()) return false;
    if (_wcsnicmp(dir.c_str(),prefix.c_str(),prefix.size())!=0) return false;
    return dir.find(L'\\',prefix.size())==std::wstring::npos;
}

// Убирает копии, оставшиеся от прошлых запусков. Файл, который ещё кем-то занят
// (второй деинсталлятор работает), не удаляется — его снимет его собственный запуск.
void CleanupStalePrivateCopies() noexcept {
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows,MAX_PATH)) return;
    const std::wstring temp=std::wstring(windows)+L"\\Temp";
    WIN32_FIND_DATAW data{};
    HANDLE find=FindFirstFileW((temp+L"\\GlassUnins-*").c_str(),&data);
    if (find==INVALID_HANDLE_VALUE) return;
    int removed=0,kept=0;
    do {
        if (!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(data.cFileName,L".")==0 || wcscmp(data.cFileName,L"..")==0) continue;
        const std::wstring dir=temp+L"\\"+data.cFileName;
        if (!DeleteFileW((dir+L"\\unins000.exe").c_str())) { ++kept; continue; }
        if (RemoveDirectoryW(dir.c_str())) ++removed; else ++kept;
    } while (FindNextFileW(find,&data));
    FindClose(find);
    if (removed||kept)
        LogLine(L"Stale private copies: removed="+std::to_wstring(removed)+
                L" busy="+std::to_wstring(kept));
}

// Свою копию работающий процесс удалить не может, поэтому удаление откладывается на
// короткоживущий скрытый cmd: он ждёт выхода этого процесса и убирает файл и каталог.
// Ветка запускается только для собственной копии в %WINDIR%\Temp\GlassUnins-<guid>.
void SchedulePrivateCopyRemoval() noexcept {
    wchar_t module[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr,module,MAX_PATH)) return;
    const std::wstring exe=module;
    const size_t slash=exe.find_last_of(L'\\');
    if (slash==std::wstring::npos) return;
    const std::wstring dir=exe.substr(0,slash);
    if (!IsPrivateTempDirectory(dir)) return;
    // Полные пути: поиск по PATH в каталоге, куда мы сами себя скопировали, не используется.
    wchar_t systemDir[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDir,MAX_PATH)) return;
    const std::wstring cmd=std::wstring(systemDir)+L"\\cmd.exe";
    const std::wstring ping=std::wstring(systemDir)+L"\\ping.exe";
    // /s — cmd снимает ровно одну внешнюю пару кавычек, поэтому внутренние остаются как есть.
    std::wstring command=L"\""+cmd+L"\" /d /s /c \""+ping+
        L"\" -n 4 127.0.0.1 >nul & del /f /q \""+exe+L"\" & rmdir \""+dir+L"\"\"";
    std::vector<wchar_t> buffer(command.begin(),command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW si{}; si.cb=sizeof(si);
    si.dwFlags=STARTF_USESHOWWINDOW; si.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr,buffer.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) {
        LogLine(L"Private copy cleanup could not start, error "+std::to_wstring(GetLastError()));
        return;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    LogLine(L"Private copy cleanup scheduled "+dir);
}

int RelaunchFromPrivateTemp(const Arguments& a) noexcept {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr,executable,MAX_PATH)) return 1;
    wchar_t sourceDir[MAX_PATH]{};
    if (wcscpy_s(sourceDir,executable)!=0 || !PathRemoveFileSpecW(sourceDir)) return 1;
    const std::wstring product=a.product.empty()?ProductFromDat(sourceDir):a.product;
    if (!product.empty() && !IsKnownCode(product)) { LogLine(L"Unknown product identifier"); return 4; }
    CleanupStalePrivateCopies();
    std::wstring temp;
    if (!GlassInstallUtil::CreateSecureTempDirectory(L"GlassUnins",temp)) { LogLine(L"Cannot create secure temporary directory"); return 5; }
    const std::wstring dest=temp+L"\\unins000.exe";
    if (!CopyFileW(executable,dest.c_str(),TRUE)) { RemoveDirectoryW(temp.c_str()); LogLine(L"Cannot copy uninstaller"); return 6; }
    std::wstring command=L"\""+dest+L"\" /fromtemp \""+std::wstring(sourceDir)+L"\"";
    if (!product.empty()) command+=L" --product \""+product+L"\"";
    if (a.silent) command+=L" --silent";
    STARTUPINFOW si{}; si.cb=sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(dest.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,temp.c_str(),&si,&pi)) {
        LogLine(L"Could not launch temporary uninstaller");
        DeleteFileW(dest.c_str()); RemoveDirectoryW(temp.c_str()); return 7;
    }
    CloseHandle(pi.hThread);
    if (a.silent) {
        WaitForSingleObject(pi.hProcess,INFINITE);
        DWORD exitCode=1; GetExitCodeProcess(pi.hProcess,&exitCode);
        CloseHandle(pi.hProcess);
        DeleteFileW(dest.c_str()); RemoveDirectoryW(temp.c_str());
        return static_cast<int>(exitCode);
    }
    CloseHandle(pi.hProcess);
    return 0;
}

struct Selection {
    bool x64=false, x86=false, settings=false, openGlass=false, secureUxTheme=false;
    // Не продукт: очистка настроек текущего пользователя по явному выбору.
    bool cleanupSettings=false;
    bool Any() const noexcept { return x64 || x86 || settings || openGlass || secureUxTheme; }
};
enum class Page { Select, Stopping, UnlockModules, UnlockProcesses, Removing, Cancelled, Complete, Error };
enum class Action { None, Retry, Unlock, Cancel };
enum class OperationState { BeforeRemoval, CancelRequested, Removing, Finished };

class Uninstaller {
public:
    HWND window=nullptr;
    explicit Uninstaller(Arguments args): args_(std::move(args)) {}
    ~Uninstaller() { if (worker_.joinable()) worker_.join(); if (gate_) CloseHandle(gate_); }

    int RunSilent() {
        DetectInstalled(); ResolveRoots();
        if (!args_.product.empty() && !IsKnownCode(args_.product)) return 4;
        ChooseInitialProduct();
        NormalizeSelection();
        if (!selected_.Any()) return 0;
        return RunOperation(selected_);
    }

    bool Create(int show, bool alreadyRunning) {
        alreadyRunning_=alreadyRunning;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc=WindowProc; wc.hInstance=gInstance;
        wc.hIcon=LoadIconW(gInstance,MAKEINTRESOURCEW(IDI_APP)); wc.hIconSm=wc.hIcon;
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        brush_=CreateSolidBrush(kDark); buttonBrush_=CreateSolidBrush(RGB(24,25,47));
        wc.hbrBackground=brush_; wc.lpszClassName=L"GlassUninsWindow";
        if (!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
        registered_=true;
        dpi_=GetDpiForSystem();
        constexpr DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN;
        RECT r{0,0,Scale(kWidth),Scale(kHeight)};
        AdjustWindowRectExForDpi(&r,style,FALSE,WS_EX_CONTROLPARENT,dpi_);
        const int width=r.right-r.left,height=r.bottom-r.top;
        POINT cursor{}; GetCursorPos(&cursor);
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTOPRIMARY),&mi);
        const int x=static_cast<int>(mi.rcWork.left+std::max<LONG>(0,(mi.rcWork.right-mi.rcWork.left-width)/2));
        const int y=static_cast<int>(mi.rcWork.top+std::max<LONG>(0,(mi.rcWork.bottom-mi.rcWork.top-height)/2));
        window=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,
            T(L"GlassPlatform — Удаление",L"GlassPlatform — Uninstall"),style,x,y,width,height,nullptr,nullptr,gInstance,this);
        if (!window) return false;
        ShowWindow(window,show); UpdateWindow(window);
        if (!alreadyRunning_) PostMessageW(window,kWebInit,0,0);
        return true;
    }

    int MessageLoop() {
        MSG m{}; BOOL result;
        while ((result=GetMessageW(&m,nullptr,0,0))>0) {
            if (!IsDialogMessageW(window,&m)) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
        return result<0?5:static_cast<int>(m.wParam);
    }

private:
    Arguments args_;
    Selection installed_{}, selected_{};
    GlassInstallUtil::SecureUxThemeState secureUxThemeState_=GlassInstallUtil::SecureUxThemeState::Unavailable;
    Page page_=Page::Select;
    UINT dpi_=96;
    bool registered_=false, alreadyRunning_=false, closing_=false;
    bool webReady_=false, bridgeReady_=false, initSent_=false, webFailed_=false;
    HWND back_=nullptr,next_=nullptr,cancel_=nullptr,webHost_=nullptr;
    std::array<HWND,6> check_{};
    HFONT font_=nullptr, heading_=nullptr;
    HBRUSH brush_=nullptr,buttonBrush_=nullptr;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> web_;
    EventRegistrationToken requestToken_{}, navToken_{}, completedToken_{}, messageToken_{}, popupToken_{}, permissionToken_{};
    std::wstring webState_;
    std::wstring x64Dir_,x86Dir_,openGlassDir_;
    bool rootsKnown_=false;
    std::mutex dataMutex_;
    std::wstring status_,error_,holdReport_;
    int progress_=0, resultCode_=0;
    HANDLE gate_=nullptr;
    std::thread worker_;
    std::atomic_bool cancelRequested_{false}, removalStarted_{false};
    std::atomic<OperationState> operationState_{OperationState::BeforeRemoval};
    std::atomic<Action> action_{Action::None};

    int Scale(int n) const noexcept { return MulDiv(n,static_cast<int>(dpi_),96); }
    RECT Box(int x,int y,int w,int h) const noexcept { return {Scale(x),Scale(y),Scale(x+w),Scale(y+h)}; }
    void Move(HWND h,int x,int y,int w,int hgt) {
        if (h) SetWindowPos(h,nullptr,Scale(x),Scale(y),Scale(w),Scale(hgt),SWP_NOZORDER|SWP_NOACTIVATE);
    }
    HWND Button(const wchar_t* text,int id,DWORD style=BS_PUSHBUTTON) {
        return CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_TABSTOP|style,0,0,0,0,window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),gInstance,nullptr);
    }
    void CreateFonts() {
        if (font_) DeleteObject(font_); if (heading_) DeleteObject(heading_);
        font_=CreateFontW(-Scale(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        heading_=CreateFontW(-Scale(30),0,0,0,FW_LIGHT,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        for (HWND h:{back_,next_,cancel_,check_[0],check_[1],check_[2],check_[3],check_[4],check_[5]})
            if (h) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
    }
    void DetectInstalled() noexcept {
        installed_.x64=Msi::IsProductInstalled(Msi::kProductPlatformX64);
        installed_.x86=Msi::IsProductInstalled(Msi::kProductPlatformX86);
        installed_.settings=Msi::IsProductInstalled(Msi::kProductSettingsX64) || Msi::IsProductInstalled(Msi::kProductSettingsX86);
        installed_.openGlass=Msi::IsProductInstalled(Msi::kProductOpenGlass);
        installed_.secureUxTheme=Msi::IsProductInstalled(Msi::kProductSecureUxThemeX64) ||
            Msi::IsProductInstalled(Msi::kProductSecureUxThemeX86);
    }
    void ChooseInitialProduct() noexcept {
        if (SameCode(args_.product,Msi::kProductPlatformX64)) selected_.x64=installed_.x64;
        else if (SameCode(args_.product,Msi::kProductPlatformX86)) selected_.x86=installed_.x86;
        else if (SameCode(args_.product,Msi::kProductSettingsX64) || SameCode(args_.product,Msi::kProductSettingsX86)) selected_.settings=installed_.settings;
        else if (SameCode(args_.product,Msi::kProductOpenGlass)) selected_.openGlass=installed_.openGlass;
        else if (SameCode(args_.product,Msi::kProductSecureUxThemeX64) || SameCode(args_.product,Msi::kProductSecureUxThemeX86))
            selected_.secureUxTheme=installed_.secureUxTheme;
    }
    bool CoreSelected() const noexcept { return GlassInstallUtil::IsOs64Bit()?selected_.x64:selected_.x86; }
    bool SecureUxThemeManaged() const noexcept {
        return secureUxThemeState_==GlassInstallUtil::SecureUxThemeState::Active ||
            secureUxThemeState_==GlassInstallUtil::SecureUxThemeState::ActivationPending ||
            secureUxThemeState_==GlassInstallUtil::SecureUxThemeState::DeactivationPending;
    }
    void NormalizeSelection() noexcept {
        const GlassProducts::Selection chosen{selected_.x64,selected_.x86,selected_.settings,selected_.openGlass,selected_.secureUxTheme};
        const GlassProducts::Selection installed{installed_.x64,installed_.x86,installed_.settings,installed_.openGlass,installed_.secureUxTheme};
        const auto safe=GlassProducts::NormalizeRemoval(chosen,installed,GlassInstallUtil::IsOs64Bit());
        selected_={safe.platform64,safe.platform32,safe.glassSettings,safe.openGlass,
                   safe.secureUxTheme,selected_.cleanupSettings};
        if (!installed_.secureUxTheme) selected_.secureUxTheme=false;
        else if (selected_.settings || CoreSelected()) selected_.secureUxTheme=true;
    }
    void ResolveRoots() {
        GlassInstallUtil::InstallRoots roots;
        rootsKnown_=GlassInstallUtil::ResolveInstallRoots(roots);
        if (!rootsKnown_) {
            x64Dir_.clear(); x86Dir_.clear(); openGlassDir_.clear();
            LogLine(L"Installation roots unavailable, error "+std::to_wstring(GetLastError()));
            return;
        }
        x64Dir_=roots.x64; x86Dir_=roots.x86; openGlassDir_=roots.openGlass;
        if(installed_.settings || installed_.secureUxTheme){
            std::wstring sutError;
            if(!GlassInstallUtil::QuerySecureUxThemeState(x64Dir_,x86Dir_,&secureUxThemeState_,&sutError)){
                secureUxThemeState_=GlassInstallUtil::SecureUxThemeState::Conflict;
                LogLine(L"SecureUxTheme state query failed: "+sutError);
            }
            installed_.secureUxTheme=installed_.secureUxTheme||SecureUxThemeManaged();
        }
        NormalizeSelection();
        LogLine(L"Roots x64="+x64Dir_+L" ("+roots.sourceX64+L") x86="+x86Dir_+L" ("+
            roots.sourceX86+L") OpenGlass="+openGlassDir_+L" ("+roots.sourceOpenGlass+L")");
    }

    void Initialize() {
        dpi_=GetDpiForWindow(window);
        DetectInstalled(); ChooseInitialProduct(); NormalizeSelection(); ResolveRoots();
        gate_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        back_=Button(T(L"Выбрать всё",L"Select all"),kSelectAll);
        next_=Button(T(L"Удалить",L"Remove"),IDC_NEXT);
        cancel_=Button(T(L"Отмена",L"Cancel"),IDC_CANCEL);
        check_[0]=Button(L"Glass Platform (64-bit)",IDC_COMPONENT64,BS_AUTOCHECKBOX);
        check_[1]=Button(L"Glass Platform (32-bit)",IDC_COMPONENT32,BS_AUTOCHECKBOX);
        check_[2]=Button(L"GlassSettings",IDC_COMPONENT_SETTINGS,BS_AUTOCHECKBOX);
        check_[3]=Button(L"OpenGlass",IDC_COMPONENT_OG,BS_AUTOCHECKBOX);
        check_[4]=Button(T(L"Удалить настройки",L"Remove settings"),IDC_UNINS_SETTINGS_CLEANUP,BS_AUTOCHECKBOX);
        check_[5]=Button(L"SecureUxTheme",IDC_UNINS_SECUREUXTHEME,BS_AUTOCHECKBOX);
        webHost_=CreateWindowExW(WS_EX_CONTROLPARENT,L"STATIC",L"",WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
            0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_WEBHOST)),gInstance,nullptr);
        CreateFonts();
        SetTimer(window,kTimer,40,nullptr);
        for (HWND h:{back_,next_,cancel_}) WizardTheme::AttachButtonScope(h);
        BuildCache();
        if (args_.launchError) {
            resultCode_=args_.launchError;
            error_=std::wstring(T(L"Не удалось подготовить временную копию деинсталлятора. Код: ",
                L"Could not prepare the temporary uninstaller. Code: "))+std::to_wstring(args_.launchError);
            SetPage(Page::Error);
        } else if (alreadyRunning_) {
            resultCode_=ERROR_INSTALL_ALREADY_RUNNING;
            error_=T(L"Другой установщик или деинсталлятор GlassPlatform уже запущен.",L"Another GlassPlatform setup or uninstaller is already running.");
            SetPage(Page::Error);
        } else if (!gate_ || (!args_.product.empty() && !IsKnownCode(args_.product))) {
            error_=T(L"Неизвестный продукт или ошибка инициализации.",L"Unknown product or initialization error.");
            SetPage(Page::Error);
        } else SetPage(Page::Select);
    }
    void BuildCache() {
        HDC dc=GetDC(window);
        if (!dc) return;
        WizardTheme::PrewarmButtonCache(dc,{Scale(104),Scale(160),Scale(184)},Scale(32));
        ReleaseDC(window,dc);
    }
    void Layout() {
        Move(webHost_,0,0,kWidth,kHtmlHeight);
        Move(back_,644,kContentHeight+12,160,32);
        Move(next_,page_==Page::Select?814:750,kContentHeight+12,page_==Page::Select?130:104,32);
        Move(cancel_,864,kContentHeight+12,104,32);
        if (page_==Page::Select) { Move(next_,750,kContentHeight+12,104,32); Move(back_,574,kContentHeight+12,160,32); }
        if (page_==Page::Complete || page_==Page::Cancelled || page_==Page::Error) Move(next_,864,kContentHeight+12,104,32);
        Move(check_[0],60,203,390,32); Move(check_[1],510,203,390,32);
        Move(check_[2],60,293,390,32); Move(check_[3],510,293,390,32);
        Move(check_[5],60,373,390,32); Move(check_[4],60,417,840,32);
        if (controller_) { RECT bounds=Box(0,0,kWidth,kHtmlHeight); controller_->put_Bounds(bounds); }
    }
    // R13: нативные чекбоксы — только резерв на случай, когда WebView2 недоступен.
    // Пока контент грузится, в тех же местах рисуется индикатор ожидания: раньше
    // пользователь видел четыре белых прямоугольника на месте карточек продуктов.
    void ShowNativeChecks() {
        const bool show=page_==Page::Select && webFailed_;
        const bool core=CoreSelected();
        const bool values[]={selected_.x64,selected_.x86,selected_.settings,selected_.openGlass,selected_.cleanupSettings,selected_.secureUxTheme};
        const bool available[]={installed_.x64,installed_.x86,installed_.settings,installed_.openGlass,true,installed_.secureUxTheme};
        for (int i=0;i<6;++i) {
            SendMessageW(check_[i],BM_SETCHECK,values[i]?BST_CHECKED:BST_UNCHECKED,0);
            ShowWindow(check_[i],show && available[i]?SW_SHOWNA:SW_HIDE);
            const bool dependent=(i<4 && core && ((GlassInstallUtil::IsOs64Bit() && i!=0) || (!GlassInstallUtil::IsOs64Bit() && i!=1)))
                || (i==5 && (core||selected_.settings));
            EnableWindow(check_[i],available[i] && !dependent);
        }
    }
    const wchar_t* WebPage() const noexcept {
        switch (page_) {
        case Page::Select: return L"uninstall-select";
        case Page::Stopping: case Page::Removing: return L"uninstalling";
        case Page::UnlockModules: return L"unlock-modules";
        case Page::UnlockProcesses: return L"unlock-processes";
        case Page::Cancelled: return L"cancelled";
        case Page::Complete: return L"complete";
        case Page::Error: return L"error";
        }
        return L"error";
    }
    std::wstring ComponentsJson() const {
        std::wstring json=L"[";
        const bool core=CoreSelected();
        const bool os64=GlassInstallUtil::IsOs64Bit();
        auto add=[&](const wchar_t* id,const wchar_t* title,const wchar_t* detail,bool checked,bool available,bool locked) {
            if (json.size()>1) json+=L",";
            const std::wstring badge=locked?T(L"ЗАВИСИМОСТЬ",L"DEPENDENT"):T(L"УСТАНОВЛЕНО",L"INSTALLED");
            // Строка «что будет выполнено» — по фактическим действиям удаления;
            // причина объясняет зависимость (каскад основной платформы).
            const std::wstring state=checked?T(L"Будет удалено",L"Will be removed"):T(L"Останется без изменений",L"Will be kept as is");
            std::wstring reasons=L"[";
            if (locked) reasons+=Q(T(L"удалится вместе с основной платформой",L"removed together with the core platform"));
            reasons+=L"]";
            json+=L"{\"id\":"+Q(id)+L",\"title\":"+Q(title)+L",\"description\":"+Q(detail)+
                L",\"badge\":"+Q(badge)+L",\"state\":"+Q(state)+L",\"reasons\":"+reasons+
                L",\"checked\":"+(checked?L"true":L"false")+
                L",\"locked\":"+(locked?L"true":L"false")+L",\"hidden\":"+(available?L"false":L"true")+L"}";
        };
        add(L"x64",L"Glass Platform (64-bit)",T(L"Платформа 64-битных приложений",L"64-bit platform"),selected_.x64,installed_.x64,core&&!os64);
        add(L"x86",L"Glass Platform (32-bit)",T(L"Поддержка 32-битных приложений",L"32-bit application support"),selected_.x86,installed_.x86,core&&os64);
        add(L"settings",L"GlassSettings",T(L"Панель управления платформой",L"Platform settings application"),selected_.settings,installed_.settings,core);
        add(L"openglass",L"OpenGlass",T(L"Оформление рамок окон",L"Glass window frame engine"),selected_.openGlass,installed_.openGlass,core);
        if(installed_.secureUxTheme){
            const bool active=SecureUxThemeManaged();
            const bool pending=secureUxThemeState_==GlassInstallUtil::SecureUxThemeState::ActivationPending||secureUxThemeState_==GlassInstallUtil::SecureUxThemeState::DeactivationPending;
            const bool external=secureUxThemeState_==GlassInstallUtil::SecureUxThemeState::External;
            const bool dependent=(core||selected_.settings)&&installed_.secureUxTheme;
            const bool checked=selected_.secureUxTheme||dependent;
            const bool disabled=external||secureUxThemeState_==GlassInstallUtil::SecureUxThemeState::Conflict;
            const std::wstring badge=external?T(L"ВНЕШНЯЯ УСТАНОВКА",L"EXTERNAL INSTALL"):pending?T(L"ПЕРЕЗАПУСК",L"RESTART PENDING"):active?T(L"АКТИВЕН",L"ACTIVE"):T(L"УСТАНОВЛЕН",L"INSTALLED");
            const std::wstring state=external?T(L"Управляется внешней установкой",L"Managed by an external installation"):
                pending?T(L"Состояние изменится после перезапуска",L"State change takes effect after restart"):
                checked?T(L"Пакет SecureUxTheme будет удалён",L"SecureUxTheme package will be removed"):
                active?T(L"Останется без изменений",L"Will remain unchanged"):
                T(L"Установлен, интеграция выключена",L"Installed, integration is off");
            const std::wstring reason=dependent?T(L"удалится вместе с GlassSettings",L"removed with GlassSettings"):
                T(L"можно удалить отдельно",L"can be removed separately");
            json+=L",{\"id\":\"secureUxTheme\",\"title\":\"SecureUxTheme\",\"description\":"+
                Q(T(L"Отдельный SecureUxTheme MSI, зависящий от GlassSettings.",L"Standalone SecureUxTheme MSI that depends on GlassSettings."))+
                L",\"badge\":"+Q(badge)+L",\"badgeTone\":"+Q(external?L"repair":pending?L"update":L"keep")+
                L",\"state\":"+Q(state)+L",\"reasons\":"+(reason.empty()?L"[]":L"["+Q(reason)+L"]")+
                L",\"checked\":"+(checked?L"true":L"false")+L",\"locked\":"+(dependent?L"true":L"false")+
                L",\"lockTitle\":"+Q(dependent?T(L"Удаляется вместе с GlassSettings",L"Removed with GlassSettings"):L"")+
                L",\"disabled\":"+(disabled?L"true":L"false")+L",\"hidden\":false}";
        }
        // «Удалить настройки» — отдельный компактный чекбокс, а не карточка.
        json+=L",{\"id\":\"cleanup\",\"slim\":true,\"title\":"+Q(T(L"Удалить настройки",L"Remove settings"))+
            L",\"description\":"+Q(T(L"Раздел GlassPlatform и параметры OpenGlass в DWM текущего пользователя.",
                L"The GlassPlatform key and the OpenGlass DWM values of the current user."))+
            L",\"checked\":"+(selected_.cleanupSettings?L"true":L"false")+L",\"locked\":false,\"hidden\":false}";
        return json+L"]";
    }
    std::wstring Warning() const {
        std::wstring text;
        const auto append=[&](const std::wstring& value){if(!text.empty())text+=L"\n";text+=value;};
        if (CoreSelected()) append(T(L"При удалении основной платформы зависимые продукты также будут удалены, а SecureUxTheme будет отключён.",L"Removing the core platform also removes dependent products and disables SecureUxTheme."));
        else if(selected_.settings&&(installed_.x64||installed_.x86)) append(T(L"GlassSettings и SecureUxTheme будут отключены. Без GlassSettings параметры платформы придётся менять вручную в HKCU\\Software\\StanislavKa\\GlassPlatform.",
            L"GlassSettings and SecureUxTheme will be disabled. Without GlassSettings, platform settings must be edited manually in HKCU\\Software\\StanislavKa\\GlassPlatform."));
        if(secureUxThemeState_==GlassInstallUtil::SecureUxThemeState::External)
            append(T(L"Внешняя установка SecureUxTheme останется без изменений.",L"The external SecureUxTheme installation will be left unchanged."));
        else if(secureUxThemeState_==GlassInstallUtil::SecureUxThemeState::Conflict)
            append(T(L"Состояние SecureUxTheme не удалось проверить; его интеграция не будет изменена.",L"SecureUxTheme could not be inspected; its integration will not be changed."));
        return text;
    }
    void SyncWeb() {
        if (!web_ || !webReady_ || !bridgeReady_) return;
        if (!initSent_) {
            const std::wstring ui=L"{\"brand\":\"GlassPlatform\",\"compTag\":"+Q(T(L"ВЫБОРОЧНОЕ УДАЛЕНИЕ",L"SELECTIVE UNINSTALL"))+
                L",\"compTitle\":"+Q(T(L"Выберите продукты для удаления",L"Select products to remove"))+
                L",\"compIntro\":"+Q(T(L"Будут удалены только отмеченные продукты.",L"Only selected products will be removed."))+
                L",\"uninsTag\":"+Q(T(L"УДАЛЕНИЕ КОМПОНЕНТОВ",L"UNINSTALLING COMPONENTS"))+
                L",\"uninsTitle\":"+Q(T(L"Удаляем выбранные продукты",L"Removing selected products"))+
                L",\"uninsBody\":"+Q(T(L"Останавливаем затронутые модули и удаляем выбранные пакеты.",L"Stopping affected modules and removing the selected packages."))+
                L",\"uninsSubstatus\":"+Q(T(L"Дождитесь окончания операции",L"Please wait for the operation to finish"))+
                L",\"doneTag\":"+Q(T(L"УДАЛЕНИЕ ЗАВЕРШЕНО",L"UNINSTALL COMPLETE"))+
                L",\"doneTitle\":"+Q(T(L"Готово",L"Finished"))+
                L",\"doneBody\":"+Q(T(L"Выбранные продукты удалены.",L"The selected products have been removed."))+
                L",\"doneReady\":"+Q(T(L"ГОТОВО",L"DONE"))+
                L",\"doneNote\":"+Q(T(L"Журнал: %WINDIR%\\Temp\\GlassUninsLogs-*\\GlassUnins.log",L"Log: %WINDIR%\\Temp\\GlassUninsLogs-*\\GlassUnins.log"))+
                L",\"cancelledTag\":"+Q(T(L"УДАЛЕНИЕ ОТМЕНЕНО",L"UNINSTALL CANCELLED"))+
                L",\"cancelledTitle\":"+Q(T(L"Удаление отменено",L"Uninstall cancelled"))+
                L",\"cancelledBody\":"+Q(T(L"Пакеты не удалялись.",L"No packages were removed."))+
                L",\"errorTag\":"+Q(T(L"ОШИБКА",L"ERROR"))+
                L",\"errorTitle\":"+Q(T(L"Удаление не завершено",L"Uninstall did not finish"))+
                L",\"errorBody\":"+Q(T(L"Подробности приведены ниже.",L"Details are shown below."))+
                L",\"errorDetails\":"+Q(T(L"ДЕТАЛИ",L"DETAILS"))+
                L",\"errorNote\":"+Q(T(L"Журнал: %WINDIR%\\Temp\\GlassUninsLogs-*\\GlassUnins.log",L"Log: %WINDIR%\\Temp\\GlassUninsLogs-*\\GlassUnins.log"))+L"}";
            const std::wstring init=L"{\"type\":\"init\",\"appMode\":\"unins\",\"locale\":"+Q(IsRu()?L"ru":L"en")+L",\"ui\":"+ui+L"}";
            if (FAILED(web_->PostWebMessageAsJson(init.c_str()))) return;
            initSent_=true;
        }
        std::wstring status,error,report; int progress;
        { std::lock_guard<std::mutex> lock(dataMutex_); status=status_; error=error_; report=holdReport_; progress=progress_; }
        const std::wstring json=L"{\"type\":\"state\",\"page\":"+Q(WebPage())+
            L",\"components\":"+ComponentsJson()+L",\"componentsWarning\":"+Q(Warning())+
            L",\"status\":"+Q(status)+L",\"errorReason\":"+Q(error)+L",\"holdReport\":"+Q(report)+
            L",\"unlockActive\":false,\"progress\":"+std::to_wstring(progress)+L"}";
        if (json==webState_) return;
        if (SUCCEEDED(web_->PostWebMessageAsJson(json.c_str()))) webState_=json;
    }
    void SetPage(Page page) {
        page_=page;
        const bool select=page==Page::Select;
        const bool hold=page==Page::UnlockModules || page==Page::UnlockProcesses;
        const bool terminal=page==Page::Cancelled || page==Page::Complete || page==Page::Error;
        const bool busy=page==Page::Stopping || page==Page::Removing;
        WizardTheme::ApplyDarkWindowChrome(window);
        WizardTheme::SetSystemCloseEnabled(window,!(page==Page::Removing));
        SetWindowTextW(back_,T(L"Выбрать всё",L"Select all"));
        SetWindowTextW(next_,select?T(L"Удалить",L"Remove"):
            page==Page::UnlockModules?T(L"Повторить",L"Retry"):
            page==Page::UnlockProcesses?T(L"Разблокировать",L"Unlock"):T(L"Закрыть",L"Close"));
        ShowWindow(back_,select?SW_SHOWNA:SW_HIDE);
        ShowWindow(next_,!busy?SW_SHOWNA:SW_HIDE);
        ShowWindow(cancel_,!terminal && page!=Page::Removing?SW_SHOWNA:SW_HIDE);
        EnableWindow(next_,!select || selected_.Any()||selected_.cleanupSettings);
        EnableWindow(cancel_,!cancelRequested_.load());
        if (hold) SendMessageW(window,DM_SETDEFID,IDC_NEXT,0);
        Layout(); ShowNativeChecks();
        if (controller_) controller_->put_IsVisible(webReady_?TRUE:FALSE);
        ShowWindow(webHost_,webReady_?SW_SHOWNA:SW_HIDE);
        SyncWeb(); InvalidateRect(window,nullptr,FALSE);
    }
    void SetStatus(const std::wstring& status,int progress) {
        { std::lock_guard<std::mutex> lock(dataMutex_); status_=status; progress_=progress; }
        LogLine(status);
        if (window) PostMessageW(window,kWorkerStatus,0,0);
    }
    void SetError(const std::wstring& text) {
        { std::lock_guard<std::mutex> lock(dataMutex_); error_=text; }
        LogLine(text);
    }
    Action AskUser(Page page,const std::wstring& report) {
        if (args_.silent || !gate_) return Action::Cancel;
        ResetEvent(gate_);
        action_.store(Action::None);
        { std::lock_guard<std::mutex> lock(dataMutex_); holdReport_=report; }
        if (cancelRequested_.load()) return Action::Cancel;
        PostMessageW(window,kWorkerHold,static_cast<WPARAM>(page),0);
        WaitForSingleObject(gate_,INFINITE);
        return cancelRequested_.load()?Action::Cancel:action_.load();
    }
    bool EnsureModulesReleased() {
        bool nativeFailed=false;
        auto holders=GlassInstallUnlock::CollectHolders(x64Dir_,x86Dir_,GlassInstallUtil::IsOs64Bit(),nativeFailed,&cancelRequested_);
        if (nativeFailed) { SetError(T(L"Не удалось проверить модули в процессах.",L"Could not check modules in processes.")); return false; }
        if (holders.empty()) return true;
        const bool clear=GlassInstallWatch::WaitUntilClearOrTimeout(30000,x64Dir_,x86Dir_,GlassInstallUtil::IsOs64Bit(),holders,nativeFailed,&cancelRequested_);
        if (cancelRequested_.load()) return false;
        if (nativeFailed) { SetError(T(L"Не удалось проверить модули в процессах.",L"Could not check modules in processes.")); return false; }
        if (clear && holders.empty()) return true;
        for (;;) {
            holders=GlassInstallUnlock::CollectHolders(x64Dir_,x86Dir_,GlassInstallUtil::IsOs64Bit(),nativeFailed,&cancelRequested_);
            if (nativeFailed) { SetError(T(L"Не удалось проверить модули в процессах.",L"Could not check modules in processes.")); return false; }
            if (holders.empty()) return true;
            Action choice=AskUser(Page::UnlockModules,GlassInstallUnlock::FormatHolders(holders));
            if (choice==Action::Cancel) { cancelRequested_.store(true); return false; }
            for (const auto& h:holders) (void)GlassInstallUnlock::ForceUnloadHolder(h,x64Dir_,x86Dir_);
            if (cancelRequested_.load()) return false;
            holders=GlassInstallUnlock::CollectHolders(x64Dir_,x86Dir_,GlassInstallUtil::IsOs64Bit(),nativeFailed,&cancelRequested_);
            if (nativeFailed) { SetError(T(L"Не удалось проверить модули в процессах.",L"Could not check modules in processes.")); return false; }
            if (holders.empty()) return true;
            choice=AskUser(Page::UnlockProcesses,GlassInstallUnlock::FormatHolders(holders));
            if (choice==Action::Cancel) { cancelRequested_.store(true); return false; }
            for (const auto& h:holders) GlassInstallUnlock::UnlockProcess(h.pid,h.exe.c_str());
            if (cancelRequested_.load()) return false;
        }
    }
    int RunOperation(Selection chosen) {
        GlassInstallUtil::InstallRoots current;
        const bool rootsNow=GlassInstallUtil::ResolveInstallRoots(current);
        const bool same64=(current.x64.empty() && x64Dir_.empty()) ||
            GlassInstallUtil::SameInstallPath(current.x64,x64Dir_);
        const bool sameOg=(current.openGlass.empty() && openGlassDir_.empty()) ||
            GlassInstallUtil::SameInstallPath(current.openGlass,openGlassDir_);
        if (!rootsKnown_ || !rootsNow || !same64 || !sameOg ||
            !GlassInstallUtil::SameInstallPath(current.x86,x86Dir_)) {
            LogLine(L"Installation roots unavailable or changed; removal stopped");
            SetError(T(L"Не удалось определить неизменившиеся пути установки. Пакеты не удалены; сохраните журнал.",
                L"Could not resolve unchanged installation paths. No packages were removed; save the log."));
            return 9;
        }
        // MsiConfigureProductEx uninstalls the MSI's registered file paths,
        // not our cleanup directory. Inspect selected products before stopping
        // any host; the agreed safety check prevents touching paths outside
        // the corresponding InstallPath.
        struct Probe { const wchar_t* code; std::wstring root, cached; bool installed=false; };
        struct ProbeWorkspace {
            std::wstring directory;
            std::vector<std::wstring> files;
            ~ProbeWorkspace() { for (const auto& file:files) DeleteFileW(file.c_str()); if (!directory.empty()) RemoveDirectoryW(directory.c_str()); }
        } workspace;
        std::vector<Probe> probes;
        if (chosen.x64) probes.push_back({Msi::kProductPlatformX64,x64Dir_,{}});
        if (chosen.x86) probes.push_back({Msi::kProductPlatformX86,x86Dir_,{}});
        if (chosen.settings) {
            probes.push_back({Msi::kProductSettingsX64,x64Dir_,{}});
            probes.push_back({Msi::kProductSettingsX86,x86Dir_,{}});
        }
        if (chosen.secureUxTheme) {
            probes.push_back({Msi::kProductSecureUxThemeX64,x64Dir_,{}});
            probes.push_back({Msi::kProductSecureUxThemeX86,x86Dir_,{}});
        }
        if (chosen.openGlass) probes.push_back({Msi::kProductOpenGlass,openGlassDir_,{}});
        SetStatus(T(L"Проверка путей установленных продуктов…",L"Checking installed product paths…"),100);
        if (!GlassInstallUtil::CreateSecureTempDirectory(L"GlassUninsCheck",workspace.directory)) {
            SetError(T(L"Не удалось подготовить проверку MSI. Пакеты не удалены.",L"Could not prepare the MSI check. No packages were removed."));
            return 9;
        }
        const auto refuse=[&](const Probe& p,const std::wstring& detail,bool beforeChanges=true) {
            LogLine(std::wstring(L"Refusing MSI removal: ")+p.code+L" "+detail);
            std::wstring message=beforeChanges
                ? T(L"Путь или контекст ранее установленного MSI нельзя безопасно проверить. Удаление остановлено до изменений. Сохраните журнал и обратитесь за помощью; не удаляйте файлы вручную.",
                    L"The installed MSI path or context cannot be verified safely. Removal stopped before making changes. Save the log and ask for help; do not delete files manually.")
                : T(L"Регистрация MSI изменилась во время удаления; некоторые продукты могли уже быть удалены. Сохраните журнал и обратитесь за помощью; не удаляйте файлы вручную.",
                    L"An MSI registration changed during removal; some products may already have been removed. Save the log and ask for help; do not delete files manually.");
            message+=L"\r\n";message+=p.code;if (!detail.empty()) message+=L"\r\n"+detail;
            SetError(message);
            return false;
        };
        for (auto& p:probes) {
            if (cancelRequested_.load()) return ERROR_INSTALL_USEREXIT;
            std::wstring detail;
            if (!p.root.empty() && !GlassInstallUtil::IsSafeInstallPath(p.root)) {
                refuse(p,p.root);return 9;
            }
            const auto context=Msi::CheckProductContexts(p.code,detail,&cancelRequested_);
            if (context==Msi::ProductPathCheck::Cancelled) return ERROR_INSTALL_USEREXIT;
            if (context!=Msi::ProductPathCheck::Safe) { refuse(p,detail);return 9; }
            p.installed=Msi::IsProductInstalled(p.code);
            if (!p.installed) continue;
            if (p.root.empty()) { refuse(p,L"<installation directory unavailable>");return 9; }
            p.cached=workspace.directory+L"\\product-"+std::to_wstring(workspace.files.size())+L".msi";
            workspace.files.push_back(p.cached);
            if (!Msi::CopyLocalPackage(p.code,p.cached,&cancelRequested_)) {
                if (cancelRequested_.load()) return ERROR_INSTALL_USEREXIT;
                refuse(p,L"<cached MSI unavailable>");return 9;
            }
            const auto checked=Msi::CheckProductFileRoots(p.code,p.cached,p.root,detail,&cancelRequested_);
            if (checked==Msi::ProductPathCheck::Cancelled) return ERROR_INSTALL_USEREXIT;
            if (checked!=Msi::ProductPathCheck::Safe) { refuse(p,detail);return 9; }
        }
        // Native x64 must understand the exact-root installer protocol. Check
        // before stopping hosts or deleting any MSI, not after partial removal.
        // On 32-bit Windows the supported fallback is regsvr32 of ShellExt from
        // the selected root, which must remain available until platform removal.
        const bool os64=GlassInstallUtil::IsOs64Bit();
        const bool removingCore=os64?chosen.x64:chosen.x86;
        if (removingCore) {
            bool helperReady=false;
            if (os64) {
                const std::wstring helper=GlassInstallUtil::NativeDllForArch(GP::PlatformArch::X64,x64Dir_,x86Dir_);
                helperReady=!helper.empty() && GlassInstallUtil::PeMatches(helper,IMAGE_FILE_MACHINE_AMD64,
                    GlassInstallUtil::kInstallerShellExport);
            } else {
                const std::wstring shell=GlassInstallUtil::TrimTrailingSlashes(x86Dir_)+L"\\"+
                    GlassShared::GlassShellMenu::kShellExtRelativePath;
                helperReady=!x86Dir_.empty() && GlassInstallUtil::IsSafeInstallPath(shell) && PathFileExistsW(shell.c_str());
            }
            if (!helperReady) {
                LogLine(L"Shell helper for the selected root is missing or incompatible; no packages removed");
                SetError(T(L"Не удалось подготовить компонент регистрации оболочки для выбранного пути. Пакеты не удалены.",
                    L"Could not prepare the shell registration component for the selected path. No packages were removed."));
                return 9;
            }
        }
        bool prior64=false,prior86=false;
        if (!GlassInstallHost::QueryGlassPlatformRunning(GP::PlatformArch::X64,prior64) ||
            !GlassInstallHost::QueryGlassPlatformRunning(GP::PlatformArch::X86,prior86)) {
            LogLine(L"Could not inspect platform mutex state before removal");
            SetError(T(L"Не удалось проверить состояние платформы. Пакеты не удалены.",
                L"Could not inspect the platform state. No packages were removed."));
            return 10;
        }
        const bool enabled64=GP::QueryGlassAutorunUiState(GP::PlatformArch::X64)!=GP::AutorunUiState::Disabled;
        const bool enabled86=GP::QueryGlassAutorunUiState(GP::PlatformArch::X86)!=GP::AutorunUiState::Disabled;
        const bool platformRemoval=chosen.x64 || chosen.x86;
        const bool stopOg=GlassInstallUtil::IsOs64Bit() && (chosen.x64 || chosen.openGlass);
        const bool task64=GP::GlassAutorunTaskExists(GP::PlatformArch::X64);
        const bool task86=GP::GlassAutorunTaskExists(GP::PlatformArch::X86);
        if (platformRemoval) {
            std::wstring error;
            if (!GlassInstallRemoval::ValidatePlatformTask(GP::PlatformArch::X64,x64Dir_,task64,prior64,IsRu(),error) ||
                !GlassInstallRemoval::ValidatePlatformTask(GP::PlatformArch::X86,x86Dir_,task86,prior86,IsRu(),error) ||
                (prior64 && (!task64 || !enabled64)) || (prior86 && (!task86 || !enabled86))) {
                if (error.empty()) error=T(L"Работающую платформу невозможно безопасно восстановить. Остановите её и повторите удаление.",
                    L"The running platform cannot be restored safely. Stop it and retry removal.");
                LogLine(L"Refusing platform task: "+error);SetError(error);return 10;
            }
        }
        bool priorOg=false, ogServiceExisted=false;
        DWORD ogStartType=SERVICE_AUTO_START;
        GlassInstallUtil::OpenGlassTaskState ogTasks;
        if (stopOg && (!GlassInstallHost::QueryOpenGlassServiceStartType(ogServiceExisted,ogStartType) ||
            !GlassInstallUtil::QueryOpenGlassTasks(ogTasks) ||
            !GlassInstallUtil::QueryOpenGlassRunning(priorOg,&ogTasks))) {
            SetError(T(L"Не удалось проверить состояние OpenGlass. Пакеты не удалены.",
                L"Could not inspect OpenGlass. No packages were removed."));
            return 10;
        }
        if (stopOg && (ogServiceExisted || ogTasks.found)) {
            std::wstring error;
            if (!GlassInstallRemoval::ValidateOpenGlassPaths(openGlassDir_,&cancelRequested_,IsRu(),error)) {
                if (cancelRequested_.load()) return ERROR_INSTALL_USEREXIT;
                LogLine(L"Refusing OpenGlass backend: "+error);SetError(error);return 10;
            }
        }
        if (stopOg && priorOg && ((!ogServiceExisted && (!ogTasks.found || ogTasks.anyDisabled)) ||
            (ogServiceExisted && ogTasks.anyRunning))) {
            SetError(T(L"Нельзя безопасно восстановить текущее состояние OpenGlass. Остановите OpenGlass и повторите удаление.",
                L"The current OpenGlass state cannot be safely restored. Stop OpenGlass and retry removal."));
            return 10;
        }
        bool removed64=false,removed86=false,removedOg=false;
        bool stopAttempted64=false,stopAttempted86=false;
        auto restoreArch=[&](GP::PlatformArch arch,const std::wstring& root,bool wasRunning,bool wasEnabled,bool removed,bool stopAttempted) -> bool {
            if (!wasRunning || removed) return true;
            // Do not claim restoration while a previously requested stop can
            // still complete and shut down the restored instance afterwards.
            if (stopAttempted && !GP::WaitForPlatformStopped(arch,30000)) return false;
            bool running=false;
            if (!GlassInstallHost::QueryGlassPlatformRunning(arch,running)) return false;
            if (running) return true;
            std::wstring error;
            if (!wasEnabled || !GlassInstallRemoval::ValidatePlatformTask(arch,root,true,false,IsRu(),error)) {
                LogLine(L"Refusing to restart changed platform task: "+error);return false;
            }
            GP::StartPlatform(arch);
            const ULONGLONG start=GetTickCount64();
            do {
                if (!GlassInstallHost::QueryGlassPlatformRunning(arch,running)) return false;
                if (running) return true;
                Sleep(200);
            } while (GetTickCount64()-start<30000);
            return false;
        };
        auto restoreRemaining=[&]() -> bool {
            bool restored=true;
            if (!restoreArch(GP::PlatformArch::X64,x64Dir_,prior64,enabled64,removed64,stopAttempted64)) restored=false;
            if (!restoreArch(GP::PlatformArch::X86,x86Dir_,prior86,enabled86,removed86,stopAttempted86)) restored=false;
            if (priorOg && !removedOg) {
                bool running=false;
                if (!GlassInstallUtil::QueryOpenGlassRunning(running)) restored=false;
                else if (!running) {
                    std::wstring error;
                    if (!GlassInstallRemoval::ValidateOpenGlassPaths(openGlassDir_,nullptr,IsRu(),error)) {
                        LogLine(L"Refusing to restart changed OpenGlass backend: "+error);return false;
                    }
                    bool started=false;
                    if (ogServiceExisted) {
                        started=GlassInstallHost::StartOpenGlassServicePreservingStartType(ogStartType);
                        if (!started && ogStartType==SERVICE_DISABLED)
                            (void)GlassInstallHost::RestoreOpenGlassServiceStartType(ogStartType);
                    } else {
                        started=GlassInstallUtil::RunEnabledOpenGlassTask();
                    }
                    if (!started) restored=false;
                    else {
                        const ULONGLONG start=GetTickCount64();
                        do {
                            if (!GlassInstallUtil::QueryOpenGlassRunning(running)) { restored=false; break; }
                            if (running || GetTickCount64()-start>=15000) break;
                            Sleep(300);
                        } while (true);
                        if (!running) restored=false;
                    }
                }
            }
            if (!restored) LogLine(L"Could not restore a previously running component");
            return restored;
        };
        auto cancelledResult=[&]() -> int {
            if (restoreRemaining()) return ERROR_INSTALL_USEREXIT;
            SetError(T(L"Пакеты не удалены, но не удалось перезапустить прежние компоненты.",
                L"No packages were removed, but previously running components could not be restarted."));
            return 17;
        };
        // SecureUxTheme принадлежит GlassSettings: деактивация «как в GlassSettings» — до
        // удаления продуктов и до остановки компонентов (удаление GlassSettings и полное
        // удаление). Отказ helper'а останавливает удаление: активный хук winlogon без
        // интерфейса управления оставаться не должен.
        bool secureUxThemeReboot=false, secureUxThemeThemeNote=false;
        if (chosen.settings || (chosen.secureUxTheme && SecureUxThemeManaged())) {
            SetStatus(T(L"Отключение SecureUxTheme…",L"Deactivating SecureUxTheme…"),450);
            GlassInstallUtil::SecureUxThemeDeactivateRun sutRun;
            std::wstring sutError;
            if (!GlassInstallUtil::InvokeSecureUxThemeDeactivate(x64Dir_,x86Dir_,&sutRun,&sutError)) {
                LogLine(L"SecureUxTheme deactivation failed: "+sutError);
                if (chosen.settings || chosen.secureUxTheme) {
                    SetError(T(L"Не удалось отключить SecureUxTheme. Пакеты не удалены.",
                        L"Could not deactivate SecureUxTheme. No packages were removed."));
                    return 18;
                }
                // Полное удаление без установленного GlassSettings: helper недоступен,
                // активного состояния обычно и нет — продолжаем с записью в журнал.
            } else if (sutRun.ran) {
                secureUxThemeReboot=sutRun.rebootRequired;
                secureUxThemeThemeNote=sutRun.themeNotRestored;
                if (sutRun.skippedExternalInstall)
                    LogLine(L"SecureUxTheme: external installation owns the state; nothing changed");
            }
        }
        SetStatus(T(L"Остановка компонентов…",L"Stopping components…"),500);
        if (!GlassInstallHost::StopAllForUninstall(x64Dir_,x86Dir_,platformRemoval,platformRemoval,stopOg,
                &cancelRequested_,&stopAttempted64,&stopAttempted86,&prior64,&prior86)) {
            if (cancelRequested_.load()) return cancelledResult();
            if (!restoreRemaining()) {
                SetError(T(L"Не удалось остановить компоненты или восстановить их прежнее состояние. Пакеты не удалены.",
                    L"Could not stop components or restore their previous state. No packages were removed."));
                return 17;
            }
            SetError(T(L"Не удалось остановить компоненты. Пакеты не удалены.",L"Could not stop components. No packages were removed."));
            return 10;
        }
        if (cancelRequested_.load()) return cancelledResult();
        if (platformRemoval || chosen.settings)
            SetStatus(T(L"Проверка модулей в процессах…",L"Checking modules in processes…"),600);
        if ((platformRemoval || chosen.settings) && !EnsureModulesReleased()) {
            if (cancelRequested_.load()) return cancelledResult();
            (void)restoreRemaining(); return 11;
        }
        if (cancelRequested_.load()) return cancelledResult();
        // The close/cancel handler and worker race only through this atomic boundary.
        // Once Removing wins, the window can no longer promise cancellation before MSI.
        OperationState expected=OperationState::BeforeRemoval;
        if (!operationState_.compare_exchange_strong(expected,OperationState::Removing,std::memory_order_acq_rel))
            return cancelledResult();
        removalStarted_.store(true,std::memory_order_release);
        if (window) PostMessageW(window,kWorkerHold,static_cast<WPARAM>(Page::Removing),0);
        int count=(chosen.secureUxTheme?1:0)+(chosen.settings?1:0)+(chosen.openGlass?1:0)+(chosen.x86?1:0)+(chosen.x64?1:0);
        int done=0;
        auto remove=[&](const wchar_t* code,const wchar_t* name) -> bool {
            SetStatus(std::wstring(T(L"Удаление ",L"Removing "))+name+L"…",1000+(done*8500/std::max(1,count)));
            const auto it=std::find_if(probes.begin(),probes.end(),[&](const Probe& p){return _wcsicmp(p.code,code)==0;});
            if (it==probes.end()) return false;
            std::wstring detail;
            if (Msi::CheckProductContexts(code,detail)!=Msi::ProductPathCheck::Safe ||
                Msi::IsProductInstalled(code)!=it->installed)
                return refuse(*it,detail,false);
            if (it->installed) {
                // Re-read the current cached package: the registration may
                // have changed since the protected preflight copy was made.
                const std::wstring current=workspace.directory+L"\\current-"+code+L".msi";
                const bool copied=Msi::CopyLocalPackage(code,current);
                const auto paths=copied ? Msi::CheckProductFileRoots(code,current,it->root,detail)
                                        : Msi::ProductPathCheck::Unverifiable;
                DeleteFileW(current.c_str());
                if (!copied) detail=L"Current cached MSI is unavailable";
                if (paths!=Msi::ProductPathCheck::Safe) return refuse(*it,detail,false);
            }
            if (!Msi::UninstallProduct(code)) {
                SetError(std::wstring(T(L"Не удалось удалить пакет: ",L"Could not remove package: "))+name);
                return false;
            }
            return true;
        };
        if (chosen.secureUxTheme) {
            if (!remove(Msi::kProductSecureUxThemeX64,L"SecureUxTheme x64") ||
                !remove(Msi::kProductSecureUxThemeX86,L"SecureUxTheme x86")) { restoreRemaining(); return 12; }
            ++done;
        }
        if (chosen.settings) {
            if (!remove(Msi::kProductSettingsX64,L"GlassSettings x64") || !remove(Msi::kProductSettingsX86,L"GlassSettings x86")) { restoreRemaining(); return 12; }
            GlassInstallUtil::DeleteStartMenuShortcut(); ++done;
        }
        if (chosen.openGlass) {
            if (!remove(Msi::kProductOpenGlass,L"OpenGlass")) { restoreRemaining(); return 13; }
            removedOg=true;
            std::wstring backendError;bool serviceExists=false;DWORD serviceType=0;GlassInstallUtil::OpenGlassTaskState currentTasks;
            if (!GlassInstallHost::QueryOpenGlassServiceStartType(serviceExists,serviceType) ||
                !GlassInstallUtil::QueryOpenGlassTasks(currentTasks) || serviceExists!=ogServiceExisted || currentTasks.found!=ogTasks.found ||
                !GlassInstallRemoval::ValidateOpenGlassPaths(openGlassDir_,nullptr,IsRu(),backendError)) {
                SetError(T(L"OpenGlass MSI удалён, но служба или задача изменилась и не будет удалена.",
                    L"OpenGlass MSI was removed, but the service or task changed and will not be deleted."));
                LogLine(L"OpenGlass cleanup refused: "+backendError);(void)restoreRemaining();return 17;
            }
            if (!GlassInstallHost::RemoveOpenGlassServiceAndTasks()) { SetError(T(L"OpenGlass удалён, но служба или задача не удалена.",L"OpenGlass was removed, but its service or task could not be removed.")); (void)restoreRemaining(); return 17; }
            GlassInstallUtil::ClearOpenGlassInstallPath(); ++done;
        }
        if (removingCore && !GlassInstallUtil::InvokeShellExtRegistration(false,x64Dir_,x86Dir_)) {
            SetError(T(L"Не удалось снять регистрацию расширения оболочки. Некоторые выбранные продукты могли уже быть удалены.",
                L"Could not unregister the shell extension. Some selected products may already have been removed."));
            restoreRemaining(); return 16;
        }
        if (chosen.x86) {
            if (!remove(Msi::kProductPlatformX86,L"Glass Platform x86")) { restoreRemaining(); return 14; }
            removed86=true;
            std::wstring taskError;
            if (!GlassInstallRemoval::ValidatePlatformTask(GP::PlatformArch::X86,x86Dir_,task86,false,IsRu(),taskError) ||
                !GlassInstallHost::RemovePlatformAutorunTasks(false,true)) {
                SetError(T(L"Платформа x86 удалена, но задача автозапуска не удалена.",L"The x86 platform was removed, but its startup task was not removed."));
                LogLine(L"x86 task cleanup refused: "+taskError);(void)restoreRemaining();return 17;
            }
            GlassInstallUtil::ClearPlatformInstallPath(false); ++done;
        }
        if (chosen.x64) {
            if (!remove(Msi::kProductPlatformX64,L"Glass Platform x64")) { restoreRemaining(); return 15; }
            removed64=true;
            std::wstring taskError;
            if (!GlassInstallRemoval::ValidatePlatformTask(GP::PlatformArch::X64,x64Dir_,task64,false,IsRu(),taskError) ||
                !GlassInstallHost::RemovePlatformAutorunTasks(true,false)) {
                SetError(T(L"Платформа x64 удалена, но задача автозапуска не удалена.",L"The x64 platform was removed, but its startup task was not removed."));
                LogLine(L"x64 task cleanup refused: "+taskError);(void)restoreRemaining();return 17;
            }
            GlassInstallUtil::ClearPlatformInstallPath(true); ++done;
        }
        // Pin retained product directories, including nested/aliased roots,
        // while pruning only empty directories of removed products.
        std::vector<std::wstring> keptRoots;
        if (installed_.x64 && !chosen.x64 && !x64Dir_.empty()) keptRoots.push_back(x64Dir_);
        if (installed_.x86 && !chosen.x86 && !x86Dir_.empty()) keptRoots.push_back(x86Dir_);
        if (installed_.openGlass && !chosen.openGlass && !openGlassDir_.empty()) keptRoots.push_back(openGlassDir_);
        if (installed_.settings && !chosen.settings) {
            if (!x64Dir_.empty()) keptRoots.push_back(GlassInstallUtil::TrimTrailingSlashes(x64Dir_)+L"\\GlassSettings");
            if (!x86Dir_.empty()) keptRoots.push_back(GlassInstallUtil::TrimTrailingSlashes(x86Dir_)+L"\\GlassSettings");
        }
        if (chosen.settings) {
            if (!x64Dir_.empty()) GlassInstallUtil::PruneEmptyDirectories(GlassInstallUtil::TrimTrailingSlashes(x64Dir_)+L"\\GlassSettings",keptRoots);
            if (!x86Dir_.empty()) GlassInstallUtil::PruneEmptyDirectories(GlassInstallUtil::TrimTrailingSlashes(x86Dir_)+L"\\GlassSettings",keptRoots);
        }
        if (chosen.openGlass && !openGlassDir_.empty() &&
            !GlassInstallUtil::SameInstallPath(openGlassDir_,x64Dir_) &&
            !GlassInstallUtil::SameInstallPath(openGlassDir_,x86Dir_))
            GlassInstallUtil::PruneEmptyDirectories(openGlassDir_,keptRoots);
        if (chosen.x86 && !x86Dir_.empty() &&
            (chosen.x64 || !GlassInstallUtil::SameInstallPath(x86Dir_,x64Dir_)))
            GlassInstallUtil::PruneEmptyDirectories(x86Dir_,keptRoots);
        if (chosen.x64 && !x64Dir_.empty()) GlassInstallUtil::PruneEmptyDirectories(x64Dir_,keptRoots);
        // Настройки текущего пользователя — только по явному выбору: весь раздел
        // GlassPlatform и кастомные значения OpenGlass в DWM.
        if (chosen.cleanupSettings) {
            SetStatus(T(L"Удаление настроек…",L"Removing settings…"),9200);
            Shared::RegistrySanity::RemoveUserSettings();
            LogLine(L"Settings cleanup: HKCU Software\\StanislavKa\\GlassPlatform removed; OpenGlass DWM values removed");
        }
        if (!restoreRemaining()) {
            SetError(T(L"Выбранные продукты удалены, но не удалось перезапустить оставшиеся компоненты.",
                L"Selected products were removed, but remaining components could not be restarted."));
            return 17;
        }
        std::wstring complete=T(L"Удаление завершено.",L"Uninstall complete.");
        if (secureUxThemeReboot)
            complete+=T(L" Требуется перезагрузка, чтобы завершить очистку SecureUxTheme.",
                L" Restart Windows to finish the SecureUxTheme cleanup.");
        if (secureUxThemeThemeNote)
            complete+=T(L" Пользовательский визуальный стиль не восстановлен: после перезагрузки Windows применит стандартный.",
                L" The custom visual style was not restored: Windows will use the standard one after restart.");
        SetStatus(complete,10000);
        return 0;
    }
    void StartRemoval() {
        if (worker_.joinable() || (!selected_.Any() && !selected_.cleanupSettings)) return;
        NormalizeSelection(); cancelRequested_.store(false); removalStarted_.store(false);
        operationState_.store(OperationState::BeforeRemoval);
        const Selection chosen=selected_;
        SetPage(Page::Stopping);
        try {
            worker_=std::thread([this,chosen]() {
                const int result=RunOperation(chosen);
                { std::lock_guard<std::mutex> lock(dataMutex_); resultCode_=result; }
                if (window) PostMessageW(window,kWorkerFinished,0,0);
            });
        } catch (...) { SetError(T(L"Не удалось запустить удаление.",L"Could not start uninstallation.")); SetPage(Page::Error); }
    }
    void FinishRemoval() {
        if (worker_.joinable()) worker_.join();
        // The irreversible MSI phase has actually ended. Keeping Removing here
        // makes both Close and the system X ignore clicks on every terminal page.
        operationState_.store(OperationState::Finished,std::memory_order_release);
        int result; { std::lock_guard<std::mutex> lock(dataMutex_); result=resultCode_; }
        LogLine(L"Removal worker finished; result="+std::to_wstring(result));
        if (result==ERROR_INSTALL_USEREXIT) SetPage(Page::Cancelled);
        else if (result==0) SetPage(Page::Complete);
        else SetPage(Page::Error);
    }
    void RequestClose() {
        if (page_==Page::Removing || operationState_.load(std::memory_order_acquire)==OperationState::Removing) return;
        if (page_==Page::Stopping || page_==Page::UnlockModules || page_==Page::UnlockProcesses) {
            OperationState expected=OperationState::BeforeRemoval;
            if (!operationState_.compare_exchange_strong(expected,OperationState::CancelRequested,std::memory_order_acq_rel)) return;
            cancelRequested_.store(true); action_.store(Action::Cancel);
            if (gate_) SetEvent(gate_);
            EnableWindow(cancel_,FALSE);
            return;
        }
        DestroyWindow(window);
    }
    void SetComponent(const std::wstring& id,bool checked) {
        if (page_!=Page::Select) return;
        if (id==L"x64" && installed_.x64) selected_.x64=checked;
        else if (id==L"x86" && installed_.x86 && !(CoreSelected() && GlassInstallUtil::IsOs64Bit())) selected_.x86=checked;
        else if (id==L"settings" && installed_.settings && !CoreSelected()) selected_.settings=checked;
        else if (id==L"openglass" && installed_.openGlass && !CoreSelected()) selected_.openGlass=checked;
        else if (id==L"secureUxTheme" && installed_.secureUxTheme && !selected_.settings && !CoreSelected()) selected_.secureUxTheme=checked;
        else if (id==L"cleanup") selected_.cleanupSettings=checked;
        NormalizeSelection(); EnableWindow(next_,selected_.Any()||selected_.cleanupSettings); ShowNativeChecks(); SyncWeb(); InvalidateRect(window,nullptr,FALSE);
    }
    HRESULT HandleWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
        if (closing_ || !args) return S_OK;
        LPWSTR rawSource=nullptr;
        if (FAILED(args->get_Source(&rawSource))) return S_OK;
        const std::wstring source=rawSource?rawSource:L""; CoTaskMemFree(rawSource);
        if (source!=std::wstring(WizardAssets::kDocumentUrl)+L"?lang=ru-RU" && source!=std::wstring(WizardAssets::kDocumentUrl)+L"?lang=en-US") return S_OK;
        LPWSTR raw=nullptr;
        if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) { CoTaskMemFree(raw); return S_OK; }
        const std::wstring message(raw); CoTaskMemFree(raw);
        if (message==L"cancel") { RequestClose(); return S_OK; }
        if (message==L"ready") { bridgeReady_=true; SyncWeb(); return S_OK; }
        if (message.size()>64 || page_!=Page::Select || message.rfind(L"component:",0)!=0) return S_OK;
        const auto colon=message.find(L':',10);
        if (colon==std::wstring::npos || colon+2!=message.size()) return S_OK;
        if (message.back()!=L'0' && message.back()!=L'1') return S_OK;
        SetComponent(message.substr(10,colon-10),message.back()==L'1');
        return S_OK;
    }
    void WebFailure() {
        webReady_=false; bridgeReady_=false; initSent_=false; webState_.clear(); webFailed_=true;
        if (window) KillTimer(window,kTimer);
        if (controller_) controller_->put_IsVisible(FALSE);
        ShowWindow(webHost_,SW_HIDE); ShowNativeChecks(); InvalidateRect(window,nullptr,FALSE);
    }
    void InitializeWebView() {
        LPWSTR version=nullptr;
        const bool available=SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr,&version)) && version && *version;
        CoTaskMemFree(version);
        if (!available) { WebFailure(); return; }
        PWSTR local=nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)) || !local) { WebFailure(); return; }
        const std::wstring folder=std::wstring(local)+L"\\GlassPlatform\\UninstallerWebView2";
        CoTaskMemFree(local);
        const int result=SHCreateDirectoryExW(window,folder.c_str(),nullptr);
        if (result!=ERROR_SUCCESS && result!=ERROR_ALREADY_EXISTS && result!=ERROR_FILE_EXISTS) { WebFailure(); return; }
        const HRESULT hr=CreateCoreWebView2EnvironmentWithOptions(nullptr,folder.c_str(),nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result,ICoreWebView2Environment* env) -> HRESULT {
                if (closing_ || !IsWindow(window)) return S_OK;
                if (FAILED(result) || !env) { WebFailure(); return S_OK; }
                environment_=env;
                const HRESULT create=environment_->CreateCoreWebView2Controller(webHost_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT result2,ICoreWebView2Controller* ctrl) -> HRESULT {
                        if (closing_ || !IsWindow(window)) { if (ctrl) ctrl->Close(); return S_OK; }
                        if (FAILED(result2) || !ctrl) { WebFailure(); return S_OK; }
                        controller_=ctrl;
                        if (FAILED(ctrl->get_CoreWebView2(web_.ReleaseAndGetAddressOf())) || !web_) { WebFailure(); return S_OK; }
                        ConfigureWebView(); return S_OK;
                    }).Get());
                if (FAILED(create)) WebFailure();
                return S_OK;
            }).Get());
        if (FAILED(hr)) WebFailure();
    }
    void ConfigureWebView() {
        controller_->put_IsVisible(FALSE);
        ComPtr<ICoreWebView2Controller2> colored;
        if (SUCCEEDED(controller_.As(&colored))) { COREWEBVIEW2_COLOR c{255,16,18,37}; colored->put_DefaultBackgroundColor(c); }
        Layout();
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(web_->get_Settings(&settings))) {
            settings->put_AreDevToolsEnabled(FALSE); settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE); settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        }
        if (FAILED(web_->AddWebResourceRequestedFilter(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL))) { WebFailure(); return; }
        if (FAILED(web_->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (closing_) return S_OK;
                ComPtr<ICoreWebView2WebResourceRequest> req;
                if (FAILED(args->get_Request(&req))) return E_FAIL;
                LPWSTR raw=nullptr; req->get_Uri(&raw); const std::wstring url=raw?raw:L""; CoTaskMemFree(raw);
                const auto asset=WizardAssets::Resolve(url); const bool allowed=asset.id!=0;
                ComPtr<IStream> stream;
                if (allowed) { const auto data=Resource(asset.id); if (!data.data || !data.size) return E_FAIL; stream.Attach(SHCreateMemStream(data.data,data.size)); }
                if (allowed && !stream) return E_OUTOFMEMORY;
                std::wstring headers=L"Content-Type: "; headers+=allowed?asset.contentType:L"text/plain";
                headers+=L"\r\nCache-Control: no-store\r\n";
                if (asset.document) headers+=L"Content-Security-Policy: default-src 'none'; img-src https://glassplatform.invalid data:; font-src https://glassplatform.invalid; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'\r\n";
                ComPtr<ICoreWebView2WebResourceResponse> response;
                const HRESULT hr=environment_->CreateWebResourceResponse(stream.Get(),allowed?200:403,allowed?L"OK":L"Forbidden",headers.c_str(),&response);
                return FAILED(hr)?hr:args->put_Response(response.Get());
            }).Get(),&requestToken_))) { WebFailure(); return; }
        web_->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR raw=nullptr; args->get_Uri(&raw); const std::wstring url=raw?raw:L""; CoTaskMemFree(raw);
                if (url!=std::wstring(WizardAssets::kDocumentUrl)+L"?lang=ru-RU" && url!=std::wstring(WizardAssets::kDocumentUrl)+L"?lang=en-US") args->put_Cancel(TRUE);
                return S_OK;
            }).Get(),&navToken_);
        web_->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*,ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT { return args->put_Handled(TRUE); }).Get(),&popupToken_);
        web_->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*,ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT { return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); }).Get(),&permissionToken_);
        if (FAILED(web_->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT { return HandleWebMessage(args); }).Get(),&messageToken_))) { WebFailure(); return; }
        web_->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (closing_) return S_OK;
                BOOL success=FALSE; args->get_IsSuccess(&success);
                if (!success) { WebFailure(); return S_OK; }
                webReady_=true; ShowNativeChecks(); KillTimer(window,kTimer);
                controller_->put_IsVisible(TRUE); ShowWindow(webHost_,SW_SHOWNA); SyncWeb();
                return S_OK;
            }).Get(),&completedToken_);
        const std::wstring url=std::wstring(WizardAssets::kDocumentUrl)+(IsRu()?L"?lang=ru-RU":L"?lang=en-US");
        if (FAILED(web_->Navigate(url.c_str()))) WebFailure();
    }
    void DrawTextBlock(HDC dc,const std::wstring& text,RECT r,HFONT font,COLORREF color) {
        HGDIOBJ old=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,color);
        DrawTextW(dc,text.c_str(),static_cast<int>(text.size()),&r,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
        SelectObject(dc,old);
    }
    void DrawLoadingSpinner(HDC dc) {
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
        g.DrawArc(&arc,x,y,diameter,diameter,
            static_cast<Gdiplus::REAL>(-90.0+phase*360.0),static_cast<Gdiplus::REAL>(268.0));
    }
    void Paint() {
        PAINTSTRUCT ps{}; HDC dc=BeginPaint(window,&ps);
        RECT client{}; GetClientRect(window,&client);
        RECT content=Box(0,0,kWidth,kContentHeight); FillRect(dc,&content,brush_);
        WizardTheme::PaintFooterBar(dc,client.right,Scale(kContentHeight),client.bottom,kFooterTop,kFooterBottom,RGB(74,71,109));
        std::wstring status,error,report; int progress;
        { std::lock_guard<std::mutex> lock(dataMutex_); status=status_; error=error_; report=holdReport_; progress=progress_; }
        if (!webReady_ && !webFailed_) {
            // Загрузка HTML-контента: тот же индикатор, что и в установщике.
            DrawTextBlock(dc,L"GlassPlatform",Box(48,32,400,28),font_,RGB(245,244,252));
            DrawLoadingSpinner(dc);
            EndPaint(window,&ps);
            return;
        }
        if (!webReady_) {
            DrawTextBlock(dc,L"GlassPlatform",Box(48,32,400,28),font_,RGB(245,244,252));
            const wchar_t* title=page_==Page::Select?T(L"Выберите продукты для удаления",L"Select products to remove"):
                page_==Page::UnlockModules?T(L"Модули заняты процессами",L"Processes are holding modules"):
                page_==Page::UnlockProcesses?T(L"Процессы удерживают модули",L"Processes are holding modules"):
                page_==Page::Cancelled?T(L"Удаление отменено",L"Uninstall cancelled"):
                page_==Page::Complete?T(L"Удаление завершено",L"Uninstall complete"):
                page_==Page::Error?T(L"Удаление не завершено",L"Uninstall did not finish"):
                T(L"Удаление компонентов…",L"Removing components…");
            DrawTextBlock(dc,title,Box(48,94,890,55),heading_,RGB(245,244,252));
            const std::wstring body=page_==Page::Select?Warning():
                page_==Page::UnlockModules || page_==Page::UnlockProcesses?report:
                page_==Page::Error?error:status;
            DrawTextBlock(dc,body,Box(48,page_==Page::Select?390:190,880,220),font_,RGB(196,198,219));
        }
        if (page_==Page::Stopping || page_==Page::Removing) {
            RECT track=Box(0,kHtmlHeight,kWidth,4); HBRUSH b=CreateSolidBrush(RGB(42,45,67)); FillRect(dc,&track,b); DeleteObject(b);
            RECT fill=track; fill.right=fill.left+MulDiv(fill.right-fill.left,std::clamp(progress,0,10000),10000);
            b=CreateSolidBrush(RGB(144,127,230)); FillRect(dc,&fill,b); DeleteObject(b);
            DrawTextBlock(dc,status,Box(34,kContentHeight+16,500,28),font_,RGB(231,229,246));
        }
        EndPaint(window,&ps);
    }
    void Cleanup() {
        closing_=true;
        if (worker_.joinable()) { cancelRequested_.store(true); if (gate_) SetEvent(gate_); worker_.join(); }
        if (controller_) controller_->Close();
        web_.Reset(); controller_.Reset(); environment_.Reset();
        for (HWND h:{back_,next_,cancel_}) WizardTheme::DetachButtonScope(h);
        WizardTheme::ResetButtonCache();
        if (font_) DeleteObject(font_); if (heading_) DeleteObject(heading_);
        if (buttonBrush_) DeleteObject(buttonBrush_); if (brush_) DeleteObject(brush_);
        font_=heading_=nullptr; brush_=buttonBrush_=nullptr;
    }
    LRESULT Handle(UINT msg,WPARAM w,LPARAM l) {
        switch (msg) {
        case WM_CREATE: Initialize(); return 0;
        case kWebInit: if (!closing_) InitializeWebView(); return 0;
        case kWorkerStatus: SyncWeb(); InvalidateRect(window,nullptr,FALSE); return 0;
        case kWorkerHold: if (!closing_) SetPage(static_cast<Page>(w)); return 0;
        case kWorkerFinished: FinishRemoval(); return 0;
        case WM_SIZE: Layout(); return 0;
        case WM_MOVE: if (controller_) controller_->NotifyParentWindowPositionChanged(); return 0;
        case WM_DPICHANGED: {
            dpi_=HIWORD(w); const RECT* suggested=reinterpret_cast<const RECT*>(l);
            RECT r{0,0,Scale(kWidth),Scale(kHeight)};
            AdjustWindowRectExForDpi(&r,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,WS_EX_CONTROLPARENT,dpi_);
            SetWindowPos(window,nullptr,suggested->left,suggested->top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
            CreateFonts(); Layout(); BuildCache(); InvalidateRect(window,nullptr,FALSE); return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (w==kTimer) {
                if (!webReady_ && !webFailed_) InvalidateRect(window,nullptr,FALSE);
                else KillTimer(window,kTimer);
                return 0;
            }
            break;
        case WM_PAINT: Paint(); return 0;
        case WM_CTLCOLORBTN: {
            HDC dc=reinterpret_cast<HDC>(w); HWND h=reinterpret_cast<HWND>(l);
            const bool footer=h==back_ || h==next_ || h==cancel_;
            SetBkMode(dc,TRANSPARENT); SetBkColor(dc,footer?RGB(24,25,47):kDark);
            if (!footer) SetTextColor(dc,RGB(239,237,249));
            return reinterpret_cast<LRESULT>(footer?buttonBrush_:brush_);
        }
        case WM_COMMAND: {
            const int id=LOWORD(w);
            if (id==IDC_CANCEL || id==IDCANCEL) { RequestClose(); return 0; }
            if (id==kSelectAll && page_==Page::Select) {
                const bool cleanup=selected_.cleanupSettings;
                selected_=installed_; selected_.cleanupSettings=cleanup;
                NormalizeSelection(); SetPage(Page::Select); return 0;
            }
            if (HIWORD(w)==BN_CLICKED && page_==Page::Select && !webReady_) {
                if (id==IDC_COMPONENT64) SetComponent(L"x64",SendMessageW(check_[0],BM_GETCHECK,0,0)==BST_CHECKED);
                else if (id==IDC_COMPONENT32) SetComponent(L"x86",SendMessageW(check_[1],BM_GETCHECK,0,0)==BST_CHECKED);
                else if (id==IDC_COMPONENT_SETTINGS) SetComponent(L"settings",SendMessageW(check_[2],BM_GETCHECK,0,0)==BST_CHECKED);
                else if (id==IDC_COMPONENT_OG) SetComponent(L"openglass",SendMessageW(check_[3],BM_GETCHECK,0,0)==BST_CHECKED);
                else if (id==IDC_UNINS_SETTINGS_CLEANUP) SetComponent(L"cleanup",SendMessageW(check_[4],BM_GETCHECK,0,0)==BST_CHECKED);
                else if (id==IDC_UNINS_SECUREUXTHEME) SetComponent(L"secureUxTheme",SendMessageW(check_[5],BM_GETCHECK,0,0)==BST_CHECKED);
            }
            if (id==IDC_NEXT || id==IDOK) {
                if (page_==Page::Select) StartRemoval();
                else if (page_==Page::UnlockModules || page_==Page::UnlockProcesses) {
                    action_.store(page_==Page::UnlockModules?Action::Retry:Action::Unlock);
                    if (gate_) SetEvent(gate_); SetPage(Page::Stopping);
                } else if (page_==Page::Complete || page_==Page::Cancelled || page_==Page::Error) RequestClose();
                return 0;
            }
            break;
        }
        case WM_SYSCOMMAND: if ((w&0xfff0)==SC_CLOSE) { RequestClose(); return 0; } break;
        case WM_CLOSE: RequestClose(); return 0;
        case WM_DESTROY: { const int result=resultCode_; Cleanup(); PostQuitMessage(result); return 0; }
        }
        return DefWindowProcW(window,msg,w,l);
    }
    static LRESULT CALLBACK WindowProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
        auto* self=reinterpret_cast<Uninstaller*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (msg==WM_NCCREATE) {
            self=static_cast<Uninstaller*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window=hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
            WizardTheme::SetRootWindow(hwnd,kContentHeight,kHeight,kFooterTop,kFooterBottom);
        }
        const LRESULT result=self?self->Handle(msg,w,l):DefWindowProcW(hwnd,msg,w,l);
        if (msg==WM_NCDESTROY) WizardTheme::SetRootWindow(nullptr);
        return result;
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    gInstance=instance;
    gLanguage=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_RUSSIAN?
        MAKELANGID(LANG_RUSSIAN,SUBLANG_DEFAULT):MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US);
    SetThreadUILanguage(gLanguage);
    Arguments args=ParseArguments();
    if (!args.fromTemp) {
        const int launch=RelaunchFromPrivateTemp(args);
        if (!launch || args.silent) return launch;
        args.launchError=launch; // Show the failure inside the wizard, not in a system dialog.
    }
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 4;
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\GlassSetup.Instance.Mutex");
    if (!mutex) { CoUninitialize(); return 4; }
    const bool already=GetLastError()==ERROR_ALREADY_EXISTS;
    if (already && args.silent) { CloseHandle(mutex); CoUninitialize(); return ERROR_INSTALL_ALREADY_RUNNING; }
    GlassInstallUtil::ConfigureEmbeddedUnlocker(instance,IDR_UNLOCKER_X64);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    ULONG_PTR token=0; Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok) { CloseHandle(mutex); CoUninitialize(); return 3; }
    int result=0;
    {
        Uninstaller app(args);
        if (args.silent) result=app.RunSilent();
        else {
            (void)WizardTheme::InstallDetours();
            if (app.Create(show,already)) result=app.MessageLoop(); else result=4;
        }
    }
    WizardTheme::UninstallDetours();
    GlassInstallUtil::CleanupEmbeddedUnlocker();
    Gdiplus::GdiplusShutdown(token);
    CloseHandle(mutex);
    CoUninitialize();
    // R13: после удаления в %WINDIR%\Temp не остаётся ни каталога GlassUnins-*, ни копии exe.
    if (args.fromTemp) SchedulePrivateCopyRemoval();
    return result;
}
