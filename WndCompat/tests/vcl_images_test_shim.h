// Environment doubles only. The image suite compiles complete production
// bitmap utility, ToolbarHooks and TPanelDraw translation units below this shim.
// Byte/parameter/resource checks are not a Windows GDI/DWM renderer or MSVC ABI test.
#pragma once
#define GLASS_TEST_REAL_OPAQUE_BITMAP
#define HookDispatcher LegacyHookDispatcher
#include "panel_test_shim.h"
#undef HookDispatcher
#include <set>
#include <limits>
#include <stdexcept>

inline constexpr DWORD SRCCOPY = 0x00cc0020, SRCAND = 0x008800c6;
inline constexpr int MAX_PATH = 260, COLOR_WINDOWTEXT = 8;
inline constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4;
inline constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 2;
namespace WndCompat::Menu
{
    struct IconBackgroundColorRemoval
    { bool enable{}; DWORD colorTreatAsTransparent{}; int colorTreatAsTransparentThreshold{}; };
}
namespace ImageTest
{
    inline std::set<HBITMAP> bitmaps;
    inline int creates = 0, deletes = 0, reads = 0, flushes = 0, selects = 0;
    inline int nativeMasks = 0, nativeStretches = 0, nativeBits = 0, alphaBlends = 0, keys = 0, queries = 0;
    inline bool failPlaceholder = false, failDib = false, nullBits = false, failRead = false;
    // The 1-based DIB section that fails, or comes without bits; 0 = none. And what
    // a new one holds: Windows promises nothing about it.
    inline int dibs = 0, failDibAt = 0, nullBitsAt = 0;
    inline bool dirtyDib = false;
    inline bool partialRead = false, failFlush = false, failMetadata = false, failNative = false;
    inline int failSelectAt = -1;
    inline bool selectReturnsNull = false, glow = false;
    inline HMODULE caller{}, sysmon{};
    inline HDC readDc{};
    inline std::unordered_map<HMODULE, std::wstring> modulePaths;
    inline std::unordered_map<HMODULE, std::set<std::string>> registrationFailures;
    inline std::function<void()> duringNative;
    inline bool reentered = false;
    inline WndCompat::Menu::IconBackgroundColorRemoval removal{};
    struct BltCall
    {
        HDC dest{}, source{};
        int x{}, y{}, w{}, h{}, sx{}, sy{}, sw{}, sh{}, mx{}, my{};
        HBITMAP mask{}, selected{};
        DWORD rop{}, entryError{};
        DcState state{};
        std::vector<BYTE> pixels;
    };
    inline BltCall last;
    inline std::vector<BltCall> calls;
    // What the toolbar gave the text compositor last.
    inline HDC textDc{};
    inline std::wstring textString;
    inline UINT textFlags{};
    inline RECT textRect{};

    inline HBITMAP MakeBitmap(int width, int height, int bpp = 32, bool dib = true)
    {
        auto bmp = std::make_unique<TestBitmap>();
        bmp->info.bmWidth = width; bmp->info.bmHeight = height;
        bmp->info.bmWidthBytes = width * 4; bmp->info.bmBitsPixel = bpp;
        bmp->pixels.resize(static_cast<size_t>(width) * height * 4);
        bmp->info.bmBits = dib ? bmp->Data() : nullptr;
        auto* result = bmp.release(); bitmaps.insert(result); ++creates;
        return result;
    }
    inline HBITMAP Selected(HDC dc)
    {
        const auto p = dc ? PanelCurrentObject(dc, OBJ_BITMAP) : nullptr;
        return bitmaps.count(static_cast<HBITMAP>(p)) ? static_cast<HBITMAP>(p) : nullptr;
    }
    inline BOOL Record(BltCall call)
    {
        call.selected = Selected(call.source); call.entryError = GetLastError();
        if (call.source) call.state = *call.source;
        if (call.selected) call.pixels = call.selected->pixels;
        last = call; calls.push_back(call);
        if (duringNative && !reentered)
        { reentered = true; duringNative(); reentered = false; }
        SetLastError(0x51);
        return !failNative;
    }

    // Match the ref-delta contract: nested EnableHook returns false even when
    // its ref increased. Registration failure is injected explicitly; this
    // double does NOT emulate executable IAT trampolines or their chain.
    template <size_t N, size_t M> struct Dispatcher
    {
        const GlassCore::Hooks::HookDispatcherDependency<N, M>& dependency;
        void* moduleAddress{};
        std::array<int, N> refs{};
        Dispatcher(const GlassCore::Hooks::HookDispatcherDependency<N, M>& dep, void* base)
            : dependency(dep), moduleAddress(base) {}
        void CacheHookData() {}
        int GetRef(size_t i) const { return refs[i]; }
        bool EnableHook(size_t i, bool enable)
        {
            auto* mod = static_cast<HMODULE>(moduleAddress);
            const auto name = std::get<1>(dependency[i]);
            if (!mod || !name || !mod->originals.count(name)) return false;
            const auto detour = std::get<2>(dependency[i]);
            if (enable)
            {
                if (refs[i]) { ++refs[i]; ++mod->refs[name]; return false; }
                if (registrationFailures[mod].count(name)) return false;
                ++refs[i]; ++mod->refs[name]; mod->hooks[name] = detour; return true;
            }
            if (!refs[i]) return false;
            --refs[i]; --mod->refs[name];
            if (refs[i]) return false;
            mod->hooks.erase(name); return true;
        }
        template <size_t I, typename Fn> Fn GetOrg() const
        {
            auto* mod = static_cast<HMODULE>(moduleAddress);
            return refs[I] ? reinterpret_cast<Fn>(mod->originals.at(std::get<1>(dependency[I]))) : nullptr;
        }
        void DisableAllHooks()
        { for (size_t i = 0; i < N; ++i) while (refs[i]) EnableHook(i, false); }
    };
}
namespace GlassCore::Hooks
{
    template <size_t N, size_t M> using HookDispatcher = ImageTest::Dispatcher<N, M>;
}

inline BOOL ImageDeleteObject(HBITMAP bmp)
{
    assert(ImageTest::bitmaps.count(bmp));
    for (const auto& [dc, selected] : PanelTest::bitmaps) { (void)dc; assert(selected != bmp); }
    ImageTest::bitmaps.erase(bmp); delete bmp; ++ImageTest::deletes;
    SetLastError(0xdead); return TRUE;
}
inline HBITMAP CreateBitmap(int w, int h, UINT planes, UINT bpp, const void* data)
{
    assert(w == 1 && h == 1 && planes == 1 && bpp == 1 && !data);
    return ImageTest::failPlaceholder ? nullptr : ImageTest::MakeBitmap(w, h, bpp, false);
}
inline HBITMAP ImageCreateDIBSection(HDC, const BITMAPINFO* bi, UINT usage, void** data, void*, DWORD offset)
{
    assert(bi && bi->bmiHeader.biWidth > 0 && bi->bmiHeader.biHeight < 0);
    assert(bi->bmiHeader.biBitCount == 32 && bi->bmiHeader.biPlanes == 1 && !offset && usage == DIB_RGB_COLORS);
    const int index = ++ImageTest::dibs;
    if (ImageTest::failDib || index == ImageTest::failDibAt) return nullptr;
    auto* bmp = ImageTest::MakeBitmap(bi->bmiHeader.biWidth, -bi->bmiHeader.biHeight);
    if (ImageTest::dirtyDib) std::fill(bmp->pixels.begin(), bmp->pixels.end(), BYTE(0xcd));
    *data = ImageTest::nullBits || index == ImageTest::nullBitsAt ? nullptr : bmp->Data(); return bmp;
}
inline HGDIOBJ ImageSelectObject(HDC dc, HGDIOBJ bitmap)
{
    if (++ImageTest::selects == ImageTest::failSelectAt)
        return ImageTest::selectReturnsNull ? nullptr : HGDI_ERROR;
    assert(PanelTest::bitmaps.count(dc));
    return std::exchange(PanelTest::bitmaps.at(dc), bitmap);
}
inline int ImageGetObjectW(HGDIOBJ object, int bytes, void* out)
{
    const auto bmp = static_cast<HBITMAP>(object);
    if (ImageTest::bitmaps.count(bmp))
    {
        if (ImageTest::failMetadata) return 0;
        if (bytes == sizeof(BITMAP)) { *static_cast<BITMAP*>(out) = bmp->info; return bytes; }
        if (bytes == sizeof(DIBSECTION))
        {
            static_cast<DIBSECTION*>(out)->dsBm = bmp->info;
            return bmp->info.bmBits ? bytes : int(sizeof(BITMAP));
        }
        return 0;
    }
    if (reinterpret_cast<ULONG_PTR>(object) <= 5) return 0;
    return GetObjectW(static_cast<HBRUSH>(object), bytes, out);
}
inline int GetDIBits(HDC dc, HBITMAP bmp, UINT start, UINT count, void* data, BITMAPINFO* bi, UINT usage)
{
    assert(ImageTest::bitmaps.count(bmp) && !start && count == unsigned(bmp->info.bmHeight));
    assert(bi->bmiHeader.biWidth == bmp->info.bmWidth && bi->bmiHeader.biHeight == -bmp->info.bmHeight);
    assert(bi->bmiHeader.biBitCount == 32 && usage == DIB_RGB_COLORS);
    // Unlike a permissive double, reject the documented invalid selected read.
    for (const auto& [selectedDc, selected] : PanelTest::bitmaps) { (void)selectedDc; assert(selected != bmp); }
    ++ImageTest::reads; ImageTest::readDc = dc;
    if (ImageTest::failRead) return 0;
    std::memcpy(data, bmp->Data(), bmp->pixels.size());
    return ImageTest::partialRead ? int(count) - 1 : int(count);
}
inline BOOL GdiFlush() { ++ImageTest::flushes; return !ImageTest::failFlush; }
inline HDC GetDC(HWND) { static TestDC screen; return &screen; }
inline int ReleaseDC(HWND, HDC) { return 1; }
inline BOOL MaskBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, HBITMAP mask, int mx, int my, DWORD rop)
{
    ++ImageTest::nativeMasks;
    ImageTest::BltCall call{}; call.dest=dc; call.source=src; call.x=x; call.y=y; call.w=w; call.h=h;
    call.sx=sx; call.sy=sy; call.sw=w; call.sh=h; call.mask=mask; call.mx=mx; call.my=my; call.rop=rop;
    return ImageTest::Record(call);
}
inline BOOL ImageStretchBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
    ++ImageTest::nativeStretches;
    ImageTest::BltCall call{}; call.dest=dc; call.source=src; call.x=x; call.y=y; call.w=w; call.h=h;
    call.sx=sx; call.sy=sy; call.sw=sw; call.sh=sh; call.rop=rop;
    return ImageTest::Record(call);
}
inline BOOL BitBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    ++ImageTest::nativeBits;
    ImageTest::BltCall call{}; call.dest=dc; call.source=src; call.x=x; call.y=y; call.w=w; call.h=h;
    call.sx=sx; call.sy=sy; call.sw=w; call.sh=h; call.rop=rop;
    return ImageTest::Record(call);
}
// Icons: a narrow model of DrawIconEx over the bitmap selected into the DC.
// Covered pixels are blended as GDI blends them; an icon without alpha leaves
// A=0 under itself; an inverting pixel XORs. Not user32, and no clipping.
namespace IconTest
{
    struct Call { HDC dc{}; int x{}, y{}, cx{}, cy{}; HICON icon{}; UINT step{}, flags{}; HBRUSH flicker{}; DWORD entryError{}; };
    inline std::vector<Call> calls;
    inline int failDrawAt = 0;                   // 1-based index of the call that fails, 0 = never
    inline std::function<void()> duringDraw;     // once, from inside the next call
    inline bool failInfo = false, failBlend = false;
    inline int infos = 0, metrics = 0, layoutQueries = 0;
    inline int systemWidth = 32, systemHeight = 40;
    inline std::unordered_map<HDC, DWORD> layouts;
    // What was blended last: the geometry and the picture as it was then.
    struct Blend
    {
        HDC dest{}; int x{}, y{}, w{}, h{}, sx{}, sy{}, sw{}, sh{};
        BLENDFUNCTION mode{};
        int width{}, height{};
        std::vector<BYTE> pixels;
    };
    inline Blend blend;
}
inline BOOL ImageAlphaBlend(HDC dest, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, BLENDFUNCTION mode)
{
    ++ImageTest::alphaBlends;
    IconTest::blend = IconTest::Blend{dest, x, y, w, h, sx, sy, sw, sh, mode, 0, 0, {}};
    if (const auto picture = ImageTest::Selected(src))
    {
        IconTest::blend.width = picture->info.bmWidth; IconTest::blend.height = picture->info.bmHeight;
        IconTest::blend.pixels = picture->pixels;
    }
    SetLastError(0x53);
    return !IconTest::failBlend;
}
inline int GetSystemMetrics(int index)
{
    if (index != SM_CXICON && index != SM_CYICON) return 17;
    ++IconTest::metrics;
    return index == SM_CXICON ? IconTest::systemWidth : IconTest::systemHeight;
}
inline DWORD GetLayout(HDC dc)
{
    ++IconTest::layoutQueries;
    const auto it = IconTest::layouts.find(dc);
    return it == IconTest::layouts.end() ? 0u : it->second;
}
inline BOOL GetIconInfo(HICON icon, ICONINFO* info)
{
    ++IconTest::infos; SetLastError(0x54);
    // A call that fails promises nothing about what it left in its output.
    info->hbmMask = info->hbmColor = reinterpret_cast<HBITMAP>(std::uintptr_t(0xbad));
    if (IconTest::failInfo || !icon) return FALSE;
    *info = ICONINFO{};
    info->hbmMask = ImageTest::MakeBitmap(icon->width, icon->colour ? icon->height : icon->height * 2, 1, false);
    if (icon->colour) info->hbmColor = ImageTest::MakeBitmap(icon->width, icon->height, 32, false);
    return TRUE;
}
inline BOOL DrawIconEx(HDC dc, int x, int y, HICON icon, int cx, int cy, UINT step, HBRUSH flicker, UINT flags)
{
    IconTest::calls.push_back({dc, x, y, cx, cy, icon, step, flags, flicker, GetLastError()});
    if (const auto inside = std::exchange(IconTest::duringDraw, {})) inside();
    SetLastError(0x52);
    if (static_cast<int>(IconTest::calls.size()) == IconTest::failDrawAt) return FALSE;
    const auto surface = ImageTest::Selected(dc);
    if (!surface || !icon) return TRUE;
    const int w = cx ? cx : icon->width, h = cy ? cy : icon->height;
    for (int row = 0; row < h; ++row) for (int column = 0; column < w; ++column)
    {
        const int tx = x + column, ty = y + row;
        if (tx < 0 || ty < 0 || tx >= surface->info.bmWidth || ty >= surface->info.bmHeight) continue;
        const auto& p = icon->pixels[static_cast<size_t>(row * icon->height / h) * icon->width
            + static_cast<size_t>(column * icon->width / w)];
        BYTE* d = surface->pixels.data() + (static_cast<size_t>(ty) * surface->info.bmWidth + tx) * 4; // B G R A
        if (p.inverts) { d[0] ^= p.b; d[1] ^= p.g; d[2] ^= p.r; continue; }
        const auto mix = [&](BYTE colour, BYTE under) {
            return static_cast<BYTE>((unsigned(colour) * p.alpha + unsigned(under) * (255u - p.alpha) + 127u) / 255u);
        };
        d[0] = mix(p.b, d[0]); d[1] = mix(p.g, d[1]); d[2] = mix(p.r, d[2]);
        if (icon->alpha) d[3] = static_cast<BYTE>(p.alpha + unsigned(d[3]) * (255u - p.alpha) / 255u);
        else if (p.alpha) d[3] = 0;
    }
    return TRUE;
}
// What the Debug line of a toolbar session reads about its window.
inline constexpr int GWL_STYLE = -16, TP_BUTTON = 1, TMT_COMPOSITED = 2204;
namespace ImageTest
{
    inline int styleQueries = 0, themeQueries = 0;
    inline std::unordered_map<HWND, LONG> styles;
    inline BOOL composited = FALSE;
    inline HTHEME themeAsked{};
    inline int partAsked{}, stateAsked{}, propertyAsked{};
}
inline LONG GetWindowLongW(HWND w, int index)
{
    assert(index == GWL_STYLE);
    ++ImageTest::styleQueries;
    const auto it = ImageTest::styles.find(w);
    return it == ImageTest::styles.end() ? 0 : it->second;
}
inline HRESULT GetThemeBool(HTHEME theme, int part, int state, int property, BOOL* value)
{
    ++ImageTest::themeQueries;
    ImageTest::themeAsked = theme; ImageTest::partAsked = part;
    ImageTest::stateAsked = state; ImageTest::propertyAsked = property;
    *value = ImageTest::composited;
    return S_OK;
}
inline HBRUSH CreateSolidBrush(COLORREF c) { return new LOGBRUSH{BS_SOLID,c}; }
inline BOOL FAILED(HRESULT hr) { return hr < 0; }
inline DWORD GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD capacity)
{
    const auto name = ImageTest::modulePaths.count(module) ? ImageTest::modulePaths.at(module) : LR"(C:\App\Host.exe)";
    if (name.size() >= capacity) return 0;
    std::wcscpy(out, name.c_str()); return name.size();
}
inline BOOL GetModuleHandleExW(DWORD, LPCWSTR address, HMODULE* out)
{ *out = reinterpret_cast<HMODULE>(const_cast<wchar_t*>(address)); return *out != nullptr; }
inline HMODULE ImageGetModuleHandleW(LPCWSTR name)
{
    if (std::wstring_view(name) == L"uxtheme.dll") return Test::ux;
    if (std::wstring_view(name) == L"sysmon.ocx") return ImageTest::sysmon;
    return PanelGetModuleHandleW(name);
}
namespace wil
{
    struct BitmapDeleter { void operator()(HBITMAP bmp) const { ImageDeleteObject(bmp); } };
    using unique_hbitmap = std::unique_ptr<TestBitmap, BitmapDeleter>;
}
namespace GlassCore::PaintDispatcher
{
    struct PaintProvider { void (*onBegin)(HWND, HMODULE); void (*onEnd)(HWND, HMODULE); };
    inline std::vector<PaintProvider> imageProviders;
    inline void RegisterPaintProvider(PaintProvider p) { imageProviders.push_back(p); }
}
namespace GlassCore::TextGlow { inline void EnsureTextGlowClientRegistered(HWND) {} }
namespace GlassCore::Settings
{
    inline void QueryIconBackgroundColorRemovalContext(std::initializer_list<LPCWSTR>, WndCompat::Menu::IconBackgroundColorRemoval& out)
    { ++ImageTest::queries; out = ImageTest::removal; }
}
namespace GlassCore::Text
{
    inline bool IsInkGlowEffectActiveForPaintSession(HWND) { return ImageTest::glow; }
    inline HRESULT DrawTextWithAlpha(HDC dc, LPCWSTR text, int n, LPRECT r, UINT f, int& result)
    {
        ImageTest::textDc = dc; ImageTest::textString.assign(text, n > 0 ? n : 0);
        ImageTest::textFlags = f; ImageTest::textRect = *r;
        return DrawTextWithAlpha(dc, text, n, r, f, result, true);
    }
    inline HRESULT DrawThemeContent(HDC dc, const RECT& r, const RECT* clip, const RECT* exclude, DWORD flags,
        const std::function<void(HDC,HPAINTBUFFER,RGBQUAD*,int)>& draw)
    { return DrawThemeContent(dc, r, clip, exclude, flags, draw, 255, false, true, false); }
}
namespace GlassCore::GdiUtils
{
    inline void ApplyIconBackgroundAlpha(HBITMAP, const WndCompat::Menu::IconBackgroundColorRemoval&)
    { ++ImageTest::keys; }
}
namespace Shared::WinApiUtils
{
    inline HBRUSH CreateSolidColorBrushWithAlpha(COLORREF c, BYTE) { return CreateSolidBrush(c); }
}
#undef GetModuleHandleW
#define GetModuleHandleW ImageGetModuleHandleW
#define GetObject ImageGetObjectW
#define GetObjectW ImageGetObjectW
#define CreateDIBSection ImageCreateDIBSection
#define SelectObject ImageSelectObject
#define DeleteObject ImageDeleteObject
#define StretchBlt ImageStretchBlt
#define GdiAlphaBlend ImageAlphaBlend
#define _ReturnAddress() reinterpret_cast<void*>(ImageTest::caller)
