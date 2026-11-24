// Environment doubles for the production GlassCore RestoratorCheckerboard unit.
// They model what the unit can see and do: the import table of the EXE with
// FillRect and CreateDIBSection in it, a dispatcher that counts references as
// the production one does, DCs that tell their kind and their selected bitmap,
// brushes that are objects (or a system colour index in a brush's place), the
// pattern brush made from a packed DIB, and what a fill leaves in a bitmap -
// byte by byte, with the tile laid from the bitmap's origin.
// There are no threads: a test may say what another one did just before the
// unit took its lock. This is not Windows GDI (no clip, no transform, no
// brush origin other than 0,0, no colour conversion), not a real PE import
// table, not the IAT registry's chain and not the VCL or a PNG decoder.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define WINAPI
using BOOL = int;
using UINT = unsigned int;
using LONG = std::int32_t;
using DWORD = std::uint32_t;
using WORD = std::uint16_t;
using BYTE = std::uint8_t;
using COLORREF = DWORD;
using ULONGLONG = std::uint64_t;
using ULONG_PTR = std::uintptr_t;
using LPCSTR = const char*;
using LPCWSTR = const wchar_t*;
using PVOID = void*;
using HANDLE = void*;
struct RECT { LONG left{}, top{}, right{}, bottom{}; };
struct TestWindow;
struct TestModule;
struct TestDC;
struct TestObject;
struct TestBrush;
struct TestBitmap;
using HWND = TestWindow*;
using HMODULE = TestModule*;
using HDC = TestDC*;
using HGDIOBJ = TestObject*;
using HBRUSH = TestBrush*;
using HBITMAP = TestBitmap*;

inline constexpr UINT BS_SOLID = 0, BS_NULL = 1, BS_HATCHED = 2, BS_PATTERN = 3, BS_DIBPATTERNPT = 6;
inline constexpr DWORD OBJ_DC = 3, OBJ_METADC = 4, OBJ_BITMAP = 7, OBJ_MEMDC = 10, OBJ_ENHMETADC = 12;
inline constexpr int COLOR_WINDOW = 5, COLOR_BTNFACE = 15, COLOR_MENUBAR = 30;
inline constexpr DWORD BI_RGB = 0;
inline constexpr UINT DIB_RGB_COLORS = 0, GA_ROOT = 2;
inline constexpr int DBG_NORMAL = 0, DBG_WARNING = 2;
constexpr COLORREF RGB(int r, int g, int b) { return COLORREF(r) | (COLORREF(g) << 8) | (COLORREF(b) << 16); }

struct LOGBRUSH { UINT lbStyle{}; COLORREF lbColor{}; ULONG_PTR lbHatch{}; };
struct BITMAP
{
    LONG bmType{}, bmWidth{}, bmHeight{}, bmWidthBytes{};
    WORD bmPlanes{}, bmBitsPixel{};
    void* bmBits{};
};
// The eleven fields and forty bytes of the real one: the tile is a packed DIB.
struct BITMAPINFOHEADER
{
    DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount;
    DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
};
static_assert(sizeof(BITMAPINFOHEADER) == 40);
struct RGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; };
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; };

struct TestWindow
{
    TestWindow* parent{};
    bool alive = true;
    int mark = 2;               // the root's backdrop mark: 2 allowed, 1 denied, 0 none
};
struct TestModule
{
    // One slot per imported function: the DLL it is imported from and where it leads.
    struct Import { std::string dll; void* target{}; };
    std::unordered_map<std::string, Import> imports;
    std::unordered_map<std::string, void*> hooks;   // what the slot was replaced with
    std::unordered_map<std::string, int> refs;
};
struct TestObject
{
    enum class Kind { Brush, Bitmap } kind;
    explicit TestObject(Kind k);
    virtual ~TestObject();
};
struct TestBrush : TestObject
{
    LOGBRUSH data{};
    std::vector<BYTE> packed;   // a pattern brush: the DIB it was made from
    TestBrush() : TestObject(Kind::Brush) {}
};
struct TestBitmap : TestObject
{
    int width, height; WORD bpp; bool section;
    std::vector<BYTE> pixels;   // B, G, R, A per pixel; the first row is the top one
    TestBitmap(int w, int h, WORD bits = 24, bool dib = true)
        : TestObject(Kind::Bitmap), width(w), height(h), bpp(bits), section(dib),
          pixels(static_cast<size_t>(w) * h * 4, 0x11) {}
};
struct TestDC
{
    DWORD type = OBJ_MEMDC;
    HBITMAP bitmap{};
};

namespace BoardTest
{
    inline thread_local DWORD error = 0;
    inline ULONGLONG clock = 1000;
    inline bool full = true;                        // the injection mode
    inline bool rendering = true;                   // the process policy: root glass is rendered
    inline std::unordered_map<int, COLORREF> colours;
    inline std::unordered_set<const TestObject*> live;
    inline int typeQueries = 0, colourQueries = 0, brushQueries = 0, bitmapQueries = 0, ancestors = 0;
    inline std::vector<HWND> glassQueries;          // every root asked about its backdrop, in order
    inline bool exclusive = false; inline int shared = 0, exclusiveLocks = 0;
    // Runs once, in place of another thread, just before the exclusive lock is taken.
    inline std::function<void()> beforeExclusive;

    // What CreateDIBPatternBrushPt was given, byte for byte.
    inline std::vector<BYTE> lastPattern;
    inline UINT lastUsage = 99;
    inline int patternBrushes = 0, brushDeletes = 0;
    inline bool failPattern = false;

    enum class Route { Import, Direct };            // the EXE's import chain, or GlassCore's own call
    struct Fill
    {
        Route route{}; HDC dc{}; const RECT* rectPointer{}; RECT rect{}; HBRUSH brush{};
        bool pattern{};                             // a live pattern brush at the call
        DWORD entryError{};
    };
    inline std::vector<Fill> fills;
    inline std::unordered_set<int> failFills;       // 1-based indices of the fills that return 0
    inline int fillResult = 1;
    inline std::function<void()> duringFill;
    struct Dib
    {
        Route route{}; HDC dc{}; const BITMAPINFO* info{}; UINT usage{}; void** bits{}; HANDLE section{}; DWORD offset{};
        DWORD entryError{};
    };
    inline std::vector<Dib> dibs;
    inline HBITMAP dibResult{};

    struct Log { int level{}; std::wstring format; std::vector<long long> numbers; std::vector<std::wstring> strings; };
    inline std::vector<Log> logs;

    // Dispatcher bookkeeping.
    inline std::unordered_map<HMODULE, std::unordered_set<std::string>> registrationFailures;
    inline int enables = 0, disables = 0, disableAlls = 0;
}

inline TestObject::TestObject(Kind k) : kind(k) { BoardTest::live.insert(this); }
inline TestObject::~TestObject() { BoardTest::live.erase(this); }

inline DWORD GetLastError() { return BoardTest::error; }
inline void SetLastError(DWORD value) { BoardTest::error = value; }
inline ULONGLONG GetTickCount64() { return BoardTest::clock; }
// Each query leaves an error of its own: the unit must not let it reach the caller.
inline DWORD GetObjectType(HDC dc) { ++BoardTest::typeQueries; SetLastError(0xb001); return dc ? dc->type : 0; }
inline COLORREF GetSysColor(int index)
{
    ++BoardTest::colourQueries; SetLastError(0xb002);
    const auto found = BoardTest::colours.find(index);
    return found == BoardTest::colours.end() ? 0 : found->second;
}
// Only a live object may be asked: a system colour index in a brush's place is not one.
inline int GetObjectW(HGDIOBJ object, int bytes, void* out)
{
    SetLastError(0xb003);
    assert(object && reinterpret_cast<ULONG_PTR>(object) > 64 && out);
    if (!BoardTest::live.count(object)) return 0;   // a handle that was deleted
    if (object->kind == TestObject::Kind::Brush)
    {
        ++BoardTest::brushQueries;
        if (bytes != static_cast<int>(sizeof(LOGBRUSH))) return 0;
        *static_cast<LOGBRUSH*>(out) = static_cast<TestBrush*>(object)->data;
        return bytes;
    }
    ++BoardTest::bitmapQueries;
    if (bytes != static_cast<int>(sizeof(BITMAP))) return 0;
    const auto* bitmap = static_cast<TestBitmap*>(object);
    BITMAP info{};
    info.bmWidth = bitmap->width; info.bmHeight = bitmap->height; info.bmPlanes = 1; info.bmBitsPixel = bitmap->bpp;
    info.bmWidthBytes = (bitmap->width * bitmap->bpp + 15) / 16 * 2;
    info.bmBits = bitmap->section ? const_cast<BYTE*>(bitmap->pixels.data()) : nullptr;
    *static_cast<BITMAP*>(out) = info;
    return bytes;
}
inline HGDIOBJ GetCurrentObject(HDC dc, DWORD type)
{
    SetLastError(0xb004);
    assert(dc && type == OBJ_BITMAP);
    return dc->bitmap;
}
inline HWND GetAncestor(HWND window, UINT kind)
{
    ++BoardTest::ancestors; SetLastError(0xb005);
    assert(kind == GA_ROOT);
    if (!window || !window->alive) return nullptr;
    while (window->parent) window = window->parent;
    return window;
}
inline HBRUSH CreateDIBPatternBrushPt(const void* packed, UINT usage)
{
    ++BoardTest::patternBrushes; BoardTest::lastUsage = usage;
    SetLastError(0xb006);
    if (BoardTest::failPattern) return nullptr;
    const auto* header = static_cast<const BITMAPINFOHEADER*>(packed);
    assert(header->biSize == sizeof(BITMAPINFOHEADER) && header->biWidth > 0 && header->biHeight != 0);
    assert(header->biPlanes == 1 && header->biBitCount == 24 && header->biCompression == BI_RGB && !header->biClrUsed);
    const size_t stride = (static_cast<size_t>(header->biWidth) * header->biBitCount + 31) / 32 * 4;
    const size_t bytes = header->biSize + stride * static_cast<size_t>(std::abs(header->biHeight));
    auto* brush = new TestBrush;
    brush->data = LOGBRUSH{BS_DIBPATTERNPT, usage, 0};
    const auto* begin = static_cast<const BYTE*>(packed);
    brush->packed.assign(begin, begin + bytes);
    BoardTest::lastPattern = brush->packed;
    return brush;
}
// The unit deletes only what it made: a pattern brush, alive, once.
inline BOOL DeleteObject(HGDIOBJ object)
{
    assert(object && BoardTest::live.count(object) && object->kind == TestObject::Kind::Brush);
    assert(!static_cast<TestBrush*>(object)->packed.empty());
    delete object; ++BoardTest::brushDeletes;
    SetLastError(0xb007);
    return 1;
}

namespace BoardTest
{
    // The pixel of a packed 24bpp DIB that a pattern brush shows at (x, y),
    // with the tile laid from 0,0. A positive height is bottom-up: the first
    // row in memory is the lowest one.
    inline const BYTE* TilePixel(const std::vector<BYTE>& packed, int x, int y)
    {
        const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(packed.data());
        const int width = header->biWidth, height = std::abs(header->biHeight);
        const size_t stride = (static_cast<size_t>(width) * 24 + 31) / 32 * 4;
        const int row = header->biHeight > 0 ? height - 1 - y % height : y % height;
        return packed.data() + header->biSize + stride * row + static_cast<size_t>(x % width) * 3;
    }
    // A fill as GDI leaves it: the colour, and nothing in the fourth byte.
    // Only solid and DIB pattern brushes are modelled; the others draw nothing.
    inline void Paint(HBITMAP bitmap, const RECT& rect, HBRUSH brush)
    {
        const auto value = reinterpret_cast<ULONG_PTR>(brush);
        const bool index = value <= COLOR_MENUBAR + 1u;
        if (!index && !live.count(brush)) return;
        if (!index && brush->data.lbStyle != BS_SOLID && brush->data.lbStyle != BS_DIBPATTERNPT) return;
        const bool pattern = !index && brush->data.lbStyle == BS_DIBPATTERNPT;
        const COLORREF solid = index ? colours[static_cast<int>(value) - 1] : brush->data.lbColor;
        for (LONG y = (std::max)(rect.top, LONG(0)); y < (std::min)(rect.bottom, LONG(bitmap->height)); ++y)
            for (LONG x = (std::max)(rect.left, LONG(0)); x < (std::min)(rect.right, LONG(bitmap->width)); ++x)
            {
                BYTE* out = bitmap->pixels.data() + (static_cast<size_t>(y) * bitmap->width + x) * 4;
                if (pattern)
                {
                    const BYTE* in = TilePixel(brush->packed, x, y);
                    out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
                }
                else { out[0] = BYTE(solid >> 16); out[1] = BYTE(solid >> 8); out[2] = BYTE(solid); }
                out[3] = 0;
            }
    }
    inline int DoFill(Route route, HDC dc, const RECT* rect, HBRUSH brush)
    {
        // No call leaves the unit under its lock.
        assert(!exclusive && !shared);
        // A pattern brush counts only while it is alive: one deleted too early is no pattern.
        const bool object = reinterpret_cast<ULONG_PTR>(brush) > COLOR_MENUBAR + 1u && live.count(brush);
        fills.push_back({route, dc, rect, rect ? *rect : RECT{}, brush,
            object && brush->data.lbStyle == BS_DIBPATTERNPT, GetLastError()});
        const size_t index = fills.size();
        if (const auto inside = std::exchange(duringFill, {})) inside();
        SetLastError(0xc000 + static_cast<DWORD>(index));
        if (failFills.count(static_cast<int>(index))) return 0;
        if (dc && dc->bitmap && rect) Paint(dc->bitmap, *rect, brush);
        return fillResult;
    }
    inline HBITMAP DoDib(Route route, HDC dc, const BITMAPINFO* info, UINT usage, void** bits, HANDLE section, DWORD offset)
    {
        assert(!exclusive && !shared);
        dibs.push_back({route, dc, info, usage, bits, section, offset, GetLastError()});
        SetLastError(0xd000 + static_cast<DWORD>(dibs.size()));
        return dibResult;
    }
}
// Where the EXE's two imports lead: user32 and gdi32.
inline int WINAPI ImportedFillRect(HDC dc, const RECT* rect, HBRUSH brush)
{ return BoardTest::DoFill(BoardTest::Route::Import, dc, rect, brush); }
inline HBITMAP WINAPI ImportedCreateDIBSection(HDC dc, const BITMAPINFO* info, UINT usage, void** bits, HANDLE section, DWORD offset)
{ return BoardTest::DoDib(BoardTest::Route::Import, dc, info, usage, bits, section, offset); }
// GlassCore's own imports of the same two: what a detour without an original calls.
inline int WINAPI FillRect(HDC dc, const RECT* rect, HBRUSH brush)
{ return BoardTest::DoFill(BoardTest::Route::Direct, dc, rect, brush); }
inline HBITMAP WINAPI CreateDIBSection(HDC dc, const BITMAPINFO* info, UINT usage, void** bits, HANDLE section, DWORD offset)
{ return BoardTest::DoDib(BoardTest::Route::Direct, dc, info, usage, bits, section, offset); }

// Every line is recorded, in either build; the logger leaves an error of its own.
template <typename... Args> inline void DebugLog(int level, const wchar_t* format, Args... args)
{
    assert(!BoardTest::exclusive && !BoardTest::shared);   // never under the unit's lock
    BoardTest::Log log{level, format, {}, {}};
    const auto capture = [&](auto value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_convertible_v<T, LPCWSTR>) log.strings.emplace_back(value ? value : L"(null)");
        else if constexpr (std::is_pointer_v<T>) log.numbers.push_back(static_cast<long long>(reinterpret_cast<std::intptr_t>(value)));
        else log.numbers.push_back(static_cast<long long>(value));
    };
    (capture(args), ...);
    BoardTest::logs.push_back(std::move(log));
    SetLastError(0xb008);
}

namespace wil
{
    struct srwlock
    {
        struct Exclusive { ~Exclusive() { BoardTest::exclusive = false; } };
        struct Shared { ~Shared() { --BoardTest::shared; } };
        // Whatever runs between a hook's entry and its call may leave an error: so do these.
        Exclusive lock_exclusive()
        {
            if (const auto first = std::exchange(BoardTest::beforeExclusive, {})) first();
            assert(!BoardTest::exclusive && !BoardTest::shared);   // not recursive
            BoardTest::exclusive = true; ++BoardTest::exclusiveLocks;
            SetLastError(0xb00a);
            return {};
        }
        Shared lock_shared() { assert(!BoardTest::exclusive); ++BoardTest::shared; SetLastError(0xb00b); return {}; }
    };
}
namespace GlassCore::ModuleHandler { inline HMODULE g_entryPointModule{}; }
namespace GlassCore::Root { inline bool IsFullInjectionMode() { return BoardTest::full; } }
namespace GlassShared
{
    // As GlassBackdropMark.h has it: the mark of a living root, else the policy
    // of the process - a window that is gone has no mark.
    inline bool GlassIsRootBackdropAllowed(HWND root) noexcept
    {
        BoardTest::glassQueries.push_back(root);
        SetLastError(0xb009);
        if (!root) return false;
        const int mark = root->alive ? root->mark : 0;
        if (mark == 1) return false;
        return BoardTest::rendering;
    }
}
namespace GlassCore::Hooks
{
    template <size_t Count, size_t Imports> using HookDispatcherDependency = std::array<
        std::tuple<std::array<std::string_view, Imports>, LPCSTR, void*>, Count>;
    // One import slot per name in the module, found only under a DLL name the
    // dependency lists; refs as the production dispatcher counts them.
    // EnableHook reports only a change of the slot, as it does there, and a
    // slot that cannot be registered takes no reference.
    template <size_t Count, size_t Imports> struct HookDispatcher
    {
        const HookDispatcherDependency<Count, Imports>& dependency;
        void* moduleAddress{};
        std::array<int, Count> refs{};
        HookDispatcher(const HookDispatcherDependency<Count, Imports>& dep, void* base)
            : dependency(dep), moduleAddress(base) {}
        TestModule::Import* Slot(size_t i)
        {
            auto* mod = static_cast<HMODULE>(moduleAddress);
            if (!mod) return nullptr;
            const auto found = mod->imports.find(std::get<1>(dependency[i]));
            if (found == mod->imports.end()) return nullptr;
            for (const auto dll : std::get<0>(dependency[i]))
                if (!dll.empty() && dll == found->second.dll) return &found->second;
            return nullptr;
        }
        int GetRef(size_t i) { return refs[i]; }
        bool EnableHook(size_t i, bool enable)
        {
            assert(BoardTest::exclusive);            // the unit serialises its dispatcher
            ++(enable ? BoardTest::enables : BoardTest::disables);
            auto* mod = static_cast<HMODULE>(moduleAddress);
            const auto name = std::get<1>(dependency[i]);
            if (!Slot(i)) return false;
            if (enable)
            {
                if (!refs[i] && BoardTest::registrationFailures[mod].count(name)) return false;
                ++refs[i]; ++mod->refs[name];
                mod->hooks[name] = std::get<2>(dependency[i]);
                return refs[i] == 1;
            }
            if (!refs[i]) return false;
            --refs[i]; --mod->refs[name];
            if (!refs[i]) mod->hooks.erase(name);
            return !refs[i];
        }
        template <size_t I, typename Fn> Fn GetOrg()
        {
            const auto* slot = refs[I] ? Slot(I) : nullptr;
            return slot ? reinterpret_cast<Fn>(slot->target) : nullptr;
        }
        void DisableAllHooks()
        {
            assert(BoardTest::exclusive);
            ++BoardTest::disableAlls;
            if (!moduleAddress) return;
            auto* mod = static_cast<HMODULE>(moduleAddress);
            for (size_t i = 0; i < Count; ++i)
            {
                if (!refs[i]) continue;
                const auto name = std::get<1>(dependency[i]);
                mod->refs[name] -= refs[i]; refs[i] = 0;
                mod->hooks.erase(name);
            }
        }
    };
}
