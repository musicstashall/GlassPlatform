// Environment doubles for the complete production TResVersionEditorDraw
// translation unit, on top of the portable base types only. This file adds the
// EXE's two imports (ExtTextOutA, FillRect) as slots a hook can take, a text
// renderer that records what it is asked to draw, and a narrow ANSI decoder.
// It is not Windows GDI, DWM, uxtheme, the NLS tables, a real PE import table
// or the VCL.
#pragma once
#include "coolbar_test_shim.h"
#include <climits>
#include <stdexcept>
#include <thread>

using BYTE = std::uint8_t;
using ULONG_PTR = std::uintptr_t;
using LONG_PTR = std::intptr_t;
struct CHARSETINFO { UINT ciCharset{}, ciACP{}; };
inline constexpr int MM_TEXT = 1, MM_ANISOTROPIC = 8, GWLP_HINSTANCE = -6;
inline constexpr UINT TA_NOUPDATECP = 0, TA_UPDATECP = 1, TA_LEFT = 0, TA_RIGHT = 2, TA_CENTER = 6;
inline constexpr UINT TA_TOP = 0, TA_BOTTOM = 8, TA_BASELINE = 24;
inline constexpr UINT ETO_RTLREADING = 0x80, ETO_PDY = 0x2000, BS_HATCHED = 2;
inline constexpr int DEFAULT_CHARSET = 1, SYMBOL_CHARSET = 2, RUSSIAN_CHARSET = 204, SHIFTJIS_CHARSET = 128;
inline constexpr int TCI_SRCCHARSET = 1, CP_ACP = 0;
inline BOOL FAILED(HRESULT hr) { return hr < 0; }

namespace ResTest
{
    // DC state the base DcState does not carry.
    struct Extra
    {
        UINT align = TA_LEFT | TA_TOP | TA_NOUPDATECP;
        int map = MM_TEXT, charset = DEFAULT_CHARSET;
        int spacing = 0; // SetTextCharacterExtra
    };
    inline std::unordered_map<HDC, Extra> extra;
    inline std::unordered_map<HWND, HMODULE> instances;
    inline std::unordered_map<HDC, DWORD> objectTypes; // a DC that is neither a window's nor a memory DC
    inline thread_local DWORD error{};
    inline COLORREF windowColor = 0xffffff, faceColor = 0xf0f0f0;

    // Everything that reaches the surface, in order.
    //  NativeText / NativeFill: the true functions, reached through the original
    //      a hook was given, or by the EXE when its import is not hooked.
    //  DirectText / DirectFill: the production unit calling the API by name.
    //  Black / Fill / Glyphs: what the unit drew itself.
    enum class Kind { NativeText, NativeFill, DirectText, DirectFill, Black, Fill, Glyphs };
    struct Event
    {
        Kind kind{};
        HDC dc{};
        int x{}, y{};
        UINT options{};
        const RECT* rectPointer{};
        RECT rect{};
        LPCSTR ansi{};                   // native calls: the caller's own pointer
        int count{};
        const INT* dxPointer{};
        std::wstring text;               // glyphs
        std::vector<INT> dx;             // glyphs
        HBRUSH brush{};                  // native fills: the caller's own brush
        COLORREF color{};
        unsigned alpha{};
        void* original{};
        DWORD entryError{};
        DcState state{};
    };
    inline thread_local std::vector<Event> events;
    inline BOOL nativeTextResult = TRUE;
    inline int nativeFillResult = 1;
    inline bool nativeLeavesError = false;
    inline HRESULT renderResult = S_OK;
    inline bool throwRenderer = false, failBlack = false, failBrush = false, failFill = false, failObject = false;
    inline bool failTranslate = false, failConvert = false, partialConvert = false, throwConvert = false;
    inline UINT codepage{};
    inline int conversions = 0;
    inline std::function<void()> duringRender, duringNative;
    inline thread_local ULONGLONG clock = 100;
    struct Log { std::wstring format; std::vector<long long> numbers; std::vector<std::wstring> strings; };
    inline thread_local std::vector<Log> logs;

    inline int Count(Kind kind)
    { return static_cast<int>(std::count_if(events.begin(), events.end(), [=](const Event& e) { return e.kind == kind; })); }
}

inline DWORD GetLastError() { return ResTest::error; }
inline void SetLastError(DWORD error) { ResTest::error = error; }
inline LONG_PTR GetWindowLongPtrW(HWND w, int index)
{
    assert(index == GWLP_HINSTANCE);
    const auto it = ResTest::instances.find(w);
    return reinterpret_cast<LONG_PTR>(it == ResTest::instances.end()
        ? GlassCore::ModuleHandler::g_entryPointModule : it->second);
}
inline DWORD ResGetObjectType(HDC dc)
{
    SetLastError(0xe10e);
    const auto it = ResTest::objectTypes.find(dc);
    return it == ResTest::objectTypes.end() ? GetObjectType(dc) : it->second;
}
inline int GetMapMode(HDC dc) { SetLastError(0xe101); return ResTest::extra[dc].map; }
inline UINT GetTextAlign(HDC dc) { SetLastError(0xe102); return ResTest::extra[dc].align; }
inline int GetTextCharset(HDC dc) { SetLastError(0xe103); return ResTest::extra[dc].charset; }
inline int GetTextCharacterExtra(HDC dc) { SetLastError(0xe104); return ResTest::extra[dc].spacing; }
inline BOOL TranslateCharsetInfo(DWORD* source, CHARSETINFO* out, DWORD mode)
{
    SetLastError(0xe105);
    assert(mode == TCI_SRCCHARSET);
    if (ResTest::failTranslate) return FALSE;
    const auto charset = reinterpret_cast<ULONG_PTR>(source);
    if (charset != 0 && charset != SYMBOL_CHARSET && charset != SHIFTJIS_CHARSET && charset != RUSSIAN_CHARSET)
        return FALSE;
    out->ciACP = charset == SHIFTJIS_CHARSET ? 932 : charset == RUSSIAN_CHARSET ? 1251
        : charset == SYMBOL_CHARSET ? 42 : 1252;
    return TRUE;
}
// Only the byte sequences of the fixtures: Cyrillic capitals in 1251 and one
// double-byte pair in 932. Every other byte maps to itself.
inline int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR text, int bytes, LPWSTR out, int capacity)
{
    SetLastError(0xe106); ++ResTest::conversions; ResTest::codepage = cp;
    assert(flags == 0 && text && bytes > 0);
    if (ResTest::throwConvert) throw std::bad_alloc{};
    if (ResTest::failConvert || (out && ResTest::partialConvert)) return 0;
    std::wstring value;
    for (int i = 0; i < bytes; ++i)
    {
        const auto ch = static_cast<unsigned char>(text[i]);
        if (cp == 932 && ch == 0x82 && i + 1 < bytes && static_cast<unsigned char>(text[i + 1]) == 0xa0)
        { value.push_back(L'\u3042'); ++i; }
        else if (cp == 1251 && ch >= 0xc0) value.push_back(wchar_t(0x410 + ch - 0xc0));
        else value.push_back(wchar_t(ch));
    }
    if (out)
    {
        assert(capacity >= static_cast<int>(value.size()));
        std::copy(value.begin(), value.end(), out);
    }
    return static_cast<int>(value.size());
}
inline COLORREF ResGetSysColor(int index)
{
    SetLastError(0xe107);
    assert(index == COLOR_WINDOW || index == COLOR_BTNFACE);
    return index == COLOR_WINDOW ? ResTest::windowColor : ResTest::faceColor;
}
inline int ResGetObjectW(HBRUSH brush, int bytes, void* out)
{
    SetLastError(0xe108);
    if (ResTest::failObject || !brush || bytes != static_cast<int>(sizeof(LOGBRUSH))) return 0;
    std::memcpy(out, brush, sizeof(LOGBRUSH)); return bytes;
}
inline BOOL ResPatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    SetLastError(0xe109);
    assert(dc && w > 0 && h > 0 && rop == BLACKNESS);
    if (ResTest::failBlack) return FALSE;
    ResTest::Event e{}; e.kind = ResTest::Kind::Black; e.dc = dc; e.rect = RECT{x, y, x + w, y + h}; e.state = *dc;
    ResTest::events.push_back(e);
    return TRUE;
}
namespace Shared::WinApiUtils
{
    // lbHatch carries the alpha the production code asked for.
    inline HBRUSH CreateSolidColorBrushWithAlpha(COLORREF color, BYTE alpha)
    {
        SetLastError(0xe10a);
        return ResTest::failBrush ? nullptr : new LOGBRUSH{BS_SOLID, color, alpha};
    }
}
namespace wil { using unique_hbrush = std::unique_ptr<LOGBRUSH>; }
// FillRect called by name from the production unit: its own opaque fill (a
// brush made by CreateSolidColorBrushWithAlpha) or the fallback without an original.
inline int ResFillRect(HDC dc, const RECT* rect, HBRUSH brush)
{
    SetLastError(0xe10b);
    assert(dc && rect && brush);
    if (ResTest::failFill) return 0;
    ResTest::Event e{}; e.kind = brush->lbHatch ? ResTest::Kind::Fill : ResTest::Kind::DirectFill; e.dc = dc; e.rect = *rect;
    e.brush = brush; e.color = brush->lbColor; e.alpha = static_cast<unsigned>(brush->lbHatch); e.state = *dc;
    ResTest::events.push_back(e);
    return 1;
}
// The true functions.
inline BOOL WINAPI ResNativeExtTextOutA(HDC dc, int x, int y, UINT options, const RECT* rect,
    LPCSTR text, UINT count, const INT* dx)
{
    ResTest::Event e{}; e.kind = ResTest::Kind::NativeText; e.dc = dc; e.x = x; e.y = y; e.options = options;
    e.rectPointer = rect; if (rect) e.rect = *rect;
    e.ansi = text; e.count = static_cast<int>(count); e.dxPointer = dx;
    e.entryError = GetLastError(); if (dc) e.state = *dc;
    ResTest::events.push_back(e);
    if (ResTest::duringNative) ResTest::duringNative();
    if (!ResTest::nativeLeavesError) SetLastError(0x51);
    return ResTest::nativeTextResult;
}
inline int WINAPI ResNativeFillRect(HDC dc, const RECT* rect, HBRUSH brush)
{
    ResTest::Event e{}; e.kind = ResTest::Kind::NativeFill; e.dc = dc;
    e.rectPointer = rect; if (rect) e.rect = *rect;
    e.brush = brush; e.entryError = GetLastError(); if (dc) e.state = *dc;
    ResTest::events.push_back(e);
    if (ResTest::duringNative) ResTest::duringNative();
    if (!ResTest::nativeLeavesError) SetLastError(0x52);
    return ResTest::nativeFillResult;
}
// ExtTextOutA called by name from the production unit: the fallback without an original.
inline BOOL WINAPI ExtTextOutA(HDC dc, int x, int y, UINT options, const RECT* rect,
    LPCSTR text, UINT count, const INT* dx)
{
    ResTest::Event e{}; e.kind = ResTest::Kind::DirectText; e.dc = dc; e.x = x; e.y = y; e.options = options;
    e.rectPointer = rect; e.ansi = text; e.count = static_cast<int>(count); e.dxPointer = dx;
    e.entryError = GetLastError();
    ResTest::events.push_back(e);
    return TRUE;
}
// A call the EXE makes through its import table: the installed hook, else the true function.
inline BOOL ExeExtTextOutA(TestModule& exe, HDC dc, int x, int y, UINT options, const RECT* rect,
    LPCSTR text, UINT count, const INT* dx = nullptr)
{
    using Fn = BOOL (WINAPI*)(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*);
    const auto it = exe.hooks.find("ExtTextOutA");
    return it != exe.hooks.end() ? reinterpret_cast<Fn>(it->second)(dc, x, y, options, rect, text, count, dx)
        : ResNativeExtTextOutA(dc, x, y, options, rect, text, count, dx);
}
inline int ExeFillRect(TestModule& exe, HDC dc, const RECT* rect, HBRUSH brush)
{
    using Fn = int (WINAPI*)(HDC, const RECT*, HBRUSH);
    const auto it = exe.hooks.find("FillRect");
    return it != exe.hooks.end() ? reinterpret_cast<Fn>(it->second)(dc, rect, brush) : ResNativeFillRect(dc, rect, brush);
}
inline ULONGLONG ResTickCount64() { return ResTest::clock; }
template <typename... Args> inline void ResDebugLog(int, LPCWSTR format, Args... args)
{
    ResTest::Log log{format, {}, {}};
    const auto capture = [&](auto value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_convertible_v<T, LPCWSTR>) log.strings.emplace_back(value ? value : L"(null)");
        else if constexpr (std::is_pointer_v<T>) log.numbers.push_back(static_cast<long long>(reinterpret_cast<std::intptr_t>(value)));
        else log.numbers.push_back(static_cast<long long>(value));
    };
    (capture(args), ...);
    ResTest::logs.push_back(std::move(log));
    SetLastError(0xe10c); // a logger must not leak its error into the caller's result
}
namespace GlassCore::PaintDispatcher
{
    struct PaintProvider { void (*onBegin)(HWND, HMODULE); void (*onEnd)(HWND, HMODULE); };
    inline std::vector<PaintProvider> resProviders;
    inline void RegisterPaintProvider(PaintProvider p) { resProviders.push_back(p); }
}
namespace GlassCore::Text
{
    // Records the request; draws nothing. The production renderer is not compiled.
    inline HRESULT ResExtTextOutWithAlpha(HDC dc, int x, int y, UINT options, const RECT* rect,
        LPCWSTR text, UINT count, const INT* dx, decltype(&::ExtTextOutW) original)
    {
        SetLastError(0xe10d);
        assert(dc && text && count && original);
        ResTest::Event e{}; e.kind = ResTest::Kind::Glyphs; e.dc = dc; e.x = x; e.y = y; e.options = options;
        e.rectPointer = rect; if (rect) e.rect = *rect;
        e.text.assign(text, count); e.count = static_cast<int>(count);
        e.dxPointer = dx; if (dx) e.dx.assign(dx, dx + count);
        e.original = reinterpret_cast<void*>(original); e.state = *dc;
        ResTest::events.push_back(e);
        if (ResTest::duringRender) ResTest::duringRender();
        if (ResTest::throwRenderer) throw std::runtime_error("injected renderer failure");
        return ResTest::renderResult;
    }
}
#define GetObjectType ResGetObjectType
#define GetSysColor ResGetSysColor
#define GetObjectW ResGetObjectW
#define GetTickCount64 ResTickCount64
#define DebugLog ResDebugLog
#define PatBlt ResPatBlt
#define FillRect ResFillRect
#define ExtTextOutWithAlpha ResExtTextOutWithAlpha
