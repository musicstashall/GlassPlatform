#include "GlassSecureUxPackage.h"
#include "GlassSecureUxRelease.h"
#include <SecureUxThemeInventory.h>
#include "GlassInstallMsi.h"
#include "../InstallLog.h"
#include <winhttp.h>
#include <wincrypt.h>
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
struct Internet {
    HINTERNET h=nullptr;
    explicit Internet(HINTERNET value):h(value){}
    ~Internet(){if(h)WinHttpCloseHandle(h);}
    Internet(const Internet&)=delete; Internet& operator=(const Internet&)=delete;
};
struct MsiHandle { MSIHANDLE h=0; ~MsiHandle(){if(h)MsiCloseHandle(h);} };
std::wstring Wide(const std::string& text){
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!n)return L"Invalid UTF-8 response";
    std::wstring result(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),&result[0],n);return result;
}
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
bool AllowedHost(const std::wstring& host){
    return _wcsicmp(host.c_str(),L"api.github.com")==0||_wcsicmp(host.c_str(),L"github.com")==0||
        _wcsicmp(host.c_str(),L"release-assets.githubusercontent.com")==0||
        _wcsicmp(host.c_str(),L"objects.githubusercontent.com")==0;
}
// Redirects are followed manually so neither TLS nor GitHub host restrictions
// can be bypassed. No auth tokens, shell, PowerShell or external downloader.
bool Get(const std::wstring& initial,HANDLE output,std::string* text,std::uint64_t limit,
    const std::atomic<bool>& cancel,std::wstring& error){
    Internet session(WinHttpOpen(L"GlassSetup/SecureUxTheme",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session.h)return Fail(error,L"WinHttpOpen");
    if(!WinHttpSetTimeouts(session.h,10000,15000,15000,15000))return Fail(error,L"WinHttpSetTimeouts");
    std::wstring url=initial;
    for(int redirect=0;redirect<6;++redirect){
        if(Cancelled(cancel,error))return false;
        URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);parts.dwHostNameLength=static_cast<DWORD>(-1);
        parts.dwUrlPathLength=static_cast<DWORD>(-1);parts.dwExtraInfoLength=static_cast<DWORD>(-1);
        parts.dwUserNameLength=static_cast<DWORD>(-1);parts.dwPasswordLength=static_cast<DWORD>(-1);
        if(!WinHttpCrackUrl(url.c_str(),0,0,&parts))return Fail(error,L"Invalid GitHub URL");
        const std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
        if(parts.nScheme!=INTERNET_SCHEME_HTTPS||parts.nPort!=INTERNET_DEFAULT_HTTPS_PORT||
            parts.dwUserNameLength||parts.dwPasswordLength||!AllowedHost(host))return Fail(error,L"Unsafe GitHub redirect",ERROR_ACCESS_DENIED);
        std::wstring path(parts.lpszUrlPath,parts.dwUrlPathLength);
        if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
        Internet connection(WinHttpConnect(session.h,host.c_str(),parts.nPort,0));
        if(!connection.h)return Fail(error,L"WinHttpConnect");
        Internet request(WinHttpOpenRequest(connection.h,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
        if(!request.h)return Fail(error,L"WinHttpOpenRequest");
        DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if(!WinHttpSetOption(request.h,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy)))return Fail(error,L"WinHttpSetOption");
        const wchar_t* headers=text?L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n":
                                    L"Accept: application/octet-stream\r\n";
        if(!WinHttpSendRequest(request.h,headers,static_cast<DWORD>(-1),WINHTTP_NO_REQUEST_DATA,0,0,0)||
            !WinHttpReceiveResponse(request.h,nullptr))return Fail(error,L"GitHub request failed");
        DWORD status=0,bytes=sizeof(status);
        if(!WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,&status,&bytes,WINHTTP_NO_HEADER_INDEX))return Fail(error,L"GitHub HTTP status");
        if(status==301||status==302||status==303||status==307||status==308){
            bytes=0;WinHttpQueryHeaders(request.h,WINHTTP_QUERY_LOCATION,WINHTTP_HEADER_NAME_BY_INDEX,nullptr,&bytes,WINHTTP_NO_HEADER_INDEX);
            if(!bytes||bytes>65536)return Fail(error,L"Missing GitHub redirect",ERROR_INVALID_DATA);
            std::vector<wchar_t> location(bytes/sizeof(wchar_t)+1,0);
            if(!WinHttpQueryHeaders(request.h,WINHTTP_QUERY_LOCATION,WINHTTP_HEADER_NAME_BY_INDEX,
                location.data(),&bytes,WINHTTP_NO_HEADER_INDEX))return Fail(error,L"GitHub redirect");
            url=location.data();continue;
        }
        if(status!=200)return Fail(error,L"GitHub HTTP error",status);
        std::array<char,32768> buffer{};std::uint64_t total=0;
        for(;;){
            if(Cancelled(cancel,error))return false;
            DWORD read=0;if(!WinHttpReadData(request.h,buffer.data(),static_cast<DWORD>(buffer.size()),&read))return Fail(error,L"GitHub download");
            if(!read)return true;
            total+=read;if(total>limit)return Fail(error,L"GitHub response too large",ERROR_FILE_TOO_LARGE);
            if(text)text->append(buffer.data(),read);
            else {DWORD written=0;if(!WriteFile(output,buffer.data(),read,&written,nullptr)||written!=read)return Fail(error,L"Writing adjacent MSI");}
        }
    }
    return Fail(error,L"Too many GitHub redirects",ERROR_WINHTTP_REDIRECT_FAILED);
}
bool Hash(File& file,const Asset& asset,const std::atomic<bool>& cancel,std::wstring& error){
    LARGE_INTEGER size{};BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file.h,&info)||!GetFileSizeEx(file.h,&size))return Fail(error,L"Reading adjacent MSI");
    if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||info.nNumberOfLinks!=1||
        static_cast<std::uint64_t>(size.QuadPart)!=asset.size)return Fail(error,L"MSI file size/type does not match official release",ERROR_INVALID_DATA);
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))return Fail(error,L"SHA-256 provider");
    bool ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)!=FALSE;
    LARGE_INTEGER zero{};ok=ok&&SetFilePointerEx(file.h,zero,nullptr,FILE_BEGIN)!=FALSE;
    std::array<BYTE,32768> buffer{};
    while(ok){
        if(Cancelled(cancel,error)){ok=false;break;}
        DWORD read=0;ok=ReadFile(file.h,buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr)!=FALSE;
        if(!ok||!read)break;
        ok=CryptHashData(hash,buffer.data(),read,0)!=FALSE;
    }
    BYTE digest[32]{};DWORD count=sizeof(digest);
    if(ok)ok=CryptGetHashParam(hash,HP_HASHVAL,digest,&count,0)!=FALSE&&count==32;
    if(hash)CryptDestroyHash(hash);CryptReleaseContext(provider,0);
    if(!ok)return cancel.load()?false:Fail(error,L"Computing MSI SHA-256");
    std::string hex;const char digits[]="0123456789abcdef";
    for(BYTE b:digest){hex+=digits[b>>4];hex+=digits[b&15];}
    if(hex!=asset.sha256)return Fail(error,L"MSI SHA-256 differs from official GitHub release; existing file was not overwritten",ERROR_CRC);
    return SetFilePointerEx(file.h,zero,nullptr,FILE_BEGIN)?true:Fail(error,L"Rewinding MSI");
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
    File dest;dest.h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
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
        const std::wstring adjacent=module.substr(0,slash+1)+Wide(asset.name);
        File local;
        // Deny writers/deletion while hashing and copying to the secured workspace.
        local.h=CreateFileW(adjacent.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(local.h==INVALID_HANDLE_VALUE){
            const DWORD openError=GetLastError();
            if(openError!=ERROR_FILE_NOT_FOUND)return Fail(error,L"Cannot read adjacent SecureUxTheme MSI",openError);
        }
        // Known manually supplied v4 needs no network. Unknown/newer packages
        // must match the current stable API digest. Never overwrite a local MSI.
        bool verified=local.h!=INVALID_HANDLE_VALUE&&Hash(local,asset,cancel,error);
        if(!verified){
            if(Cancelled(cancel,error))return false;
            error.clear();
            std::string json,parseError;
            if(!Get(L"https://api.github.com/repos/namazso/SecureUxTheme/releases/latest",INVALID_HANDLE_VALUE,&json,4*1024*1024,cancel,error))return false;
            if(!Select(json,arch,asset,parseError)){error=L"SecureUxTheme release discovery: "+Wide(parseError);return false;}
            InstallLog::LineFormat(L"SecureUxTheme release resolved: %s | %s | SHA256 %s",Wide(asset.tag).c_str(),Wide(asset.url).c_str(),Wide(asset.sha256).c_str());
            if(local.h!=INVALID_HANDLE_VALUE){if(!Hash(local,asset,cancel,error))return false;}
            else {
                GUID guid{};wchar_t guidText[40]{};
                if(FAILED(CoCreateGuid(&guid))||!StringFromGUID2(guid,guidText,40))return Fail(error,L"Creating MSI download name",ERROR_GEN_FAILURE);
                partial=adjacent+L"."+guidText+L".part";
                {
                    File download;download.h=CreateFileW(partial.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                    if(download.h==INVALID_HANDLE_VALUE){error=L"Cannot write next to GlassSetup.exe. Place "+Wide(asset.name)+L" there manually or move the installer to a writable directory.";return false;}
                    const bool received=Get(Wide(asset.url),download.h,nullptr,asset.size,cancel,error)&&FlushFileBuffers(download.h);
                    if(!received){if(error.empty())Fail(error,L"Flushing downloaded MSI");CloseHandle(download.h);download.h=INVALID_HANDLE_VALUE;DeleteFileW(partial.c_str());return false;}
                }
                {
                    File downloaded;downloaded.h=CreateFileW(partial.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                    if(downloaded.h==INVALID_HANDLE_VALUE||!Hash(downloaded,asset,cancel,error)){
                        if(downloaded.h!=INVALID_HANDLE_VALUE){CloseHandle(downloaded.h);downloaded.h=INVALID_HANDLE_VALUE;}
                        DeleteFileW(partial.c_str());return false;
                    }
                }
                // No REPLACE_EXISTING: a file another process placed here wins.
                if(!MoveFileExW(partial.c_str(),adjacent.c_str(),MOVEFILE_WRITE_THROUGH)){
                    const DWORD rc=GetLastError();DeleteFileW(partial.c_str());return Fail(error,L"Publishing adjacent MSI (existing file was not replaced)",rc);
                }
                partial.clear();local.h=CreateFileW(adjacent.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                if(local.h==INVALID_HANDLE_VALUE||!Hash(local,asset,cancel,error))return false;
            }
        }
        staged=privateDirectory+L"\\"+Wide(asset.name);
        if(!CopySnapshot(local,staged,cancel,error)||!Metadata(staged,arch,package,error)){DeleteFileW(staged.c_str());package={};return false;}
        package.path=staged;error.clear();
        InstallLog::LineFormat(L"SecureUxTheme original MSI: %s; release %s; ProductCode %s; version %s",adjacent.c_str(),Wide(asset.tag).c_str(),package.productCode.c_str(),package.version.c_str());
        return true;
    }catch(const std::exception& e){error=L"SecureUxTheme: "+Wide(e.what());}
    catch(...){Fail(error,L"Preparing official SecureUxTheme",ERROR_NOT_ENOUGH_MEMORY);}
    if(!partial.empty())DeleteFileW(partial.c_str());if(!staged.empty())DeleteFileW(staged.c_str());package={};return false;
}
}
