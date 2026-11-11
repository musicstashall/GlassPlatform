#include "GlassReleaseClient.h"
#include <winhttp.h>
#include <wincrypt.h>
#include <sddl.h>
#include <vector>
#include <array>

namespace GlassReleaseClient {
HANDLE CreatePrivateFile(const std::wstring& path) {
    PSECURITY_DESCRIPTOR sd=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)",SDDL_REVISION_1,&sd,nullptr))return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),sd,FALSE};
    const HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,&attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    const DWORD error=file==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;
    LocalFree(sd);SetLastError(error);return file;
}
struct Internet {
    HINTERNET h=nullptr;
    explicit Internet(HINTERNET value):h(value){}
    ~Internet(){if(h)WinHttpCloseHandle(h);}
    Internet(const Internet&)=delete; Internet& operator=(const Internet&)=delete;
};
std::wstring Wide(const std::string& text){
    // Optional catalogue metadata may be absent. Windows returns zero for an
    // empty input too; that is not a UTF-8 decoding error.
    if(text.empty())return {};
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!n)return L"Invalid UTF-8 response";
    std::wstring result(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),&result[0],n);return result;
}
bool Fail(std::wstring& error,const wchar_t* operation,DWORD code=GetLastError()){
    error=std::wstring(operation)+L" ("+std::to_wstring(code)+L")";return false;
}
bool Cancelled(const std::atomic<bool>& cancel,std::wstring& error){
    if(!cancel.load(std::memory_order_acquire))return false;
    Fail(error,L"Download cancelled",ERROR_INSTALL_USEREXIT);return true;
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
    Internet session(WinHttpOpen(L"GlassSetup/ReleaseClient",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    if(!session.h)return Fail(error,L"WinHttpOpen");
    if(!WinHttpSetTimeouts(session.h,4000,4000,4000,8000))return Fail(error,L"WinHttpSetTimeouts");
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
        if(!text&&packageProgress)packageProgress(0,limit);
        std::array<char,32768> buffer{};std::uint64_t total=0;
        for(;;){
            if(Cancelled(cancel,error))return false;
            DWORD read=0;if(!WinHttpReadData(request.h,buffer.data(),static_cast<DWORD>(buffer.size()),&read))return Fail(error,L"GitHub download");
            if(!read)return true;
            total+=read;if(total>limit)return Fail(error,L"GitHub response too large",ERROR_FILE_TOO_LARGE);
            if(text)text->append(buffer.data(),read);
            else {DWORD written=0;if(!WriteFile(output,buffer.data(),read,&written,nullptr)||written!=read)return Fail(error,L"Writing package download");
                if(packageProgress)packageProgress(total,limit);}
        }
    }
    return Fail(error,L"Too many GitHub redirects",ERROR_WINHTTP_REDIRECT_FAILED);
}
bool VerifyHash(HANDLE file,std::uint64_t expectedSize,const std::string& expectedHash,const std::atomic<bool>& cancel,std::wstring& error){
    LARGE_INTEGER size{};BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info)||!GetFileSizeEx(file,&size))return Fail(error,L"Reading package");
    if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||info.nNumberOfLinks!=1||
        static_cast<std::uint64_t>(size.QuadPart)!=expectedSize)return Fail(error,L"MSI file size/type does not match official release",ERROR_INVALID_DATA);
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))return Fail(error,L"SHA-256 provider");
    bool ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)!=FALSE;
    LARGE_INTEGER zero{};ok=ok&&SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)!=FALSE;
    std::array<BYTE,32768> buffer{};
    while(ok){
        if(Cancelled(cancel,error)){ok=false;break;}
        DWORD read=0;ok=ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&read,nullptr)!=FALSE;
        if(!ok||!read)break;
        ok=CryptHashData(hash,buffer.data(),read,0)!=FALSE;
    }
    BYTE digest[32]{};DWORD count=sizeof(digest);
    if(ok)ok=CryptGetHashParam(hash,HP_HASHVAL,digest,&count,0)!=FALSE&&count==32;
    const DWORD failure=ok?ERROR_SUCCESS:GetLastError();
    if(hash)CryptDestroyHash(hash);CryptReleaseContext(provider,0);
    if(!ok)return cancel.load()?false:Fail(error,L"Computing MSI SHA-256",failure);
    std::string hex;const char digits[]="0123456789abcdef";
    for(BYTE b:digest){hex+=digits[b>>4];hex+=digits[b&15];}
    if(hex!=expectedHash)return Fail(error,L"MSI SHA-256 differs from release metadata",ERROR_CRC);
    return SetFilePointerEx(file,zero,nullptr,FILE_BEGIN)?true:Fail(error,L"Rewinding MSI");
}
}
