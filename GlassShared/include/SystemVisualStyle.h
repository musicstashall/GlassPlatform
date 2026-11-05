#pragma once
#include <windows.h>
#include <uxtheme.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <string>
#pragma comment(lib,"uxtheme.lib")
#pragma comment(lib,"wintrust.lib")
#pragma comment(lib,"crypt32.lib")

namespace Shared::SystemVisualStyle {
enum class State { Stock, ResetRequired, Unknown };
// A theme filename is never evidence. Require the exact Windows stock resource,
// no reparse traversal, and a valid offline Authenticode trust result. Failure to
// assess a Windows resource is Unknown, not permission to reset it.
inline State ClassifyPath(const wchar_t* path) {
    if(!path||!*path)return State::Unknown;
    wchar_t expanded[32768]{},full[32768]{},windows[32768]{};
    const DWORD n=ExpandEnvironmentStringsW(path,expanded,32768);
    if(!n||n>=32768||!GetWindowsDirectoryW(windows,32768))return State::Unknown;
    const DWORD count=GetFullPathNameW(expanded,32768,full,nullptr);
    if(!count||count>=32768)return State::Unknown;
    const DWORD attributes=GetFileAttributesW(full);
    if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return State::Unknown;
    const std::wstring aero=std::wstring(windows)+L"\\Resources\\Themes\\Aero\\aero.msstyles";
    const std::wstring lite=std::wstring(windows)+L"\\Resources\\Themes\\aero\\aerolite.msstyles";
    if(_wcsicmp(full,aero.c_str())!=0&&_wcsicmp(full,lite.c_str())!=0)return State::ResetRequired;
    const std::wstring absolute=full;
    for(std::size_t i=3;i<absolute.size();++i)if(absolute[i]==L'\\') {
        const DWORD a=GetFileAttributesW(absolute.substr(0,i).c_str());
        if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))return State::Unknown;
    }
    WINTRUST_FILE_INFO info{};info.cbStruct=sizeof(info);info.pcwszFilePath=full;
    WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE;trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&info;
    trust.dwStateAction=WTD_STATEACTION_VERIFY;trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status=WinVerifyTrust(nullptr,&policy,&trust);
    bool microsoft=false;
    if(status==ERROR_SUCCESS) {
        auto* provider=WTHelperProvDataFromStateData(trust.hWVTStateData);
        auto* signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
        if(signer&&signer->csCertChain&&signer->pasCertChain&&signer->pasCertChain[0].pCert) {
            wchar_t organization[256]{};
            CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_ATTR_TYPE,0,const_cast<char*>(szOID_ORGANIZATION_NAME),organization,256);
            microsoft=_wcsicmp(organization,L"Microsoft Corporation")==0;
        }
    }
    trust.dwStateAction=WTD_STATEACTION_CLOSE;(void)WinVerifyTrust(nullptr,&policy,&trust);
    return status==ERROR_SUCCESS&&microsoft?State::Stock:State::Unknown;
}
inline bool IsStockMsStylesFileName(const wchar_t* path) noexcept {
    if(!path||!*path)return true;
    const wchar_t* name=path;
    for(const wchar_t* p=path;*p;++p)if(*p==L'\\')name=p+1;
    return _wcsicmp(name,L"aero.msstyles")==0||_wcsicmp(name,L"aerolite.msstyles")==0;
}
// GlassSettings parity: stock by base name; no Authenticode gate for apply/reset.
inline State CurrentForMaintenance() noexcept {
    if(!IsThemeActive())return State::Stock;
    wchar_t style[32768]{},color[256]{},size[256]{};
    if(FAILED(GetCurrentThemeName(style,32768,color,256,size,256)))return State::Unknown;
    return IsStockMsStylesFileName(style)?State::Stock:State::ResetRequired;
}
inline State Current() {
    if(!IsThemeActive())return State::Stock;
    wchar_t style[32768]{},color[256]{},size[256]{};
    if(FAILED(GetCurrentThemeName(style,32768,color,256,size,256)))return State::Unknown;
    return ClassifyPath(style);
}
}
