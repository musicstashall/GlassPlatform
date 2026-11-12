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
inline std::array<unsigned char,32> VerifyPublisher(const std::wstring& path,HANDLE handle) {
    WINTRUST_FILE_INFO file{};file.cbStruct=sizeof(file);file.pcwszFilePath=path.c_str();file.hFile=handle;
    WINTRUST_DATA trust{};trust.cbStruct=sizeof(trust);trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE;trust.dwUnionChoice=WTD_CHOICE_FILE;trust.pFile=&file;
    trust.dwStateAction=WTD_STATEACTION_VERIFY;trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto status=WinVerifyTrust(nullptr,&action,&trust);
    struct Close {WINTRUST_DATA& t;GUID& g;~Close(){t.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&g,&t);}} close{trust,action};
    Require(status==ERROR_SUCCESS,"OpenGlass Authenticode is not Valid");
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
