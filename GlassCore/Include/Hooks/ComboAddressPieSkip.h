#pragma once

#include <Helpers/AddressBarPieUtils.h>

namespace GlassCore::ComboAddressPieSkip
{
    // См. Shared::AddressBarPie::IsInProgressPieComboSubtree (полный подстрок от ComboLBox / ComboBox / ComboBoxEx32).
    [[nodiscard]] inline bool IsAddressPieComboBoxEx32(HWND hwnd) noexcept
    {
        return Shared::AddressBarPie::IsInProgressPieComboSubtree(hwnd);
    }
} // namespace GlassCore::ComboAddressPieSkip
