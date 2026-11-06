#include "GlassSecureUxPackage.h"
#include "GlassSecureUxRelease.h"
#include "GlassReleaseClient.h"
#include "GlassMaintenanceCache.h"
#include <SecureUxThemeInventory.h>
#include "GlassInstallMsi.h"
#include "../InstallLog.h"
#include <msi.h>
#include <msiquery.h>
#include <msidefs.h>
#include <objbase.h>
#include <vector>
#include <array>

namespace GlassSecureUxPackage {
namespace {
using namespace GlassSecureUxRelease;
struct File {
    HANDLE h=INVALID_HANDLE_VALUE;
    ~File(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
    File()=default; File(const File&)=delete; File& operator=(const File&)=delete;
};
struct MsiHandle { MSIHANDLE h=0; ~MsiHandle(){if(h)MsiCloseHandle(h);} };
using GlassReleaseClient::Wide;
using GlassReleaseClient::Get;
bool Fail(std::wstring& error,const wchar_t* operation,DWORD code=GetLastError()){
    error=std::wstring(operation)+L" ("+std::to_wstring(code)+L")";return false;
}
bool Cancelled(const std::atomic<bool>& cancel,std::wstring& error){
    if(!cancel.load(std::memory_order_acquire))return false;
    Fail(error,L"SecureUxTheme: cancelled",ERROR_INSTALL_USEREXIT);return true;
}
Architecture NativeArchitecture(){
    SYSTEM_INFO si{};GetNativeSystemInfo(&si);
    if(si.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64)return Architecture::X64;
    if(si.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_ARM64)return Architecture::Arm64;
    if(si.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_INTEL)return Architecture::X86;
    throw std::runtime_error("Unsupported native machine architecture");
}
bool Hash(File& file,const Asset& asset,const std::atomic<bool>& cancel,std::wstring& error){
    return GlassReleaseClient::VerifyHash(file.h,asset.size,asset.sha256,cancel,error);
}
bool Property(MSIHANDLE db,const wchar_t* name,std::wstring& value){
    const std::wstring sql=L"SELECT `Value` FROM `Property` WHERE `Property`='"+std::wstring(name)+L"'";
    MsiHandle view,record;
    if(MsiDatabaseOpenViewW(db,sql.c_str(),&view.h)!=ERROR_SUCCESS||MsiViewExecute(view.h,0)!=ERROR_SUCCESS||
        MsiViewFetch(view.h,&record.h)!=ERROR_SUCCESS)return false;
    DWORD n=0;wchar_t empty=0;UINT rc=MsiRecordGetStringW(record.h,1,&empty,&n);
    if(rc!=ERROR_MORE_DATA||n>32767)return false;
    std::vector<wchar_t> data(n+1);++n;
    if(MsiRecordGetStringW(record.h,1,data.data(),&n)!=ERROR_SUCCESS)return false;
    value.assign(data.data(),n);return true;
}
bool Metadata(const std::wstring& path,Architecture arch,Package& package,std::wstring& error){
    MsiHandle db;const UINT rc=MsiOpenDatabaseW(path.c_str(),MSIDBOPEN_READONLY,&db.h);
    if(rc!=ERROR_SUCCESS)return Fail(error,L"Opening official MSI metadata",rc);
    std::wstring name,upgrade;GUID product{};
    if(!Property(db.h,L"ProductName",name)||!Property(db.h,L"UpgradeCode",upgrade)||
        !Property(db.h,L"ProductCode",package.productCode)||!Property(db.h,L"ProductVersion",package.version)||
        name!=L"SecureUxTheme"||_wcsicmp(upgrade.c_str(),UpgradeCode(arch))||
        FAILED(CLSIDFromString(package.productCode.c_str(),&product)))return Fail(error,L"Unexpected official MSI identity",ERROR_INVALID_DATA);
    MsiHandle summary;
    if(MsiGetSummaryInformationW(db.h,nullptr,0,&summary.h)!=ERROR_SUCCESS)return Fail(error,L"MSI summary",ERROR_INVALID_DATA);
    UINT type=0;INT number=0;FILETIME time{};wchar_t text[256]{};DWORD count=256;
    if(MsiSummaryInfoGetPropertyW(summary.h,PID_TEMPLATE,&type,&number,&time,text,&count)!=ERROR_SUCCESS)return Fail(error,L"MSI architecture",ERROR_INVALID_DATA);
    std::wstring machine=text;machine=machine.substr(0,machine.find(L';'));
    const wchar_t* expected=arch==Architecture::X64?L"x64":arch==Architecture::Arm64?L"Arm64":L"Intel";
    if(_wcsicmp(machine.c_str(),expected))return Fail(error,L"MSI architecture differs from native OS",ERROR_BAD_EXE_FORMAT);
    return true;
}
bool CopySnapshot(File& source,const std::wstring& path,const std::atomic<bool>& cancel,std::wstring& error){
    File dest;dest.h=GlassReleaseClient::CreatePrivateFile(path);
    if(dest.h==INVALID_HANDLE_VALUE)return Fail(error,L"Creating private official MSI snapshot");
    std::array<BYTE,32768> data{};
    for(;;){
        if(Cancelled(cancel,error))return false;
        DWORD read=0,written=0;
        if(!ReadFile(source.h,data.data(),static_cast<DWORD>(data.size()),&read,nullptr))return Fail(error,L"Reading MSI snapshot");
        if(!read)break;
        if(!WriteFile(dest.h,data.data(),read,&written,nullptr)||written!=read)return Fail(error,L"Writing MSI snapshot");
    }
    return FlushFileBuffers(dest.h)?true:Fail(error,L"Flushing MSI snapshot");
}
}
bool CheckLegacyRepackAbsent(std::wstring& error) noexcept {
    try {
        for(const wchar_t* code:{GlassInstallMsi::kProductSecureUxThemeX64,GlassInstallMsi::kProductSecureUxThemeX86}){
            if(GlassInstallMsi::IsProductInstalled(code)){error=L"Remove the old Glass-repacked SecureUxTheme explicitly before installing the author's original MSI.";return false;}
        }
        return true;
    }catch(...){return Fail(error,L"Checking SecureUxTheme",ERROR_NOT_ENOUGH_MEMORY);}
}
bool Prepare(const std::wstring& privateDirectory,const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept {
    package={};std::wstring partial,staged;
    try {
        if(Cancelled(cancel,error)||!CheckLegacyRepackAbsent(error))return false;
        std::vector<std::wstring> installed;
        const UINT inventory=Shared::SecureUxThemeInventory::Query(installed);
        if(inventory!=ERROR_SUCCESS)return Fail(error,L"SecureUxTheme MSI inventory",inventory);
        if(!installed.empty()){
            package.alreadyInstalled=true;error.clear();
            InstallLog::Line(L"SecureUxTheme: existing official MSI retained; no download, repair or activation");
            return true;
        }
        const auto arch=NativeArchitecture();Asset asset=OfflineV4(arch);
        std::vector<wchar_t> exe(32768);const DWORD count=GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()));
        if(!count||count>=exe.size())return Fail(error,L"Locating GlassSetup.exe");
        const std::wstring module(exe.data(),count);const auto slash=module.find_last_of(L"\\/");
        if(slash==std::wstring::npos)return Fail(error,L"Locating installer directory",ERROR_INVALID_NAME);
        if(!GlassMaintenanceCache::TrustedDirectory(privateDirectory))return Fail(error,L"Untrusted package workspace",ERROR_ACCESS_DENIED);
        const auto directory=module.substr(0,slash+1);
        const std::vector<std::wstring> candidates={directory+L"packages\\"+Wide(asset.name),
            directory+Wide(asset.name),GlassMaintenanceCache::CurrentFile(Wide(asset.name).c_str())};
        File local;std::wstring selected;
        const auto findLocal=[&]() {
            for(const auto& path:candidates) {
                if(path.empty())continue;
                if(local.h!=INVALID_HANDLE_VALUE){CloseHandle(local.h);local.h=INVALID_HANDLE_VALUE;}
                local.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                if(local.h!=INVALID_HANDLE_VALUE&&Hash(local,asset,cancel,error)){selected=path;return true;}
                if(cancel.load())return false;
            }
            if(local.h!=INVALID_HANDLE_VALUE){CloseHandle(local.h);local.h=INVALID_HANDLE_VALUE;}
            return false;
        };
        // Known official v4 assets remain usable offline. Unknown local files
        // are never modified; a missing/invalid source can use a private download.
        if(!findLocal()) {
            if(Cancelled(cancel,error))return false;
            error.clear();std::string json,parseError;
            if(!Get(L"https://api.github.com/repos/namazso/SecureUxTheme/releases/latest",INVALID_HANDLE_VALUE,&json,4*1024*1024,cancel,error))return false;
            if(!Select(json,arch,asset,parseError)){error=L"SecureUxTheme release discovery: "+Wide(parseError);return false;}
            if(!findLocal()) {
                if(Cancelled(cancel,error))return false;
                error.clear();partial=privateDirectory+L"\\"+Wide(asset.name)+L".part";
                {
                    File download;download.h=GlassReleaseClient::CreatePrivateFile(partial);
                    if(download.h==INVALID_HANDLE_VALUE||!Get(Wide(asset.url),download.h,nullptr,asset.size,cancel,error)||
                        !FlushFileBuffers(download.h))throw std::runtime_error("Cannot download original MSI into protected workspace");
                }
                local.h=CreateFileW(partial.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                if(local.h==INVALID_HANDLE_VALUE||!Hash(local,asset,cancel,error))throw std::runtime_error("Original MSI download verification failed");
                selected=partial;
            }
        }
        staged=privateDirectory+L"\\"+Wide(asset.name);
        if(!CopySnapshot(local,staged,cancel,error)||!Metadata(staged,arch,package,error))
            throw std::runtime_error("Cannot prepare verified original MSI snapshot");
        package.path=staged;error.clear();
        if(!partial.empty()){CloseHandle(local.h);local.h=INVALID_HANDLE_VALUE;DeleteFileW(partial.c_str());partial.clear();}
        InstallLog::LineFormat(L"SecureUxTheme original MSI: %s; release %s; ProductCode %s; version %s",selected.c_str(),Wide(asset.tag).c_str(),package.productCode.c_str(),package.version.c_str());
        return true;
    }catch(const std::exception& e){error=L"SecureUxTheme: "+Wide(e.what());}
    catch(...){Fail(error,L"Preparing official SecureUxTheme",ERROR_NOT_ENOUGH_MEMORY);}
    if(!partial.empty())DeleteFileW(partial.c_str());if(!staged.empty())DeleteFileW(staged.c_str());package={};return false;
}
}
