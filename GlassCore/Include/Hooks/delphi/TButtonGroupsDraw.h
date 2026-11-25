#pragma once
#include <windows.h>

namespace GlassCore::DelphiButtonGroups
{
    // Root owns the EXE IAT slots and balances paint/GetDC sessions. Pass the
    // original from that installed slot, never infer its module by return address.
    int DrawCaptionText(decltype(&::DrawTextExW) original, HDC dc, LPWSTR text, int count,
        LPRECT rect, UINT flags, LPDRAWTEXTPARAMS params);
    int Fill(decltype(&::FillRect) original, HDC dc, const RECT* rect, HBRUSH brush);
    BOOL Gradient(decltype(&::GdiGradientFill) original, HDC dc, PTRIVERTEX vertices,
        ULONG vertexCount, PVOID mesh, ULONG meshCount, ULONG mode, LPCWSTR api);
}
