#pragma once
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>

// Shared pinned-file and exact publisher policy for acquisition and uninstall.
namespace GlassOpenGlassTrust {
struct File {
    HANDLE h=INVALID_HANDLE_VALUE;
    ~File(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
    File()=default;File(const File&)=delete;File& operator=(const File&)=delete;
};
inline void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
inline void Pin(File& file,const std::wstring& path) {
    file.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    Require(file.h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(file.h,&info)&&info.nNumberOfLinks==1&&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"OpenGlass file pin/link check failed");
}
inline std::array<unsigned char,32> Hash(const unsigned char* data,std::size_t size) {
    Require(size<=(std::numeric_limits<ULONG>::max)(),"OpenGlass hash size limit");std::array<unsigned char,32> result{};
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    Require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"OpenGlass SHA256 provider");
    const auto status=BCryptHash(algorithm,nullptr,0,const_cast<PUCHAR>(data),static_cast<ULONG>(size),result.data(),static_cast<ULONG>(result.size()));
    BCryptCloseAlgorithmProvider(algorithm,0);Require(status>=0,"OpenGlass SHA256 failed");return result;
}
inline std::wstring Attribute(PCCERT_CONTEXT cert,const char* oid) {
    const auto n=CertGetNameStringW(cert,CERT_NAME_ATTR_TYPE,0,const_cast<char*>(oid),nullptr,0);
    if(n<2)return {};
    std::wstring result(n,L'\0');CertGetNameStringW(cert,CERT_NAME_ATTR_TYPE,0,const_cast<char*>(oid),result.data(),n);
    result.resize(n-1);return result;
}
// Generated Inno uninstallers need not inherit the signed setup's signature.
// An unsigned fallback must never accept a damaged embedded certificate table.
inline bool HasNoEmbeddedSignature(HANDLE file) noexcept {
    const auto read=[file](LONGLONG offset,void* out,DWORD bytes) {
        LARGE_INTEGER position{};position.QuadPart=offset;DWORD got=0;
        return SetFilePointerEx(file,position,nullptr,FILE_BEGIN)&&
            ReadFile(file,out,bytes,&got,nullptr)&&got==bytes;
    };
    IMAGE_DOS_HEADER dos{};
    if(!read(0,&dos,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||
       dos.e_lfanew<static_cast<LONG>(sizeof(dos))||dos.e_lfanew>1024*1024)return false;
    DWORD signature=0;IMAGE_FILE_HEADER header{};
    const LONGLONG nt=dos.e_lfanew;
    if(!read(nt,&signature,sizeof(signature))||signature!=IMAGE_NT_SIGNATURE||
       !read(nt+sizeof(signature),&header,sizeof(header)))return false;
    const LONGLONG optional=nt+sizeof(signature)+sizeof(header);
    WORD magic=0;if(!read(optional,&magic,sizeof(magic)))return false;
    IMAGE_DATA_DIRECTORY certificate{};
    if(magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 value{};
        if(header.SizeOfOptionalHeader<sizeof(value)||!read(optional,&value,sizeof(value))||
            value.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_SECURITY)return false;
        certificate=value.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    }else if(magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 value{};
        if(header.SizeOfOptionalHeader<sizeof(value)||!read(optional,&value,sizeof(value))||
            value.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_SECURITY)return false;
        certificate=value.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    }else return false;
    return certificate.VirtualAddress==0&&certificate.Size==0;
}

inline std::array<unsigned char,32> VerifyPublisher(const std::wstring& path,HANDLE handle,bool allowProtectedUnsignedUninstaller=false) {
    WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=path.c_str();file.hFile=handle;
    WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE;trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&file;
    trust.dwStateAction=WTD_STATEACTION_VERIFY;trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto status=WinVerifyTrust(nullptr,&action,&trust);
    struct Close {WINTRUST_DATA& t;GUID& g;~Close(){t.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&g,&t);}} close{trust,action};
    if(static_cast<unsigned long>(status)==0x800B0100UL&&allowProtectedUnsignedUninstaller&&HasNoEmbeddedSignature(handle))
        return {}; // No publisher digest; only the protected installed-uninstaller lane may opt in.
    if(static_cast<unsigned long>(status)==0x800B0100UL)throw std::runtime_error("OpenGlass executable has no verifiable Authenticode signature (TRUST_E_NOSIGNATURE, 0x800B0100).");
    if(status!=ERROR_SUCCESS)throw std::runtime_error("OpenGlass Authenticode is not Valid; WinVerifyTrust status="+
        std::to_string(static_cast<unsigned long>(status)));
    const auto provider=WTHelperProvDataFromStateData(trust.hWVTStateData);
    const auto signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):nullptr;
    const auto cert=signer&&signer->csCertChain&&signer->pasCertChain?signer->pasCertChain[0].pCert:nullptr;
    Require(cert&&Attribute(cert,szOID_COMMON_NAME)==L"Open Source Developer, Zhe Yang"&&
        Attribute(cert,szOID_ORGANIZATION_NAME)==L"Open Source Developer"&&Attribute(cert,szOID_LOCALITY_NAME)==L"Guangzhou"&&
        Attribute(cert,szOID_STATE_OR_PROVINCE_NAME)==L"Guangdong"&&Attribute(cert,szOID_COUNTRY_NAME)==L"CN",
        "OpenGlass publisher subject mismatch");
    return Hash(cert->pbCertEncoded,cert->cbCertEncoded);
}
}
