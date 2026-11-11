#pragma once
#include "GlassMaintenanceCache.h"
#include "GlassPackageRules.h"
#include "../../third_party/nlohmann/json.hpp"
#include <wincrypt.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

// File-product ownership is independent of MSI. The receipt authorizes exactly
// these three names in an administrator-owned root, never paths from JSON.
namespace GlassOpenGlassFiles {
inline constexpr const wchar_t* Names[]={L"OpenGlass.dll",L"OpenGlassHost.exe",L"OpenGlassGUI.exe",L"GlassOpenGlass.receipt.json"};
inline constexpr const char* Ids[]={"dll","host","gui"};
using Bytes=std::vector<BYTE>;
inline void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Handle {
    HANDLE h=INVALID_HANDLE_VALUE;
    Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& o) noexcept:h(o.h){o.h=INVALID_HANDLE_VALUE;}
    ~Handle(){Close();}
    void Close(){if(h!=INVALID_HANDLE_VALUE){CloseHandle(h);h=INVALID_HANDLE_VALUE;}}
};
inline bool Missing(const std::wstring& path) {
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return false;
    const auto error=GetLastError();Require(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND,"OpenGlass path is inaccessible");return true;
}
inline void Check(HANDLE h,bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    Require(h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(h,&info)&&
        !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
        ((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)==directory&&
        (directory||info.nNumberOfLinks==1),"OpenGlass object/link validation failed");
}
inline std::vector<Handle> PinRoot(const std::wstring& path) {
    Require(GlassInstallUtil::IsSafeInstallPath(path)&&path.size()>3&&path[1]==L':'&&path[2]==L'\\',"Invalid OpenGlass root");
    std::vector<Handle> pins;
    for(std::size_t end=3;;) {
        Handle h;h.h=CreateFileW(path.substr(0,end).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
            OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        Check(h.h,true);pins.push_back(std::move(h));if(end==path.size())break;
        const auto next=path.find(L'\\',end==3?3:end+1);end=next==std::wstring::npos?path.size():next;
    }
    Require(GlassMaintenanceCache::TrustedDirectory(path),"OpenGlass root is not administrator-owned");return pins;
}
inline Bytes Read(HANDLE h,std::size_t limit=32u*1024*1024) {
    LARGE_INTEGER size{},zero{};Require(GetFileSizeEx(h,&size)&&size.QuadPart>=0&&static_cast<std::uint64_t>(size.QuadPart)<=limit,"OpenGlass file size limit");
    Require(SetFilePointerEx(h,zero,nullptr,FILE_BEGIN)!=FALSE,"OpenGlass seek failed");
    Bytes data(static_cast<std::size_t>(size.QuadPart));DWORD count=0;
    Require(data.empty()||(ReadFile(h,data.data(),static_cast<DWORD>(data.size()),&count,nullptr)&&count==data.size()),"OpenGlass file read failed");return data;
}
inline Bytes Read(const std::wstring& path,std::size_t limit=32u*1024*1024) {
    Require(GlassMaintenanceCache::TrustedPath(path,false),"Untrusted OpenGlass file ACL");
    Handle file;file.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    Check(file.h,false);return Read(file.h,limit);
}
inline std::string Digest(const Bytes& data) {
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;
    Require(CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)!=FALSE,"OpenGlass hash provider");
    const bool made=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)!=FALSE;
    BYTE digest[32]{};DWORD size=32;
    const bool ok=made&&CryptHashData(hash,data.data(),static_cast<DWORD>(data.size()),0)&&CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0)&&size==32;
    if(hash)CryptDestroyHash(hash);CryptReleaseContext(provider,0);Require(ok,"OpenGlass SHA256 failed");
    const char digits[]="0123456789abcdef";std::string result;
    for(auto byte:digest){result+=digits[byte>>4];result+=digits[byte&15];}return result;
}
struct Record {bool present=false,integrity=false,milComp=false;std::wstring version;};
inline Record Inspect(const std::wstring& root) {
    Record result;if(root.empty()||Missing(root+L"\\"+Names[3]))return result;
    auto pins=PinRoot(root);const auto bytes=Read(root+L"\\"+Names[3],65536);
    const auto json=nlohmann::json::parse(bytes.begin(),bytes.end());
    Require(json.at("format")=="GlassPlatform.OpenGlass"&&json.at("schema")==1,"Unknown OpenGlass receipt");
    const auto version=json.at("version").get<std::string>();GlassPackageRules::Version numeric;
    Require(GlassPackageRules::ParseVersion(version,numeric),"Invalid OpenGlass receipt version");
    result.version.assign(version.begin(),version.end());result.milComp=json.at("milComp").get<bool>();
    result.present=true;result.integrity=true;
    for(unsigned i=0;i<3;++i) {
        const auto expected=json.at("sha256").at(Ids[i]).get<std::string>();
        Require(expected.size()==64&&std::all_of(expected.begin(),expected.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"Invalid OpenGlass receipt hash");
        if(Missing(root+L"\\"+Names[i])){result.integrity=false;continue;}
        if(Digest(Read(root+L"\\"+Names[i]))!=expected)result.integrity=false;
    }
    return result;
}
inline bool HasReceipt(const std::wstring& root) noexcept {
    if(root.empty())return false;
    const auto path=root+L"\\"+Names[3];const auto attr=GetFileAttributesW(path.c_str());
    if(attr!=INVALID_FILE_ATTRIBUTES)return true;
    const auto error=GetLastError();return error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND;
}
// Exact-handle backup of fixed targets, plus pinned ancestors. Captured before
// writing. Rollback checks unchanged postimages after handles were released.
class Transaction {
    struct Slot {std::wstring path;bool existed=false,touched=false,removed=false;Bytes before,after;Handle handle;};
    std::vector<Handle> pins_;std::vector<Slot> slots_;
    static void Write(HANDLE h,const Bytes& data) {
        LARGE_INTEGER zero{};Require(SetFilePointerEx(h,zero,nullptr,FILE_BEGIN)!=FALSE,"OpenGlass output seek");DWORD count=0;
        Require((data.empty()||(WriteFile(h,data.data(),static_cast<DWORD>(data.size()),&count,nullptr)&&count==data.size()))&&
            SetEndOfFile(h)&&FlushFileBuffers(h),"OpenGlass output write/flush");
    }
public:
    bool Begin(const std::wstring& root,std::wstring& error) noexcept {
        try {
            if(Missing(root)) {
                Require(GlassInstallUtil::IsSafeInstallPath(root),"Unsafe OpenGlass destination");
                // Pin the existing trusted ancestor before creating anything;
                // otherwise a junction could redirect directory creation.
                auto ancestor=root;
                do {
                    const auto slash=ancestor.find_last_of(L'\\');
                    Require(slash!=std::wstring::npos&&slash>2,"No trusted OpenGlass parent");
                    ancestor.resize(slash);
                } while(Missing(ancestor));
                auto parentPins=PinRoot(ancestor);
                const auto status=SHCreateDirectoryExW(nullptr,root.c_str(),nullptr);
                Require(status==ERROR_SUCCESS||status==ERROR_ALREADY_EXISTS||status==ERROR_FILE_EXISTS,"Cannot create OpenGlass root");
            }
            pins_=PinRoot(root);const auto receipt=Inspect(root);
            if(!receipt.present)for(unsigned i=0;i<3;++i)
                Require(Missing(root+L"\\"+Names[i]),"Foreign OpenGlass files: no Glass ownership receipt");
            for(auto name:Names) {
                Slot slot;slot.path=root+L"\\"+name;slot.existed=!Missing(slot.path);
                if(slot.existed)Require(GlassMaintenanceCache::TrustedPath(slot.path,false),"Untrusted OpenGlass target");
                slot.handle.h=CreateFileW(slot.path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,
                    slot.existed?OPEN_EXISTING:CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                slot.touched=!slot.existed&&slot.handle.h!=INVALID_HANDLE_VALUE;
                slots_.push_back(std::move(slot));
                auto& saved=slots_.back();Check(saved.handle.h,false);saved.before=Read(saved.handle.h);
            }
            return true;
        }catch(const std::exception& e){error.assign(e.what(),e.what()+std::strlen(e.what()));return false;}
         catch(...){error=L"Cannot prepare OpenGlass file transaction";return false;}
    }
    bool Apply(const std::wstring& version,bool milComp,const std::array<std::wstring,3>& sources,std::wstring& error) noexcept {
        try {
            Require(slots_.size()==4,"OpenGlass transaction not prepared");std::array<Bytes,3> bytes;
            nlohmann::json receipt={{"format","GlassPlatform.OpenGlass"},{"schema",1},{"version",std::string(version.begin(),version.end())},{"milComp",milComp}};
            for(unsigned i=0;i<3;++i){bytes[i]=Read(sources[i]);receipt["sha256"][Ids[i]]=Digest(bytes[i]);}
            const auto text=receipt.dump();
            for(unsigned i=0;i<4;++i) {
                auto& slot=slots_[i];slot.after=i<3?std::move(bytes[i]):Bytes(text.begin(),text.end());slot.touched=true;
                Write(slot.handle.h,slot.after);Require(Read(slot.handle.h)==slot.after,"OpenGlass output readback failed");
            }
            // RegisterSelectedProducts must be able to inspect executable paths.
            for(auto& slot:slots_)slot.handle.Close();return true;
        }catch(const std::exception& e){error.assign(e.what(),e.what()+std::strlen(e.what()));return false;}
         catch(...){error=L"OpenGlass file transaction failed";return false;}
    }
    bool Remove(std::wstring& error) noexcept {
        try {
            Require(slots_.size()==4,"OpenGlass removal not prepared");
            for(auto& slot:slots_) {
                FILE_DISPOSITION_INFO disposition{TRUE};
                Require(SetFileInformationByHandle(slot.handle.h,FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE,"OpenGlass file removal failed");
                slot.touched=true;slot.removed=true;slot.handle.Close();
            }
            return true;
        }catch(const std::exception& e){error.assign(e.what(),e.what()+std::strlen(e.what()));return false;}
         catch(...){error=L"OpenGlass removal failed";return false;}
    }
    bool Rollback() noexcept {
        try {
            for(auto it=slots_.rbegin();it!=slots_.rend();++it) {
                auto& slot=*it;if(!slot.touched)continue;
                if(slot.removed) {
                    Require(Missing(slot.path),"Rollback refuses replaced removed OpenGlass file");
                    if(!slot.existed){slot.touched=false;continue;}
                    slot.handle.h=CreateFileW(slot.path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                    Check(slot.handle.h,false);Write(slot.handle.h,slot.before);slot.handle.Close();slot.touched=false;continue;
                }
                if(slot.handle.h==INVALID_HANDLE_VALUE) {
                    slot.handle.h=CreateFileW(slot.path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                    Check(slot.handle.h,false);Require(Read(slot.handle.h)==slot.after,"Rollback refuses changed OpenGlass postimage");
                }
                if(slot.existed)Write(slot.handle.h,slot.before);
                else {FILE_DISPOSITION_INFO disposition{TRUE};Require(SetFileInformationByHandle(slot.handle.h,FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE,"Rollback new OpenGlass deletion");}
                slot.handle.Close();slot.touched=false;
            }
            slots_.clear();pins_.clear();return true;
        }catch(...){return false;}
    }
    void Commit(){slots_.clear();pins_.clear();}
};
}
