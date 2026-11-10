#include "GlassSetupSourceLayout.h"
#include "GlassSecureUxPackage.h"
#include "GlassSecureUxRelease.h"
#include "GlassReleaseClient.h"
#include "GlassMaintenanceCache.h"
#include <SecureUxThemeInventory.h>
#include "GlassInstallMsi.h"
#include "GlassRegisteredMsi.h"
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
    GlassPackageRules::Version numeric;
    if(std::any_of(package.version.begin(),package.version.end(),[](wchar_t c){return static_cast<unsigned>(c)>127;}) ||
       !GlassPackageRules::ParseVersion(std::string(package.version.begin(),package.version.end()),numeric))
        return Fail(error,L"Invalid official MSI ProductVersion",ERROR_INVALID_DATA);
    MsiHandle summary;
    if(MsiGetSummaryInformationW(db.h,nullptr,0,&summary.h)!=ERROR_SUCCESS)return Fail(error,L"MSI summary",ERROR_INVALID_DATA);
    UINT type=0;INT number=0;FILETIME time{};wchar_t text[256]{};DWORD count=256;
    if(MsiSummaryInfoGetPropertyW(summary.h,PID_TEMPLATE,&type,&number,&time,text,&count)!=ERROR_SUCCESS)return Fail(error,L"MSI architecture",ERROR_INVALID_DATA);
    std::wstring machine=text;machine=machine.substr(0,machine.find(L';'));
    const wchar_t* expected=arch==Architecture::X64?L"x64":arch==Architecture::Arm64?L"Arm64":L"Intel";
    if(_wcsicmp(machine.c_str(),expected))return Fail(error,L"MSI architecture differs from native OS",ERROR_BAD_EXE_FORMAT);
    return true;
}
bool NewerMsiVersion(const std::wstring& lhs,const std::wstring& rhs) {
    GlassPackageRules::Version a,b;
    const auto parse=[](const std::wstring& text,GlassPackageRules::Version& version) {
        for(wchar_t c:text)if(c>127)return false;
        return GlassPackageRules::ParseVersion(std::string(text.begin(),text.end()),version);
    };
    return parse(lhs,a)&&parse(rhs,b)&&GlassPackageRules::Compare(a,b)>0;
}
std::vector<std::wstring> CandidatePaths(const Asset& asset) {
    wchar_t exe[32768]{};const DWORD count=GetModuleFileNameW(nullptr,exe,32768);
    if(!count||count>=32768)throw std::runtime_error("Cannot locate GlassSetup.exe");
    const std::wstring module(exe,count);const auto slash=module.find_last_of(L"\\/");
    if(slash==std::wstring::npos)throw std::runtime_error("Cannot locate installer directory");
    auto directory=module.substr(0,slash+1);
    if(!GlassSetupSourceLayout::directory.empty())directory=GlassSetupSourceLayout::directory+L"\\";
    const auto name=Wide(asset.name);
    return {directory+L"packages\\"+name,directory+name,
        GlassMaintenanceCache::CurrentFile(name.c_str()),GlassSetupSourceLayout::Prepared(5)};
}
// Paths are classified by the same original-root enumerator, not by a
// UI pathname heuristic. Equal versions still require independent hash checks.
bool PreferCandidate(const Package& candidate,const std::wstring& path,const Asset& candidateAsset,
    const Package& current,const std::wstring& currentPath,const Asset& currentAsset) {
    if(NewerMsiVersion(candidate.version,current.version))return true;
    if(NewerMsiVersion(current.version,candidate.version))return false;
    const auto a=CandidatePaths(candidateAsset),b=CandidatePaths(currentAsset);
    const bool attached=path==a[0]||path==a[1];
    const bool currentAttached=currentPath==b[0]||currentPath==b[1];
    return attached&&!currentAttached;
}
bool FetchAsset(Architecture arch,const std::atomic<bool>& cancel,Asset& asset,std::wstring& error) {
    if(cancel.load())return false;
    std::string json,parseError;
    {std::lock_guard<std::mutex> lock(GlassSetupSourceLayout::secureUxCatalogMutex);json=GlassSetupSourceLayout::secureUxCatalogText;}
    if(json.empty()&&!Get(L"https://api.github.com/repos/namazso/SecureUxTheme/releases/latest",INVALID_HANDLE_VALUE,&json,
        4*1024*1024,cancel,error))return false;
    if(!Select(json,arch,asset,parseError)){error=L"SecureUxTheme release discovery: "+Wide(parseError);return false;}
    // Pin validated vendor metadata for this session too: Apply must not require
    // another online discovery after the selected package is already verified.
    {std::lock_guard<std::mutex> lock(GlassSetupSourceLayout::secureUxCatalogMutex);GlassSetupSourceLayout::secureUxCatalogText=json;}
    return !cancel.load();
}
bool FindLocal(const Asset& asset,Architecture arch,const std::atomic<bool>& cancel,
    File& file,std::wstring& selected,Package& metadata,std::wstring& error,bool attachedOnly=false) {
    const auto paths=CandidatePaths(asset);
    for(std::size_t index=0;index<paths.size();++index) {
        if(attachedOnly&&index>=2)continue;
        const auto& path=paths[index];
        if(cancel.load())return false;
        if(path.empty())continue;
        if(file.h!=INVALID_HANDLE_VALUE){CloseHandle(file.h);file.h=INVALID_HANDLE_VALUE;}
        file.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file.h==INVALID_HANDLE_VALUE)continue;
        Package candidate;
        // The locked handle denies concurrent writes/deletes. Verify exact
        // official bytes, type/link count and native MSI identity before display.
        if(Hash(file,asset,cancel,error)&&Metadata(path,arch,candidate,error)) {
            selected=path;metadata=std::move(candidate);error.clear();return true;
        }
        InstallLog::LineFormat(L"SecureUxTheme local MSI rejected: %s: %s",path.c_str(),error.c_str());
    }
    if(file.h!=INVALID_HANDLE_VALUE){CloseHandle(file.h);file.h=INVALID_HANDLE_VALUE;}
    return false;
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
            // ABSENT is not installed presence, but can still belong to another
            // user. Never allow original MSI acquisition over a foreign repack.
            if(GlassInstallMsi::CheckProductContexts(code,error)!=GlassInstallMsi::ProductPathCheck::Safe)return false;
            if(GlassInstallMsi::IsProductInstalled(code)){error=L"Remove the old Glass-repacked SecureUxTheme explicitly before installing the author's original MSI.";return false;}
        }
        return true;
    }catch(...){return Fail(error,L"Checking SecureUxTheme",ERROR_NOT_ENOUGH_MEMORY);}
}
bool QueryInstallPresence(std::vector<std::wstring>& installed,const std::atomic<bool>& cancel,std::wstring& error) noexcept {
    installed.clear();
    try {
        std::vector<std::wstring> candidates;
        const UINT status=Shared::SecureUxThemeInventory::Query(candidates);
        if(status!=ERROR_SUCCESS)return Fail(error,L"SecureUxTheme MSI inventory",status);
        for(const auto& code:candidates) {
            if(Cancelled(cancel,error) || GlassInstallMsi::CheckProductContexts(code.c_str(),error,&cancel)!=GlassInstallMsi::ProductPathCheck::Safe){installed.clear();return false;}
            const auto state=MsiQueryProductStateW(code.c_str());
            if(state==INSTALLSTATE_DEFAULT)installed.push_back(code);
            else if(state!=INSTALLSTATE_ABSENT && state!=INSTALLSTATE_UNKNOWN){installed.clear();return Fail(error,L"SecureUxTheme MSI state changed",ERROR_RETRY);}
        }
        error.clear();return !Cancelled(cancel,error);
    }catch(...){installed.clear();return Fail(error,L"Reading SecureUxTheme installation state",ERROR_NOT_ENOUGH_MEMORY);}
}
bool Describe(const std::atomic<bool>& cancel,Offer& offer,std::wstring& diagnostic) noexcept {
    offer={};diagnostic.clear();
    try {
        if(Cancelled(cancel,diagnostic))return false;
        const auto arch=NativeArchitecture();const auto known=OfflineV4(arch);
        File local;Package metadata;std::wstring selected,error;
        bool found=FindLocal(known,arch,cancel,local,selected,metadata,error);
        Asset remote;const bool online=!cancel.load()&&FetchAsset(arch,cancel,remote,diagnostic);
        if(cancel.load()){offer={};return false;}
        if(online) {
            offer.releaseTag=Wide(remote.tag);offer.releaseVersion=Wide(ReleaseVersion(remote.tag));offer.releaseSize=remote.size;
            File other;std::wstring path;Package candidate;
            if(FindLocal(remote,arch,cancel,other,path,candidate,error)&&(!found||PreferCandidate(candidate,path,remote,metadata,selected,known))) {
                std::swap(local.h,other.h);selected=path;metadata=candidate;found=true;
            }
        }
        // Attached media is a separate fact, never inferred from the selected cache.
        File attached;std::wstring path;Package candidate;
        if(FindLocal(known,arch,cancel,attached,path,candidate,error,true)) {
            offer.attachedVersion=candidate.version;offer.attachedPath=path;
        }
        if(online&&FindLocal(remote,arch,cancel,attached,path,candidate,error,true)&&
            (offer.attachedVersion.empty()||NewerMsiVersion(candidate.version,offer.attachedVersion))) {
            offer.attachedVersion=candidate.version;offer.attachedPath=path;
        }
        if(cancel.load()){offer={};return false;}
        if(found) {
            offer.localVerified=true;offer.localVersion=metadata.version;offer.localPath=selected;
            LARGE_INTEGER size{};if(GetFileSizeEx(local.h,&size)&&size.QuadPart>0)offer.localSize=static_cast<std::uint64_t>(size.QuadPart);
            InstallLog::LineFormat(L"SecureUxTheme offer: local MSI %s; ProductVersion %s",selected.c_str(),metadata.version.c_str());
        }
        return found||online;
    }catch(...){offer={};return Fail(diagnostic,L"Describing SecureUxTheme",ERROR_INVALID_DATA);}
}
bool Prepare(const std::wstring& privateDirectory,const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept {
    package={};std::wstring partial,staged;
    try {
        if(Cancelled(cancel,error)||!CheckLegacyRepackAbsent(error))return false;
        std::vector<std::wstring> installed;
        if(!QueryInstallPresence(installed,cancel,error))return false;
        if(!installed.empty()){
            package.alreadyInstalled=true;error.clear();
            InstallLog::Line(L"SecureUxTheme: existing official MSI retained; no download, repair or activation");
            return true;
        }
        const auto arch=NativeArchitecture();Asset asset=OfflineV4(arch);
        if(!GlassMaintenanceCache::TrustedDirectory(privateDirectory))return Fail(error,L"Untrusted package workspace",ERROR_ACCESS_DENIED);
        File local;std::wstring selected;Package localMetadata;
        bool found=FindLocal(asset,arch,cancel,local,selected,localMetadata,error);
        Asset remote;const bool online=FetchAsset(arch,cancel,remote,error);
        if(Cancelled(cancel,error))return false;
        if(online) {
            File candidate;std::wstring candidatePath;Package candidateMetadata;
            bool remoteLocal=FindLocal(remote,arch,cancel,candidate,candidatePath,candidateMetadata,error);
            // A release tag is NOT MSI ProductVersion. For different bytes the
            // remote MSI must be verified and inspected before ordering versions.
            if(!remoteLocal&&(!found||remote.sha256!=asset.sha256)) {
                partial=privateDirectory+L"\\"+Wide(remote.name)+L".part";
                {
                    File download;download.h=GlassReleaseClient::CreatePrivateFile(partial);
                    if(download.h==INVALID_HANDLE_VALUE||!Get(Wide(remote.url),download.h,nullptr,remote.size,cancel,error)||
                        !FlushFileBuffers(download.h))throw std::runtime_error("Cannot download original MSI into protected workspace");
                }
                candidate.h=CreateFileW(partial.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                if(candidate.h==INVALID_HANDLE_VALUE||!Hash(candidate,remote,cancel,error)||!Metadata(partial,arch,candidateMetadata,error))
                    throw std::runtime_error("Original MSI download verification failed");
                candidatePath=partial;remoteLocal=true;
            }
            if(remoteLocal&&(!found||PreferCandidate(candidateMetadata,candidatePath,remote,localMetadata,selected,asset))) {
                std::swap(local.h,candidate.h);selected=candidatePath;localMetadata=candidateMetadata;asset=remote;found=true;
            }
        }
        if(!found){if(!online)return false;throw std::runtime_error("No verified original MSI candidate");}
        error.clear();
        staged=privateDirectory+L"\\"+Wide(asset.name);
        if(!CopySnapshot(local,staged,cancel,error)||!Metadata(staged,arch,package,error))
            throw std::runtime_error("Cannot prepare verified original MSI snapshot");
        const wchar_t* source=selected==partial?L"download":L"local";
        package.path=staged;error.clear();
        if(!partial.empty()){CloseHandle(local.h);local.h=INVALID_HANDLE_VALUE;DeleteFileW(partial.c_str());partial.clear();}
        InstallLog::LineFormat(L"SecureUxTheme original MSI: %s; release %s; ProductCode %s; version %s; source=%s",selected.c_str(),Wide(asset.tag).c_str(),package.productCode.c_str(),package.version.c_str(),source);
        return true;
    }catch(const std::exception& e){error=L"SecureUxTheme: "+Wide(e.what());}
    catch(...){Fail(error,L"Preparing official SecureUxTheme",ERROR_NOT_ENOUGH_MEMORY);}
    if(!partial.empty())DeleteFileW(partial.c_str());if(!staged.empty())DeleteFileW(staged.c_str());package={};return false;
}
namespace {
bool RemovalInventory(const std::wstring& expected,const std::atomic<bool>& cancel,std::wstring& error) {
    if(Cancelled(cancel,error) || expected.empty() || !CheckLegacyRepackAbsent(error))return false;
    std::vector<std::wstring> products;
    if(Shared::SecureUxThemeInventory::Query(products)!=ERROR_SUCCESS || products.size()!=1 ||
       _wcsicmp(products.front().c_str(),expected.c_str()) || MsiQueryProductStateW(expected.c_str())!=INSTALLSTATE_DEFAULT)
        return Fail(error,L"Official MSI inventory changed or is ambiguous",ERROR_INVALID_DATA);
    if(GlassInstallMsi::CheckProductContexts(expected.c_str(),error,&cancel)!=GlassInstallMsi::ProductPathCheck::Safe)return false;
    wchar_t product[39]{};MSIINSTALLCONTEXT context=MSIINSTALLCONTEXT_NONE;
    if(MsiEnumProductsExW(expected.c_str(),nullptr,MSIINSTALLCONTEXT_ALL,0,product,&context,nullptr,nullptr)!=ERROR_SUCCESS ||
       context!=MSIINSTALLCONTEXT_MACHINE)return Fail(error,L"Vendor MSI is not a machine product",ERROR_ACCESS_DENIED);
    return true;
}
bool CopyRemovalBaseline(const std::wstring& code,const std::wstring& destination,
    const std::atomic<bool>& cancel,Package& metadata,std::wstring& error) {
    // Cached MSI signatures/cabinets can be stripped. Trust only Windows'
    // protected Installer cache plus exact vendor family/native architecture.
    wchar_t cached[32768]{},windows[32768]{};DWORD size=_countof(cached);
    if(MsiGetProductInfoW(code.c_str(),INSTALLPROPERTY_LOCALPACKAGE,cached,&size)!=ERROR_SUCCESS)
        return Fail(error,L"Registered vendor MSI cache is unavailable",ERROR_INVALID_DATA);
    const UINT count=GetWindowsDirectoryW(windows,_countof(windows));
    const std::wstring path=cached;const auto slash=path.find_last_of(L"\\/");
    const auto parent=std::wstring(windows)+L"\\Installer";
    if(!count || count>=_countof(windows) || slash==std::wstring::npos ||
       _wcsicmp(path.substr(0,slash).c_str(),parent.c_str()) ||
       !GlassMaintenanceCache::TrustedDirectory(parent) || !GlassMaintenanceCache::TrustedPath(path,false))
        return Fail(error,L"Vendor cache is not a protected Windows Installer source",ERROR_ACCESS_DENIED);
    File locked;locked.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if(locked.h==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(locked.h,&info) ||
       (info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks!=1)
        return Fail(error,L"Cannot lock vendor cache safely",ERROR_INVALID_DATA);
    if(!CopySnapshot(locked,destination,cancel,error) || !Metadata(destination,NativeArchitecture(),metadata,error))return false;
    if(_wcsicmp(metadata.productCode.c_str(),code.c_str()))return Fail(error,L"Vendor cache ProductCode mismatch",ERROR_INVALID_DATA);
    wchar_t registeredPackage[256]{};DWORD packageSize=_countof(registeredPackage);
    const auto packageCode=GlassMaintenanceCache::PackageCode(destination);
    if(packageCode.empty() || MsiGetProductInfoW(code.c_str(),INSTALLPROPERTY_PACKAGECODE,registeredPackage,&packageSize)!=ERROR_SUCCESS ||
       _wcsicmp(packageCode.c_str(),registeredPackage))return Fail(error,L"Vendor cached PackageCode differs from registration",ERROR_INVALID_DATA);
    return true;
}
}
bool PrepareRemoval(const std::wstring& workspace,const std::wstring& expectedCode,
    const std::atomic<bool>& cancel,Removal& removal,std::wstring& error) noexcept {
    removal={};std::wstring baseline;
    try {
        if(!GlassMaintenanceCache::TrustedDirectory(workspace) || !RemovalInventory(expectedCode,cancel,error))return false;
        baseline=workspace+L"\\SecureUxTheme-removal-baseline.msi";
        Package metadata;
        if(CopyRemovalBaseline(expectedCode,baseline,cancel,metadata,error) &&
           GlassRegisteredMsi::Hash(baseline,removal.digest,&cancel)) {
            removal.productCode=metadata.productCode;removal.version=metadata.version;removal.baseline=baseline;
            removal.ready=true;error.clear();return true;
        }
    }catch(...){Fail(error,L"Preparing vendor removal baseline",ERROR_NOT_ENOUGH_MEMORY);}
    if(!baseline.empty())DeleteFileW(baseline.c_str());removal={};return false;
}
bool RecheckRemoval(const std::wstring& workspace,const std::atomic<bool>& cancel,
    const Removal& removal,std::wstring& error) noexcept {
    std::wstring current;
    try {
        if(!removal.ready || removal.baseline!=workspace+L"\\SecureUxTheme-removal-baseline.msi" ||
           !GlassMaintenanceCache::TrustedDirectory(workspace) || !RemovalInventory(removal.productCode,cancel,error))return false;
        current=workspace+L"\\SecureUxTheme-removal-recheck.msi";
        Package metadata;std::array<BYTE,32> before{},now{};
        const bool ok=CopyRemovalBaseline(removal.productCode,current,cancel,metadata,error) &&
            metadata.version==removal.version && GlassRegisteredMsi::Hash(removal.baseline,before,&cancel) &&
            GlassRegisteredMsi::Hash(current,now,&cancel) && before==removal.digest && now==removal.digest && !cancel.load();
        const bool cleaned=DeleteFileW(current.c_str())!=FALSE;
        if(!ok || !cleaned)return Fail(error,L"Vendor MSI baseline changed after preflight",ERROR_INVALID_DATA);
        error.clear();return true;
    }catch(...){if(!current.empty())DeleteFileW(current.c_str());return Fail(error,L"Rechecking vendor removal",ERROR_NOT_ENOUGH_MEMORY);}
}

}
