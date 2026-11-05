#pragma once
#include <windows.h>
#include <msi.h>
#include <string>
#include <vector>

namespace Shared::SecureUxThemeInventory {
// Read-only official MSI family inventory. ProductCode varies between releases;
// neither a retained DLL nor a packed GUID guessed from WiX is product presence.
inline UINT Query(std::vector<std::wstring>& products) noexcept {
    products.clear();
    try {
        for (const wchar_t* family:{L"{965B5B74-4040-4E89-947F-AF8D99A76706}",
            L"{3FF010A5-2E94-4621-A260-3A7ABCDDB37C}",L"{E0E6C0EA-103B-4E8A-B1A9-1703853A49F4}"}) {
            for (DWORD index=0;;++index) {
                wchar_t product[39]{};
                const UINT rc=MsiEnumRelatedProductsW(family,0,index,product);
                if (rc==ERROR_NO_MORE_ITEMS) break;
                if (rc!=ERROR_SUCCESS) { products.clear(); return rc; }
                products.emplace_back(product);
            }
        }
        return ERROR_SUCCESS;
    } catch (...) { products.clear(); return ERROR_NOT_ENOUGH_MEMORY; }
}
inline bool IsInstalled() noexcept {
    std::vector<std::wstring> products;
    return Query(products)==ERROR_SUCCESS && !products.empty();
}
}
