#pragma once
#include <windows.h>
#include <msi.h>
#include <string>
#include <vector>

namespace Shared::SecureUxThemeInventory {
// Read-only official MSI family inventory. ProductCode varies between releases;
// neither a retained DLL nor a packed GUID guessed from WiX is product presence.
inline UINT Query(std::vector<std::wstring>& products, bool installedOnly=false) noexcept {
    products.clear();
    try {
        for (const wchar_t* family:{L"{965B5B74-4040-4E89-947F-AF8D99A76706}",
            L"{3FF010A5-2E94-4621-A260-3A7ABCDDB37C}",L"{E0E6C0EA-103B-4E8A-B1A9-1703853A49F4}"}) {
            for (DWORD index=0;;++index) {
                wchar_t product[39]{};
                const UINT rc=MsiEnumRelatedProductsW(family,0,index,product);
                if (rc==ERROR_NO_MORE_ITEMS) break;
                if (rc!=ERROR_SUCCESS) { products.clear(); return rc; }
                // Related-product enumeration also returns advertisements and
                // stale family references. Neither is an installed payload.
                const INSTALLSTATE state=MsiQueryProductStateW(product);
                if (state==INSTALLSTATE_UNKNOWN || state==INSTALLSTATE_ADVERTISED) continue;
                // ABSENT can mean installed for another user: keep that ownership
                // guard rather than authorizing an install over a foreign product.
                if (state!=INSTALLSTATE_DEFAULT && state!=INSTALLSTATE_ABSENT) {
                    products.clear(); return ERROR_BAD_CONFIGURATION;
                }
                // Ownership candidates are not the same as installed products.
                // ABSENT must stay in the conservative inventory for context
                // checks, but must never put a card on the Installed shelf.
                if(installedOnly && state!=INSTALLSTATE_DEFAULT)continue;
                bool duplicate=false;
                for (const auto& existing:products)
                    if (_wcsicmp(existing.c_str(),product)==0) { duplicate=true; break; }
                if (!duplicate) products.emplace_back(product);
            }
        }
        return ERROR_SUCCESS;
    } catch (...) { products.clear(); return ERROR_NOT_ENOUGH_MEMORY; }
}
inline UINT QueryInstalled(std::vector<std::wstring>& products) noexcept {
    return Query(products,true);
}
inline bool IsInstalled() noexcept {
    std::vector<std::wstring> products;
    return QueryInstalled(products)==ERROR_SUCCESS && !products.empty();
}
}
