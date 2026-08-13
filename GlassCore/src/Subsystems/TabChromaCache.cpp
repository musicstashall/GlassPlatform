#include "CorePch.h"
#include <Helpers/TabChroma.h>
#include <Helpers/ThemeHelper.h>

namespace GlassCore::TabChroma
{
    namespace
    {
        constexpr int kTisBySlot[3] = { 1, 4, 5 };

        SRWLOCK  s_rw     = SRWLOCK_INIT;
        UINT32   s_serial = 0;
        COLORREF s_color[3]{};
        bool     s_valid[3]{};
    } // namespace

    bool TryReadChromaForTisState(int tis1Based, COLORREF& outRgb, UINT32* optSerial) noexcept
    {
        outRgb = 0;
        if (optSerial)
            *optSerial = 0;
        int slot = -1;
        for (int i = 0; i < 3; ++i)
        {
            if (kTisBySlot[i] == tis1Based)
            {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return false;
        AcquireSRWLockShared(&s_rw);
        const UINT32  ser  = s_serial;
        const bool    ok   = s_valid[static_cast<size_t>(slot)] && ser != 0u;
        const COLORREF c   = s_color[static_cast<size_t>(slot)];
        ReleaseSRWLockShared(&s_rw);
        if (!ok)
            return false;
        outRgb = c & 0xFFFFFFu;
        if (optSerial)
            *optSerial = ser;
        return true;
    }

    bool RefreshFromTheme(HWND themeAnchorOrNull) noexcept
    {
        COLORREF tmp[3]{};
        bool     got[3]{};
        for (int i = 0; i < 3; ++i)
        {
            COLORREF rgb{};
            if (Shared::ThemeHelper::TryGetTabChromaKeyFromThemeForState(themeAnchorOrNull, kTisBySlot[i], rgb))
            {
                got[i]                      = true;
                tmp[static_cast<size_t>(i)] = rgb & 0xFFFFFFu;
            }
        }
        if (!got[0] && !got[1] && !got[2])
            return false;

        AcquireSRWLockExclusive(&s_rw);
        bool   changed = false;
        for (int i = 0; i < 3; ++i)
        {
            if (!got[i])
                continue;
            const COLORREF nv = tmp[static_cast<size_t>(i)];
            if (!s_valid[static_cast<size_t>(i)] || s_color[static_cast<size_t>(i)] != nv)
            {
                s_color[static_cast<size_t>(i)] = nv;
                s_valid[static_cast<size_t>(i)] = true;
                changed                        = true;
            }
        }
        if (changed)
        {
            s_serial = s_serial + 1u;
            if (s_serial == 0u)
                s_serial = 1u;
        }
        const UINT32 outSerial = s_serial;
        int          logIdx  = 0;
        for (; logIdx < 3; ++logIdx)
        {
            if (got[logIdx])
                break;
        }
        const unsigned R = logIdx < 3
            ? (unsigned)GetRValue(tmp[static_cast<size_t>(logIdx)] & 0xFFFFFFu)
            : 0u;
        const unsigned G = logIdx < 3
            ? (unsigned)GetGValue(tmp[static_cast<size_t>(logIdx)] & 0xFFFFFFu)
            : 0u;
        const unsigned B = logIdx < 3
            ? (unsigned)GetBValue(tmp[static_cast<size_t>(logIdx)] & 0xFFFFFFu)
            : 0u;
        const unsigned Cref
            = logIdx < 3 ? (unsigned)(tmp[static_cast<size_t>(logIdx)] & 0xFFFFFFu) : 0u;
        ReleaseSRWLockExclusive(&s_rw);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][TabChroma] кэш: serial=%u n=%d d=%d f=%d (valid) COLORREF[перв.слот]=0x%06X R=%u G=%u B=%u",
            outSerial,
            (int)got[0],
            (int)got[1],
            (int)got[2],
            Cref,
            R,
            G,
            B);
        return true;
    }
} // namespace GlassCore::TabChroma
