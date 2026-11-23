// Environment doubles for the complete production TButtonGroupsDraw.cpp and
// root gate/IAT wrappers. No Windows rasterization or real IAT is emulated.
#pragma once
#include "coolbar_test_shim.h"
#include <Hooks/RootGdiHooks.h>
using BYTE = std::uint8_t;
using ULONG = std::uint32_t;
using ULONG_PTR = std::uintptr_t;
struct TRIVERTEX
{
    LONG x{}, y{};
    std::uint16_t Red{}, Green{}, Blue{}, Alpha{};
};
using PTRIVERTEX = TRIVERTEX*;
struct GRADIENT_RECT { ULONG UpperLeft{}, LowerRight{}; };
inline constexpr ULONG GRADIENT_FILL_RECT_H = 0, GRADIENT_FILL_RECT_V = 1, GRADIENT_FILL_TRIANGLE = 2;
inline constexpr int MM_TEXT = 1, COLOR_MENUBAR = 30;
struct RGBQUAD { BYTE rgbBlue{}, rgbGreen{}, rgbRed{}, rgbReserved{}; };
struct GroupBuffer { std::array<RGBQUAD, 3> samples{}; bool painted = false; };
using HPAINTBUFFER = GroupBuffer*;
#define THROW_HR_IF(hr, condition) do { if (condition) throw HRESULT(hr); } while (false)
#define THROW_IF_FAILED(expr) do { const HRESULT h = (expr); if (!SUCCEEDED(h)) throw h; } while (false)
namespace GroupTest
{
    inline std::unordered_map<HWND, RECT> clients;
    inline std::unordered_map<HDC, int> mapModes;
    inline std::unordered_map<HDC, GroupBuffer*> buffers;
    inline COLORREF windowColor = 0xffffff, faceColor = 0xf0f0f0;
    inline bool failBuffer = false, failAlpha = false, failDraw = false, failFlush = false, failBlack = false;
    inline int bufferCalls = 0, commits = 0, discards = 0, alphaCalls = 0, flushes = 0;
    inline int nativeGradients = 0, nativeFills = 0, nativeTexts = 0;
    inline int chainGradient = 0, chainGdiGradient = 0, chainFill = 0, chainText = 0;
    inline HDC lastNativeDc{};
    inline PTRIVERTEX lastVertices{};
    inline PVOID lastMesh{};
    inline ULONG lastVertexCount{}, lastMeshCount{}, lastMode{};
    inline RECT bufferRect{};
    inline BYTE constantAlpha{};
    inline bool bufferErase = true;
    inline std::array<RGBQUAD, 3> committed{};
    inline HBRUSH lastBrush{};
    inline LPCWSTR alphaString{};
    inline UINT alphaFlags{};
    inline int alphaCount{};
    inline RECT alphaRect{};
    inline HMODULE originalModule{};
    inline std::function<void()> duringBuffer;
}
inline BOOL GroupGetClientRect(HWND hwnd, LPRECT r)
{
    if (const auto i = GroupTest::clients.find(hwnd); i != GroupTest::clients.end())
    { *r = i->second; return TRUE; }
    return FALSE;
}
inline COLORREF GroupGetSysColor(int i)
{ return i == COLOR_WINDOW ? GroupTest::windowColor : GroupTest::faceColor; }
inline int GetMapMode(HDC dc)
{
    const auto i = GroupTest::mapModes.find(dc);
    return i == GroupTest::mapModes.end() ? MM_TEXT : i->second;
}
inline BOOL GdiFlush() { ++GroupTest::flushes; return !GroupTest::failFlush; }
inline BOOL GroupPatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    if (rop == BLACKNESS && GroupTest::failBlack) return FALSE;
    return ::PatBlt(dc, x, y, w, h, rop);
}
inline HRESULT BufferedPaintSetAlpha(HPAINTBUFFER buffer, const RECT* rect, BYTE alpha)
{
    assert(buffer && !rect && alpha == 255 && buffer->painted);
    ++GroupTest::alphaCalls;
    if (GroupTest::failAlpha) return E_FAIL;
    for (auto& p : buffer->samples) p.rgbReserved = alpha;
    return S_OK;
}
inline BOOL GdiGradientFill(HDC dc, PTRIVERTEX v, ULONG vc, PVOID mesh, ULONG mc, ULONG mode)
{
    ++GroupTest::nativeGradients; GroupTest::lastNativeDc = dc;
    GroupTest::lastVertices = v; GroupTest::lastMesh = mesh;
    GroupTest::lastVertexCount = vc; GroupTest::lastMeshCount = mc; GroupTest::lastMode = mode;
    if (GroupTest::failDraw) return FALSE;
    if (const auto i = GroupTest::buffers.find(dc); i != GroupTest::buffers.end())
    {
        assert(v && mesh && vc == 2 && mc == 1 && mode <= 1);
        const auto pair = *static_cast<GRADIENT_RECT*>(mesh);
        const auto& a = v[pair.UpperLeft]; const auto& b = v[pair.LowerRight];
        auto& out = i->second->samples;
        out[0] = RGBQUAD{BYTE(a.Blue >> 8), BYTE(a.Green >> 8), BYTE(a.Red >> 8), 0};
        out[2] = RGBQUAD{BYTE(b.Blue >> 8), BYTE(b.Green >> 8), BYTE(b.Red >> 8), 0};
        out[1] = RGBQUAD{BYTE((unsigned(out[0].rgbBlue) + out[2].rgbBlue) / 2),
            BYTE((unsigned(out[0].rgbGreen) + out[2].rgbGreen) / 2),
            BYTE((unsigned(out[0].rgbRed) + out[2].rgbRed) / 2), 0};
        i->second->painted = true;
    }
    return TRUE;
}
inline int GroupNativeFill(HDC dc, const RECT*, HBRUSH brush)
{
    ++GroupTest::nativeFills; GroupTest::lastNativeDc = dc; GroupTest::lastBrush = brush;
    if (GroupTest::failDraw) return 0;
    if (const auto i = GroupTest::buffers.find(dc); i != GroupTest::buffers.end())
    {
        for (auto& p : i->second->samples)
            p = RGBQUAD{BYTE(brush->lbColor >> 16), BYTE(brush->lbColor >> 8), BYTE(brush->lbColor), 0};
        i->second->painted = true;
    }
    return 37;
}
inline int GroupNativeText(HDC dc, LPWSTR, int, LPRECT rect, UINT flags, LPDRAWTEXTPARAMS)
{
    ++GroupTest::nativeTexts; GroupTest::lastNativeDc = dc;
    if (rect && (flags & DT_CALCRECT)) rect->right += 7;
    return 43;
}
namespace GlassCore::Text
{
    inline HRESULT GroupDrawTextWithAlpha(HDC dc, LPCWSTR text, int count,
        LPRECT rect, UINT flags, int& result, bool transparent)
    {
        GroupTest::alphaString = text; GroupTest::alphaCount = count;
        GroupTest::alphaFlags = flags; GroupTest::alphaRect = *rect;
        return DrawTextWithAlpha(dc, text, count, rect, flags, result, transparent);
    }
    inline HRESULT DrawThemeContent(HDC dc, const RECT& rect, const RECT*, const RECT*, DWORD,
        const std::function<void(HDC, HPAINTBUFFER, RGBQUAD*, int)>& draw,
        BYTE alpha, bool blt, bool update, bool erase)
    {
        assert(!blt && update && !erase);
        ++GroupTest::bufferCalls; GroupTest::bufferRect = rect;
        GroupTest::constantAlpha = alpha; GroupTest::bufferErase = erase;
        if (GroupTest::failBuffer) return E_FAIL;
        TestDC memory; static_cast<DcState&>(memory) = *dc;
        GroupBuffer buffer;
        GroupTest::buffers[&memory] = &buffer;
        auto cleanup = wil::scope_exit([&] { GroupTest::buffers.erase(&memory); });
        try
        {
            if (GroupTest::duringBuffer) GroupTest::duringBuffer();
            draw(&memory, &buffer, buffer.samples.data(), 3);
        }
        catch (HRESULT hr) { ++GroupTest::discards; return hr; }
        ++GroupTest::commits; GroupTest::committed = buffer.samples;
        return S_OK;
    }
}
namespace GlassCore::Root
{
    inline std::atomic<bool> g_shutdown{false};
    struct RootHookCache { std::vector<std::pair<HMODULE, size_t>> paintHooks, dcHooks; };
    inline void PushPaint(HMODULE module, size_t index, RootHookCache& cache)
    { cache.paintHooks.emplace_back(module, index); }
    inline void PushDc(HMODULE module, size_t index, RootHookCache& cache)
    { cache.dcHooks.emplace_back(module, index); }
    template <size_t I, typename Fn> Fn GetRootOrg(HMODULE module)
    {
        static_assert(I >= OHK_DELPHI_GROUP_TEXT && I <= OHK_DELPHI_GROUP_GDIGRADIENT);
        constexpr const char* names[]{"DrawTextExW", "FillRect", "GradientFill", "GdiGradientFill"};
        const auto name = names[I - OHK_DELPHI_GROUP_TEXT];
        GroupTest::originalModule = module;
        if (!module || !module->refs[name]) return nullptr;
        const auto i = module->originals.find(name);
        return i == module->originals.end() ? nullptr : reinterpret_cast<Fn>(i->second);
    }
}
#define GetClientRect GroupGetClientRect
#define GetSysColor GroupGetSysColor
#define DrawTextWithAlpha GroupDrawTextWithAlpha
#define PatBlt GroupPatBlt
