#pragma once
#include "ExplorerPch.h"

namespace Util
{
    struct Color
    {
        uint8_t R = 0;
        uint8_t G = 0;
        uint8_t B = 0;
        uint8_t A = 0;

        Color() = default;

        Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
            : R(r), G(g), B(b), A(a) {
        }

        // Преобразование в UINT32 ABGR (как в TranslucentTB)
        UINT32 ToABGR() const noexcept
        {
            return (UINT32(A) << 24) |
                (UINT32(B) << 16) |
                (UINT32(G) << 8) |
                (UINT32(R));
        }

        // Преобразование в UINT32 ARGB (если понадобится)
        UINT32 ToARGB() const noexcept
        {
            return (UINT32(A) << 24) |
                (UINT32(R) << 16) |
                (UINT32(G) << 8) |
                (UINT32(B));
        }

        bool operator==(const Color& other) const noexcept
        {
            return R == other.R &&
                G == other.G &&
                B == other.B &&
                A == other.A;
        }

        bool operator!=(const Color& other) const noexcept
        {
            return !(*this == other);
        }
    };
}
