#pragma once
#include <windows.h>
#include <wil/resource.h>

namespace GlassCore::GdiUtils
{
    // Только для native MaskBlt/StretchBlt изображения: RGB сохраняется,
    // A=255 во временной 32bpp копии. Это НЕ color-key и НЕ AlphaBlend.
    // Не передавать сюда финальный клиентский буфер окна. Вызывающий обязан
    // проверить класс/сессию/DC и сохранить исходные mask/ROP/координаты.
    class OpaqueBitmapSource final
    {
    public:
        explicit OpaqueBitmapSource(HDC source) noexcept;
        ~OpaqueBitmapSource() noexcept;
        OpaqueBitmapSource(const OpaqueBitmapSource&) = delete;
        OpaqueBitmapSource& operator=(const OpaqueBitmapSource&) = delete;
        OpaqueBitmapSource(OpaqueBitmapSource&&) = delete;
        OpaqueBitmapSource& operator=(OpaqueBitmapSource&&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept { return m_dc != nullptr; }

    private:
        HDC m_dc{};
        HGDIOBJ m_original{};
        wil::unique_hbitmap m_bitmap;
    };
}
