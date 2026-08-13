#include "SharedPch.h"
#include <ExplorerCompatHostPolicy.h>
#include <array>

namespace Shared::ExplorerCompatHostPolicy
{
    namespace
    {
        [[nodiscard]] bool EqualsClass(const std::wstring& cls, const wchar_t* lit) noexcept
        {
            return _wcsicmp(cls.c_str(), lit) == 0;
        }

        static constexpr wchar_t kBridgeClass[] = L"Microsoft.UI.Content.DesktopChildSiteBridge";
        static const std::array<const wchar_t*, 4> kHostClasses{
            L"InputNonClientPointerSource",
            L"ReunionWindowingCaptionControls",
            kBridgeClass,
            L"InputSiteWindowClass",
        };
    }

    bool IsExplorerProcess() noexcept
    {
        return GetModuleHandleW(L"explorer.exe") != nullptr;
    }

    bool IsHostWindowClass(const std::wstring& normalizedCls) noexcept
    {
        if (normalizedCls.empty())
            return false;

        for (const wchar_t* entry : kHostClasses)
        {
            if (EqualsClass(normalizedCls, entry))
                return true;
        }
        return false;
    }

    bool IsWndCompatBridgeClass(const std::wstring& normalizedCls) noexcept
    {
        return EqualsClass(normalizedCls, kBridgeClass);
    }

    bool ShouldWndCompatHandleHostWindow(const std::wstring& normalizedCls) noexcept
    {
        if (!IsExplorerProcess())
            return false;
        return IsWndCompatBridgeClass(normalizedCls);
    }
}
