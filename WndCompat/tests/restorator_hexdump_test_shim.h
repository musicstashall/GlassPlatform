// Environment doubles for the complete production RestoratorHexDump translation
// unit, on top of the portable base types only. This file adds an
// export/trampoline model for the two detoured text functions, a text renderer
// that records what it is asked to draw, cell-based text metrics and a narrow
// ANSI decoder. It is not Windows GDI, DWM, uxtheme, user32's tab expansion, the
// NLS tables, a real PE export, the Detours ABI or instruction patching.
#pragma once
#include "coolbar_test_shim.h"
#include <climits>
#include <stdexcept>
#include <thread>

using BYTE = std::uint8_t;
using ULONG_PTR = std::uintptr_t;
using LONG_PTR = std::intptr_t;
using FARPROC = void(*)();
struct SIZE { LONG cx{}, cy{}; };
struct TEXTMETRICW { LONG tmHeight{}, tmAveCharWidth{}; BYTE tmUnderlined{}, tmStruckOut{}; };
struct CHARSETINFO { UINT ciCharset{}, ciACP{}; };
inline constexpr int MM_TEXT = 1, MM_ANISOTROPIC = 8, GWLP_HINSTANCE = -6;
inline constexpr UINT TA_NOUPDATECP = 0, TA_UPDATECP = 1, TA_LEFT = 0, TA_RIGHT = 2, TA_CENTER = 6;
inline constexpr UINT TA_TOP = 0, TA_BOTTOM = 8, TA_BASELINE = 24;
inline constexpr UINT ETO_RTLREADING = 0x80, ETO_PDY = 0x2000;
inline constexpr int DEFAULT_CHARSET = 1, SYMBOL_CHARSET = 2, RUSSIAN_CHARSET = 204, SHIFTJIS_CHARSET = 128;
inline constexpr int TCI_SRCCHARSET = 1, CP_ACP = 0;
#define MAKELONG(low, high) \
    (static_cast<LONG>((static_cast<DWORD>(low) & 0xffffu) | ((static_cast<DWORD>(high) & 0xffffu) << 16)))
inline BOOL FAILED(HRESULT hr) { return hr < 0; }

namespace HexTest
{
    // DC state the base DcState does not carry.
    struct Extra
    {
        UINT align = TA_LEFT | TA_TOP | TA_NOUPDATECP;
        int map = MM_TEXT, charset = DEFAULT_CHARSET;
        int cell = 8, height = 16;      // one character cell of the fixed-pitch font
        int spacing = 0;                // SetTextCharacterExtra
        int overhang = 0;               // what the extent of a string adds to its last partial extent
        bool underline = false, strikeout = false;
    };
    inline std::unordered_map<HDC, Extra> extra;
    inline std::unordered_map<HWND, HMODULE> instances;
    // The newest stack record of a DC handle when it differs from an older one.
    inline std::unordered_map<HDC, HWND> newestOwner;
    inline thread_local DWORD error{};
    inline thread_local HMODULE caller{}; // the module a detour's return address lies in

    // gdi32 / user32 exports and what is installed on them.
    inline TestModule gdi32, user32;
    inline bool gdiAvailable = true, userAvailable = true, extExport = true, tabbedExport = true;
    inline int resolutions = 0;
    inline void* extDetour{};
    inline void* tabbedDetour{};
    inline thread_local int exportCalls = 0, trampolineCalls = 0;
    inline bool failBegin = false, failCommit = false;
    inline int failAttachAt = 0, failDetachAt = 0; // 1-based call index, 0 = never
    inline int writes = 0, attachCalls = 0, detachCalls = 0, attaches = 0, detaches = 0;
    struct Op { void** real{}; void* hook{}; bool attach{}; };
    inline std::vector<Op> pending;

    // Everything that reaches the surface, in order.
    enum class Kind { NativeExt, NativeTabbed, Black, Fill, Glyphs };
    struct Event
    {
        Kind kind{};
        HDC dc{};
        int x{}, y{};
        UINT options{};
        const RECT* rectPointer{};
        RECT rect{};
        LPCSTR ansi{};                   // native calls: the caller's own pointer
        int count{}, tabCount{}, origin{};
        const INT* dxPointer{};          // native calls and glyphs: the pointer passed
        std::wstring text;               // glyphs
        std::vector<INT> dx;             // glyphs
        COLORREF color{};
        unsigned alpha{};
        void* original{};
        DWORD entryError{};
        DcState state{};
    };
    inline thread_local std::vector<Event> events;
    inline BOOL nativeExtResult = TRUE;
    inline LONG nativeTabbedResult = MAKELONG(77, 13);
    inline bool nativeLeavesError = false;
    inline HRESULT renderResult = S_OK;
    inline bool throwRenderer = false, failBlack = false, failBrush = false, failFill = false;
    inline bool failMetrics = false, failExtent = false, shrinkingExtent = false;
    inline bool failTranslate = false, failConvert = false, partialConvert = false, throwConvert = false;
    inline UINT codepage{};
    inline int conversions = 0;
    inline std::function<void()> duringRender;
    inline thread_local ULONGLONG clock = 100;
    inline thread_local std::vector<std::wstring> logFormats, logStrings;

    inline int Count(Kind kind)
    { return static_cast<int>(std::count_if(events.begin(), events.end(), [=](const Event& e) { return e.kind == kind; })); }
    inline int CellsOf(wchar_t ch) { return ch == L'W' ? 2 : ch == L'\u3042' ? 2 : 1; }
}

inline DWORD GetLastError() { return HexTest::error; }
inline void SetLastError(DWORD error) { HexTest::error = error; }
inline LONG_PTR GetWindowLongPtrW(HWND w, int index)
{
    assert(index == GWLP_HINSTANCE);
    const auto it = HexTest::instances.find(w);
    return reinterpret_cast<LONG_PTR>(it == HexTest::instances.end()
        ? GlassCore::ModuleHandler::g_entryPointModule : it->second);
}
inline int GetMapMode(HDC dc) { SetLastError(0xd101); return HexTest::extra[dc].map; }
inline UINT GetTextAlign(HDC dc) { SetLastError(0xd102); return HexTest::extra[dc].align; }
inline int GetTextCharset(HDC dc) { SetLastError(0xd103); return HexTest::extra[dc].charset; }
inline int GetTextCharacterExtra(HDC dc) { SetLastError(0xd10d); return HexTest::extra[dc].spacing; }
inline BOOL TranslateCharsetInfo(DWORD* source, CHARSETINFO* out, DWORD mode)
{
    SetLastError(0xd104);
    assert(mode == TCI_SRCCHARSET);
    if (HexTest::failTranslate) return FALSE;
    const auto charset = reinterpret_cast<ULONG_PTR>(source);
    // Windows knows the symbol charset too: it has a code page of its own.
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
    SetLastError(0xd105); ++HexTest::conversions; HexTest::codepage = cp;
    assert(flags == 0 && text && bytes > 0);
    if (HexTest::throwConvert) throw std::bad_alloc{};
    if (HexTest::failConvert || (out && HexTest::partialConvert)) return 0;
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
inline BOOL GetTextMetricsW(HDC dc, TEXTMETRICW* out)
{
    SetLastError(0xd106);
    if (HexTest::failMetrics) return FALSE;
    const auto& e = HexTest::extra[dc];
    *out = TEXTMETRICW{e.height, e.cell, BYTE(e.underline), BYTE(e.strikeout)};
    return TRUE;
}
// Cell-based extents: a character is one cell wide, 'W' and the double-byte
// character two. Partial extents are cumulative, as GDI reports them.
inline BOOL GetTextExtentExPointW(HDC dc, LPCWSTR text, int count, int maxExtent, int* fit, int* partial, SIZE* size)
{
    SetLastError(0xd107);
    assert(dc && text && count > 0 && !maxExtent && !fit && partial && size);
    for (int i = 0; i < count; ++i) assert(text[i] != L'\t'); // tabs never reach GDI
    if (HexTest::failExtent) return FALSE;
    const auto& e = HexTest::extra[dc];
    LONG total = 0;
    for (int i = 0; i < count; ++i) { total += HexTest::CellsOf(text[i]) * e.cell + e.spacing; partial[i] = total; }
    if (HexTest::shrinkingExtent && count > 1) partial[count - 2] = total + 1;
    *size = SIZE{total + e.overhang, e.height};
    return TRUE;
}
inline BOOL HexPatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    SetLastError(0xd108);
    assert(dc && w > 0 && h > 0 && rop == BLACKNESS);
    if (HexTest::failBlack) return FALSE;
    HexTest::Event e{}; e.kind = HexTest::Kind::Black; e.dc = dc; e.rect = RECT{x, y, x + w, y + h}; e.state = *dc;
    HexTest::events.push_back(e);
    return TRUE;
}
inline int HexFillRect(HDC dc, const RECT* rect, HBRUSH brush)
{
    SetLastError(0xd109);
    assert(dc && rect && brush);
    if (HexTest::failFill) return 0;
    HexTest::Event e{}; e.kind = HexTest::Kind::Fill; e.dc = dc; e.rect = *rect;
    e.color = brush->lbColor; e.alpha = static_cast<unsigned>(brush->lbHatch); e.state = *dc;
    HexTest::events.push_back(e);
    return 1;
}
namespace Shared::WinApiUtils
{
    // lbHatch carries the alpha the production code asked for.
    inline HBRUSH CreateSolidColorBrushWithAlpha(COLORREF color, BYTE alpha)
    {
        SetLastError(0xd10a);
        return HexTest::failBrush ? nullptr : new LOGBRUSH{BS_SOLID, color, alpha};
    }
}
namespace wil { using unique_hbrush = std::unique_ptr<LOGBRUSH>; }

// The true functions: what a trampoline, or an export without a detour, reaches.
inline BOOL HexNativeExtTextOutA(HDC dc, int x, int y, UINT options, const RECT* rect,
    LPCSTR text, UINT count, const INT* dx)
{
    HexTest::Event e{}; e.kind = HexTest::Kind::NativeExt; e.dc = dc; e.x = x; e.y = y; e.options = options;
    e.rectPointer = rect; if (rect) e.rect = *rect;
    e.ansi = text; e.count = static_cast<int>(count); e.dxPointer = dx;
    e.entryError = GetLastError(); if (dc) e.state = *dc;
    HexTest::events.push_back(e);
    if (!HexTest::nativeLeavesError) SetLastError(0x51);
    return HexTest::nativeExtResult;
}
inline LONG HexNativeTabbedTextOutA(HDC dc, int x, int y, LPCSTR text, int count,
    int tabCount, const INT* tabs, int origin)
{
    HexTest::Event e{}; e.kind = HexTest::Kind::NativeTabbed; e.dc = dc; e.x = x; e.y = y;
    e.ansi = text; e.count = count; e.tabCount = tabCount; e.dxPointer = tabs; e.origin = origin;
    e.entryError = GetLastError(); if (dc) e.state = *dc;
    HexTest::events.push_back(e);
    if (!HexTest::nativeLeavesError) SetLastError(0x51);
    return HexTest::nativeTabbedResult;
}
inline BOOL WINAPI HexTrampolineExtTextOutA(HDC dc, int x, int y, UINT options, const RECT* rect,
    LPCSTR text, UINT count, const INT* dx)
{
    ++HexTest::trampolineCalls;
    assert(HexTest::extDetour); // exists only while attached
    return HexNativeExtTextOutA(dc, x, y, options, rect, text, count, dx);
}
inline LONG WINAPI HexTrampolineTabbedTextOutA(HDC dc, int x, int y, LPCSTR text, int count,
    int tabCount, const INT* tabs, int origin)
{
    ++HexTest::trampolineCalls;
    assert(HexTest::tabbedDetour);
    return HexNativeTabbedTextOutA(dc, x, y, text, count, tabCount, tabs, origin);
}
// What an application reaches through gdi32 / user32: the installed detour, else
// native. Calling it again from a detour would re-enter that detour.
inline BOOL WINAPI ExtTextOutA(HDC dc, int x, int y, UINT options, const RECT* rect,
    LPCSTR text, UINT count, const INT* dx)
{
    ++HexTest::exportCalls;
    using Fn = BOOL (WINAPI*)(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*);
    if (const auto hook = reinterpret_cast<Fn>(HexTest::extDetour)) return hook(dc, x, y, options, rect, text, count, dx);
    return HexNativeExtTextOutA(dc, x, y, options, rect, text, count, dx);
}
inline LONG WINAPI TabbedTextOutA(HDC dc, int x, int y, LPCSTR text, int count,
    int tabCount, const INT* tabs, int origin)
{
    ++HexTest::exportCalls;
    using Fn = LONG (WINAPI*)(HDC, int, int, LPCSTR, int, int, const INT*, int);
    if (const auto hook = reinterpret_cast<Fn>(HexTest::tabbedDetour)) return hook(dc, x, y, text, count, tabCount, tabs, origin);
    return HexNativeTabbedTextOutA(dc, x, y, text, count, tabCount, tabs, origin);
}

namespace GlassCore::Hooks::Detours
{
    // One transaction carries both functions. Commit applies all operations or,
    // on an injected failure, none: pointers and installed detours stay untouched.
    inline bool IsExt(void* p)
    { return p == reinterpret_cast<void*>(&::ExtTextOutA) || p == reinterpret_cast<void*>(&HexTrampolineExtTextOutA); }
    inline bool IsTabbed(void* p)
    { return p == reinterpret_cast<void*>(&::TabbedTextOutA) || p == reinterpret_cast<void*>(&HexTrampolineTabbedTextOutA); }
    inline void Attach(void** real, void* hook)
    {
        using namespace HexTest;
        if (++attachCalls == failAttachAt) throw std::runtime_error("DetourAttach failed");
        assert(real && hook && (*real == reinterpret_cast<void*>(&::ExtTextOutA)
            || *real == reinterpret_cast<void*>(&::TabbedTextOutA)));
        pending.push_back({real, hook, true});
    }
    inline void Detach(void** real, void* hook)
    {
        using namespace HexTest;
        if (++detachCalls == failDetachAt) throw std::runtime_error("DetourDetach failed");
        assert(real && hook && (*real == reinterpret_cast<void*>(&HexTrampolineExtTextOutA)
            || *real == reinterpret_cast<void*>(&HexTrampolineTabbedTextOutA)));
        pending.push_back({real, hook, false});
    }
    inline HRESULT Write(const std::function<void()>& callback) noexcept
    {
        using namespace HexTest;
        ++writes;
        if (failBegin) return E_FAIL;
        pending.clear();
        try { callback(); }
        catch (...) { pending.clear(); return E_FAIL; }
        const auto operations = std::exchange(pending, {});
        if (failCommit) return E_FAIL;
        for (const auto& op : operations)
        {
            const bool ext = IsExt(*op.real);
            assert(ext || IsTabbed(*op.real));
            void*& installed = ext ? extDetour : tabbedDetour;
            if (op.attach)
            {
                assert(!installed);
                installed = op.hook; ++attaches;
                *op.real = ext ? reinterpret_cast<void*>(&HexTrampolineExtTextOutA)
                    : reinterpret_cast<void*>(&HexTrampolineTabbedTextOutA);
            }
            else
            {
                assert(installed == op.hook);
                installed = nullptr; ++detaches;
                *op.real = ext ? reinterpret_cast<void*>(&::ExtTextOutA) : reinterpret_cast<void*>(&::TabbedTextOutA);
            }
        }
        return S_OK;
    }
}

inline HMODULE DetourGetContainingModule(void* address) { return static_cast<HMODULE>(address); }
inline HMODULE HexGetModuleHandleW(LPCWSTR name)
{
    if (std::wstring_view(name) == L"gdi32.dll") return HexTest::gdiAvailable ? &HexTest::gdi32 : nullptr;
    if (std::wstring_view(name) == L"user32.dll") return HexTest::userAvailable ? &HexTest::user32 : nullptr;
    return nullptr;
}
inline FARPROC HexGetProcAddress(HMODULE module, LPCSTR name)
{
    ++HexTest::resolutions;
    if (module == &HexTest::gdi32 && std::strcmp(name, "ExtTextOutA") == 0)
        return HexTest::extExport ? reinterpret_cast<FARPROC>(&::ExtTextOutA) : nullptr;
    if (module == &HexTest::user32 && std::strcmp(name, "TabbedTextOutA") == 0)
        return HexTest::tabbedExport ? reinterpret_cast<FARPROC>(&::TabbedTextOutA) : nullptr;
    return nullptr;
}
inline ULONGLONG HexTickCount64() { return HexTest::clock; }
template <typename T> inline void HexCaptureLogString(T value)
{
    if constexpr (std::is_convertible_v<T, LPCWSTR>)
        if (value) HexTest::logStrings.emplace_back(value);
}
template <typename... Args> inline void HexDebugLog(int, LPCWSTR format, Args... args)
{
    HexTest::logFormats.emplace_back(format);
    (HexCaptureLogString(args), ...);
    SetLastError(0xd10b); // a logger must not leak its error into the caller's result
}
namespace GlassCore::PaintDispatcher
{
    inline HWND HexGetStackOwnerForDc(HDC dc)
    {
        const auto it = HexTest::newestOwner.find(dc);
        return it == HexTest::newestOwner.end() ? GetStackOwnerForDc(dc) : it->second;
    }
    struct PaintProvider { void (*onBegin)(HWND, HMODULE); void (*onEnd)(HWND, HMODULE); };
    inline std::vector<PaintProvider> hexProviders;
    inline void RegisterPaintProvider(PaintProvider p) { hexProviders.push_back(p); }
}
namespace GlassCore::Text
{
    // Records the request; draws nothing. The production renderer is not compiled.
    inline HRESULT HexExtTextOutWithAlpha(HDC dc, int x, int y, UINT options, const RECT* rect,
        LPCWSTR text, UINT count, const INT* dx, decltype(&::ExtTextOutW) original)
    {
        SetLastError(0xd10c);
        assert(dc && text && count && original);
        HexTest::Event e{}; e.kind = HexTest::Kind::Glyphs; e.dc = dc; e.x = x; e.y = y; e.options = options;
        e.rectPointer = rect; if (rect) e.rect = *rect;
        e.text.assign(text, count); e.count = static_cast<int>(count);
        e.dxPointer = dx; if (dx) e.dx.assign(dx, dx + count);
        e.original = reinterpret_cast<void*>(original); e.state = *dc;
        HexTest::events.push_back(e);
        if (HexTest::duringRender) HexTest::duringRender();
        if (HexTest::throwRenderer) throw std::runtime_error("injected renderer failure");
        return HexTest::renderResult;
    }
}
#define GetModuleHandleW HexGetModuleHandleW
#define GetProcAddress HexGetProcAddress
#define GetTickCount64 HexTickCount64
#define DebugLog HexDebugLog
#define PatBlt HexPatBlt
#define FillRect HexFillRect
#define ExtTextOutWithAlpha HexExtTextOutWithAlpha
#define GetStackOwnerForDc HexGetStackOwnerForDc
#define _ReturnAddress() static_cast<void*>(HexTest::caller)
