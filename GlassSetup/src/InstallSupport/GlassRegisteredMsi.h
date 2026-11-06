#pragma once
#include "GlassInstallMsi.h"
#include <wincrypt.h>
#include <array>
#include <cwchar>

// Shared execution-boundary guard. A fixed ProductCode/root alone does not
// prove the cached package is still the one validated before the operation.
namespace GlassRegisteredMsi {
inline bool Hash(const std::wstring& path,std::array<BYTE,32>& digest,const std::atomic_bool* cancelled=nullptr) noexcept {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;
    bool ok=GetFileInformationByHandle(file,&info) && !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) && info.nNumberOfLinks==1;
    ok=ok && CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT) && CryptCreateHash(provider,CALG_SHA_256,0,0,&hash);
    BYTE bytes[65536]{};DWORD count=0;
    while(ok) {
        if(cancelled && cancelled->load()){ok=false;break;}
        if(!ReadFile(file,bytes,sizeof(bytes),&count,nullptr)){ok=false;break;}
        if(!count)break;
        if(!CryptHashData(hash,bytes,count,0)){ok=false;break;}
    }
    DWORD length=static_cast<DWORD>(digest.size());
    if(ok)ok=CryptGetHashParam(hash,HP_HASHVAL,digest.data(),&length,0) && length==digest.size();
    if(hash)CryptDestroyHash(hash);
    if(provider)CryptReleaseContext(provider,0);
    CloseHandle(file);return ok;
}
inline bool Recheck(const wchar_t* code,const std::wstring& root,bool expectedPresent,
    const std::wstring& baseline,const std::wstring& workspace,std::wstring& detail,
    const std::atomic_bool* cancelled=nullptr) noexcept {
    detail.clear();
    if(!code || wcslen(code)!=38 || code[0]!=L'{' || code[37]!=L'}' || workspace.empty()) {
        detail=L"Missing MSI identity or protected workspace";return false;
    }
    for(unsigned i=1;i<37;++i) {
        const wchar_t c=code[i];
        const bool separator=i==9 || i==14 || i==19 || i==24;
        if(separator?c!=L'-':!((c>=L'0'&&c<=L'9')||(c>=L'A'&&c<=L'F')||(c>=L'a'&&c<=L'f'))) {
            detail=L"Invalid MSI identity";return false;
        }
    }
    if(cancelled && cancelled->load())return false;
    if(GlassInstallMsi::CheckProductContexts(code,detail,cancelled)!=GlassInstallMsi::ProductPathCheck::Safe)return false;
    if(GlassInstallMsi::IsProductInstalled(code)!=expectedPresent){detail=L"MSI presence changed after preflight";return false;}
    if(!expectedPresent)return true;
    if(root.empty() || baseline.empty()){detail=L"Missing installed-product baseline";return false;}
    const auto current=workspace+L"\\recheck-"+code+L".msi";
    const bool copied=GlassInstallMsi::CopyLocalPackage(code,current,cancelled);
    bool ok=false;
    if(copied) {
        std::array<BYTE,32> before{},now{};
        ok=Hash(baseline,before,cancelled) && Hash(current,now,cancelled) && before==now;
        if(!ok)detail=L"Registered MSI bytes/identity changed after preflight";
        if(ok)ok=GlassInstallMsi::CheckProductFileRoots(code,current,root,detail,cancelled)==GlassInstallMsi::ProductPathCheck::Safe;
    } else detail=L"Current cached MSI is unavailable";
    DeleteFileW(current.c_str());
    return ok && !(cancelled && cancelled->load());
}
}
