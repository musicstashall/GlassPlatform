#include <Windows.h>
#include <wincrypt.h>

#include <DebugLog.h>

#include "ShellDevCertificate.h"

#include <wil/resource.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace
{
    constexpr wchar_t kDevCertSubject[] = L"CN=GlassProject Dev";
    constexpr wchar_t kDevCertFriendlyName[] = L"Glass Shell Dev";

    [[nodiscard]] wil::unique_hcertstore OpenUserStore(const wchar_t* storeName) noexcept
    {
        return wil::unique_hcertstore{ ::CertOpenSystemStoreW(0, storeName) };
    }

    [[nodiscard]] bool HasPrivateKey(PCCERT_CONTEXT ctx) noexcept
    {
        if (!ctx)
            return false;

        DWORD cb = 0;
        return ::CertGetCertificateContextProperty(ctx, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &cb) && cb > 0;
    }

    [[nodiscard]] bool SubjectMatchesDevCert(PCCERT_CONTEXT ctx) noexcept
    {
        if (!ctx)
            return false;

        wchar_t subject[128]{};
        if (::CertGetNameStringW(
                ctx,
                CERT_NAME_SIMPLE_DISPLAY_TYPE,
                0,
                nullptr,
                subject,
                static_cast<DWORD>(_countof(subject))) <= 1)
            return false;

        return _wcsicmp(subject, L"GlassProject Dev") == 0;
    }

    [[nodiscard]] PCCERT_CONTEXT FindDevCertInStore(HCERTSTORE store) noexcept
    {
        if (!store)
            return nullptr;

        PCCERT_CONTEXT best = nullptr;
        FILETIME bestNotBefore{};
        bool haveBest = false;

        for (PCCERT_CONTEXT ctx = ::CertEnumCertificatesInStore(store, nullptr); ctx;
             ctx = ::CertEnumCertificatesInStore(store, ctx))
        {
            if (!SubjectMatchesDevCert(ctx) || !HasPrivateKey(ctx))
                continue;

            if (!haveBest || ::CompareFileTime(&ctx->pCertInfo->NotBefore, &bestNotBefore) > 0)
            {
                if (best)
                    ::CertFreeCertificateContext(best);
                best = ::CertDuplicateCertificateContext(ctx);
                bestNotBefore = ctx->pCertInfo->NotBefore;
                haveBest = true;
            }
        }

        return best;
    }

    [[nodiscard]] bool CopyThumbprintHex(PCCERT_CONTEXT ctx, wchar_t* out, size_t cch) noexcept
    {
        if (!ctx || !out || cch == 0)
            return false;
        out[0] = L'\0';

        DWORD cb = 0;
        if (!::CertGetCertificateContextProperty(ctx, CERT_SHA1_HASH_PROP_ID, nullptr, &cb) || cb == 0)
            return false;

        std::vector<BYTE> hash(cb);
        if (!::CertGetCertificateContextProperty(ctx, CERT_SHA1_HASH_PROP_ID, hash.data(), &cb))
            return false;

        const size_t need = static_cast<size_t>(cb) * 2u + 1u;
        if (need > cch)
            return false;

        static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
        for (DWORD i = 0; i < cb; ++i)
        {
            out[i * 2] = kHex[(hash[i] >> 4) & 0x0F];
            out[i * 2 + 1] = kHex[hash[i] & 0x0F];
        }
        out[cb * 2] = L'\0';
        return true;
    }

    [[nodiscard]] bool IsCertInStore(HCERTSTORE store, PCCERT_CONTEXT ctx) noexcept
    {
        if (!store || !ctx)
            return false;

        return ::CertFindCertificateInStore(
                   store,
                   X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                   0,
                   CERT_FIND_EXISTING,
                   ctx,
                   nullptr) != nullptr;
    }

    [[nodiscard]] bool AddCertToStore(HCERTSTORE store, PCCERT_CONTEXT ctx) noexcept
    {
        if (!store || !ctx || IsCertInStore(store, ctx))
            return true;
        return ::CertAddCertificateContextToStore(store, ctx, CERT_STORE_ADD_REPLACE_EXISTING, nullptr) != FALSE;
    }

    [[nodiscard]] PCCERT_CONTEXT CreateDevCert() noexcept
    {
        CERT_NAME_BLOB subjectBlob{};
        DWORD encodedSize = 0;
        if (!::CertStrToNameW(X509_ASN_ENCODING, kDevCertSubject, CERT_X500_NAME_STR, nullptr, nullptr, &encodedSize, nullptr))
            return nullptr;

        std::vector<BYTE> encoded(encodedSize);
        if (!::CertStrToNameW(
                X509_ASN_ENCODING,
                kDevCertSubject,
                CERT_X500_NAME_STR,
                nullptr,
                encoded.data(),
                &encodedSize,
                nullptr))
            return nullptr;

        subjectBlob.cbData = encodedSize;
        subjectBlob.pbData = encoded.data();

        SYSTEMTIME start{};
        ::GetSystemTime(&start);
        SYSTEMTIME end = start;
        end.wYear = static_cast<WORD>(end.wYear + 5);

        CRYPT_KEY_PROV_INFO provInfo{};
        provInfo.pwszContainerName = const_cast<LPWSTR>(L"GlassShellDev");
        provInfo.pwszProvName = const_cast<LPWSTR>(MS_ENHANCED_PROV_W);
        provInfo.dwProvType = PROV_RSA_FULL;
        provInfo.dwFlags = CERT_SET_KEY_CONTEXT_PROP_ID;
        provInfo.dwKeySpec = AT_SIGNATURE;

        CRYPT_ALGORITHM_IDENTIFIER sigAlg{};
        sigAlg.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

        return ::CertCreateSelfSignCertificate(
            0,
            &subjectBlob,
            0,
            &provInfo,
            &sigAlg,
            &start,
            &end,
            nullptr);
    }

    [[nodiscard]] PCCERT_CONTEXT AcquireOrCreateDevCert() noexcept
    {
        auto myStore = OpenUserStore(L"My");
        if (!myStore)
            return nullptr;

        wil::unique_cert_context ctx{ FindDevCertInStore(myStore.get()) };
        if (ctx)
            return ctx.release();

        wil::unique_cert_context created{ CreateDevCert() };
        if (!created)
            return nullptr;

        if (!::CertAddCertificateContextToStore(myStore.get(), created.get(), CERT_STORE_ADD_REPLACE_EXISTING, nullptr))
            return nullptr;

        return created.release();
    }
}

namespace ShellDevCertificate
{
    bool EnsureGlassShellDevCertificateTrusted() noexcept
    {
        wil::unique_cert_context ctx{ AcquireOrCreateDevCert() };
        if (!ctx)
        {
            DebugLog(DBG_ERROR, L"[GlassSettings][ShellDevCert] dev certificate not found");
            return false;
        }

        CRYPT_DATA_BLOB friendly{};
        friendly.cbData = static_cast<DWORD>((wcslen(kDevCertFriendlyName) + 1u) * sizeof(wchar_t));
        friendly.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(kDevCertFriendlyName));
        (void)::CertSetCertificateContextProperty(
            ctx.get(),
            CERT_FRIENDLY_NAME_PROP_ID,
            0,
            &friendly);

        auto trustedPeople = OpenUserStore(L"TrustedPeople");
        auto trustedPublisher = OpenUserStore(L"TrustedPublisher");
        if (!trustedPeople || !trustedPublisher)
            return false;

        if (!AddCertToStore(trustedPeople.get(), ctx.get()))
            return false;
        if (!AddCertToStore(trustedPublisher.get(), ctx.get()))
            return false;

        return true;
    }

    bool GetGlassShellDevCertThumbprint(wchar_t* out, size_t cch) noexcept
    {
        if (!out || cch == 0)
            return false;

        wil::unique_cert_context ctx{ AcquireOrCreateDevCert() };
        if (!ctx)
            return false;

        return CopyThumbprintHex(ctx.get(), out, cch);
    }
}
