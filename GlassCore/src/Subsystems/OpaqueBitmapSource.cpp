#include "CorePch.h"
#include <Subsystems/OpaqueBitmapSource.h>
#include <BitmapUtils.h>
#include <wil.h>
#include <limits>
#include <utility>

namespace GlassCore::GdiUtils
{
    OpaqueBitmapSource::OpaqueBitmapSource(HDC source) noexcept
    {
        if (!source || GetObjectType(source) != OBJ_MEMDC) return;
        const auto bitmap = static_cast<HBITMAP>(GetCurrentObject(source, OBJ_BITMAP));
        BITMAP info{};
        if (!bitmap || Shared::BitmapUtils::IsOemBitmap(bitmap)
            || GetObjectW(bitmap, sizeof(info), &info) != sizeof(info)
            || info.bmPlanes != 1 || info.bmBitsPixel < 8 || info.bmBitsPixel > 32
            || info.bmWidth <= 0 || info.bmHeight <= 0
            || info.bmWidth > (std::numeric_limits<LONG>::max)() / 4
            || static_cast<size_t>(info.bmHeight) > (std::numeric_limits<size_t>::max)()
                / (static_cast<size_t>(info.bmWidth) * 4)
            || !GdiFlush()) return;

        // GetDIBits требует, чтобы исходный bitmap не был выбран в DC.
        // Монохромная заглушка принадлежит нам; остальные свойства DC не трогаем.
        wil::unique_hbitmap placeholder{CreateBitmap(1, 1, 1, 1, nullptr)};
        if (!placeholder) return;
        const HGDIOBJ original = SelectObject(source, placeholder.get());
        if (!original || original == HGDI_ERROR) return;
        bool committed = false;
        auto restoreOnFailure = wil::scope_exit([&]() noexcept {
            if (!committed) SelectObject(source, original);
        });
        BITMAPINFO dib{};
        dib.bmiHeader.biSize = sizeof(dib.bmiHeader);
        dib.bmiHeader.biWidth = info.bmWidth;
        dib.bmiHeader.biHeight = -info.bmHeight;
        dib.bmiHeader.biPlanes = 1;
        dib.bmiHeader.biBitCount = 32;
        dib.bmiHeader.biCompression = BI_RGB;
        void* data = nullptr;
        wil::unique_hbitmap copy{CreateDIBSection(source, &dib, DIB_RGB_COLORS, &data, nullptr, 0)};
        // В отличие от preserve-alpha Promise32BPP здесь нужен полный read,
        // включая проверку числа строк; DC сохраняет палитру источника.
        if (!copy || !data || GetDIBits(source, bitmap, 0, static_cast<UINT>(info.bmHeight),
            data, &dib, DIB_RGB_COLORS) != info.bmHeight) return;
        auto* pixels = static_cast<DWORD*>(data);
        const size_t count = static_cast<size_t>(info.bmWidth) * info.bmHeight;
        for (size_t i = 0; i < count; ++i) pixels[i] |= 0xFF000000u;
        const HGDIOBJ previous = SelectObject(source, copy.get());
        if (!previous || previous == HGDI_ERROR) return;
        m_bitmap = std::move(copy);
        m_original = original;
        m_dc = source;
        committed = true;
    }

    OpaqueBitmapSource::~OpaqueBitmapSource() noexcept
    {
        // Восстановить до удаления копии членом unique_hbitmap.
        if (m_dc) SelectObject(m_dc, m_original);
    }
}
