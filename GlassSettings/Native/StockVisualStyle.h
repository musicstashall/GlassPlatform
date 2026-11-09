#pragma once
#include <windows.h>
#include <objbase.h>
#include <uxtheme.h>
#include <Helpers/ThemeHelper.h>
#include <string>
#include <thread>

// Native equivalent of VisualStyleThemeManager.ApplyTheme's PRIMARY path.
// Maintenance must run this in an OS-native process, not WOW64.
namespace NativeStockVisualStyle {
struct Result {
    HRESULT comHr=E_PENDING,fontHr=E_PENDING,readHr=E_PENDING;
    bool verified=false;
};
namespace Detail {
// Slot order and signatures match GlassSettings/Services/ThemeManager2.cs.
struct ThemeManager : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Init(DWORD)=0;
    virtual HRESULT STDMETHODCALLTYPE InitAsync(HWND,int)=0;
    virtual HRESULT STDMETHODCALLTYPE Refresh()=0;
    virtual HRESULT STDMETHODCALLTYPE RefreshAsync(HWND,int)=0;
    virtual HRESULT STDMETHODCALLTYPE RefreshComplete()=0;
    virtual HRESULT STDMETHODCALLTYPE GetThemeCount(int*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetTheme(int,IUnknown**)=0;
    virtual HRESULT STDMETHODCALLTYPE IsThemeDisabled(int,int*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentTheme(int*)=0;
    virtual HRESULT STDMETHODCALLTYPE SetCurrentTheme(HWND,int,int,DWORD,DWORD)=0;
    virtual HRESULT STDMETHODCALLTYPE GetCustomTheme(int*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultTheme(int*)=0;
    virtual HRESULT STDMETHODCALLTYPE CreateThemePack(HWND,LPCWSTR,DWORD)=0;
    virtual HRESULT STDMETHODCALLTYPE CloneAndSetCurrentTheme(HWND,LPCWSTR,LPWSTR*)=0;
    virtual HRESULT STDMETHODCALLTYPE InstallThemePack(HWND,LPCWSTR,int,DWORD,LPWSTR*,IUnknown**)=0;
    virtual HRESULT STDMETHODCALLTYPE DeleteTheme(LPCWSTR)=0;
    virtual HRESULT STDMETHODCALLTYPE OpenTheme(HWND,LPCWSTR,DWORD)=0;
    virtual HRESULT STDMETHODCALLTYPE AddAndSelectTheme(HWND,LPCWSTR,DWORD,DWORD)=0;
};
inline HRESULT ApplyOnSta(Result& result) {
    wchar_t windows[32768]{};
    const UINT n=GetWindowsDirectoryW(windows,32768);
    if(!n||n>=32768)return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    const std::wstring style=std::wstring(windows)+L"\\Resources\\Themes\\Aero\\aero.msstyles";
    const std::wstring theme=std::wstring(windows)+L"\\Resources\\Themes\\aero.theme";
    for(const auto& path:{style,theme}) {
        const DWORD attributes=GetFileAttributesW(path.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized)){result.comHr=initialized;return initialized;}
    struct Apartment {~Apartment(){CoUninitialize();}} apartment;
    const GUID clsid={0x9324da94,0x50ec,0x4a14,{0xa7,0x70,0xe9,0x0c,0xa0,0x3e,0x7c,0x8f}};
    const GUID iid={0xc1e8c83e,0x845d,0x4d95,{0x81,0xdb,0xe2,0x83,0xfd,0xff,0xc0,0x00}};
    ThemeManager* manager=nullptr;
    result.comHr=CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER|CLSCTX_LOCAL_SERVER|CLSCTX_REMOTE_SERVER,iid,reinterpret_cast<void**>(&manager));
    if(FAILED(result.comHr))return result.comHr;
    if(!manager)return E_NOINTERFACE;
    struct Release {ThemeManager* p;~Release(){p->Release();}} release{manager};
    result.comHr=manager->Init(0);
    if(FAILED(result.comHr))return result.comHr;
    // Exactly the Settings primary path: IgnoreScreensaver, Silent. In particular
    // do NOT IgnoreColor: that can retain custom system colors after style reset.
    result.comHr=manager->AddAndSelectTheme(nullptr,theme.c_str(),1u<<5,1u<<2);
    if(FAILED(result.comHr))return result.comHr;
    DWORD fonts=0,bytes=sizeof(fonts);
    (void)RegGetValueW(HKEY_CURRENT_USER,L"Software\\StanislavKa\\GlassPlatform\\GlassSettings",
        L"EnableSystemFont",RRF_RT_REG_DWORD,nullptr,&fonts,&bytes);
    // Settings calls ordinal 65 AFTER ThemeManager only when system fonts are on.
    if(fonts) {
        result.fontHr=Shared::ThemeHelper::SetSystemVisualStyle(style.c_str(),L"NormalColor",L"NormalSize",65);
        if(FAILED(result.fontHr))return result.fontHr;
    }
    wchar_t actual[32768]{},color[256]{},size[256]{};
    result.readHr=GetCurrentThemeName(actual,32768,color,256,size,256);
    if(FAILED(result.readHr))return result.readHr;
    result.verified=_wcsicmp(actual,style.c_str())==0;
    // A matching filename alone never overrides a failed ThemeManager/font call.
    return result.verified?S_OK:HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
}
}
inline HRESULT Apply(Result& result) noexcept {
    result={};HRESULT hr=E_FAIL;
    try {
        // rundll32's COM apartment is not our contract. Own an STA and clean it
        // up before returning; never initialize COM under the loader lock.
        std::thread worker([&]() noexcept {try{hr=Detail::ApplyOnSta(result);}catch(...){hr=E_FAIL;}});
        worker.join();
    }catch(...){hr=E_FAIL;}
    return hr;
}
}
