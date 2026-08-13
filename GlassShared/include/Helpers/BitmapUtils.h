// GlassShared/include/BitmapUtils.h
//
// FIX: SpriteEffect::Apply теперь принимает HBITMAP (работает на месте
// для DIBSECTION), а не Gdiplus::Bitmap*.  GDI+ зависимость убрана.

#pragma once
#include <windows.h>
#include <memory>
#include <vector>

namespace Shared::BitmapUtils
{
    void EarlyShutdown();
    bool IsOemBitmap(HBITMAP hbmp);

    // Конвертирует любой HBITMAP в 32bpp top-down DIBSECTION,
    // сохраняя альфа-канал для 32bpp источников.
    HBITMAP Promise32BPP(HBITMAP hbmp);

    // Проверяет, содержит ли 32bpp HBITMAP хотя бы один пиксель с ненулевым альфа.
    // Для битмапов с глубиной < 32bpp всегда возвращает false.
    [[nodiscard]] bool HasAlphaChannel(HBITMAP hbmp) noexcept;

    class SpriteEffect
    {
    public:
        // colorToReplace — RGB цвет фона для удаления
        // threshold      — допустимое отклонение по каждому каналу
        SpriteEffect(COLORREF colorToReplace, BYTE threshold = 10);

        // Работает на месте: обнуляет альфа для пикселей, совпадающих
        // по цвету с m_colorToReplace ± m_threshold.
        // hbmp ДОЛЖЕН быть DIBSECTION (создан через CreateDIBSection /
        // Promise32BPP), иначе вызов безвреден.
        void Apply(HBITMAP hbmp) const;

    private:
        COLORREF m_colorToReplace;
        BYTE m_threshold;
    };

    // Конвертирует hbmp в 32bpp и последовательно применяет effects.
    // Возвращает новый HBITMAP (вызывающий владеет и должен DeleteObject).
    HBITMAP ApplyEffects(HBITMAP hbmp, const std::vector<std::shared_ptr<SpriteEffect>>& effects);
}