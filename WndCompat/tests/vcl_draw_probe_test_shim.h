#pragma once
// Deterministic Win32/dispatcher doubles for the extracted production probe.
// No GDI rasterization, real PE imports, IATRegistry or Windows ABI is emulated.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#define WINAPI
#define _countof(value) (sizeof(value) / sizeof((value)[0]))
#define _wcsicmp wcscasecmp
using BOOL = int;
using UINT = unsigned;
using DWORD = std::uint32_t;
using LONG = std::int32_t;
using HRESULT = std::int32_t;
using COLORREF = DWORD;
using ULONGLONG = std::uint64_t;
using WCHAR = wchar_t;
using LPCSTR = const char*;
using LPSTR = char*;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using INT = int;
using PVOID = void*;
using HMODULE = void*;
using HGDIOBJ = void*;
using HBRUSH = void*;
using HTHEME = void*;
constexpr BOOL TRUE = 1, FALSE = 0;
constexpr int OBJ_DC = 3, OBJ_MEMDC = 10, OBJ_BRUSH = 2, OBJ_PEN = 1, OBJ_BITMAP = 7;
constexpr int DBG_NORMAL = 1;
struct RECT { LONG left{}, top{}, right{}, bottom{}; bool operator==(const RECT&) const = default; };
using LPRECT = RECT*;
using LPCRECT = const RECT*;
struct POINT { LONG x{}, y{}; };
struct LOGBRUSH { UINT lbStyle{}; COLORREF lbColor{}; std::uintptr_t lbHatch{}; };
struct LOGPEN { UINT lopnStyle{}; POINT lopnWidth{}; COLORREF lopnColor{}; };
struct BITMAP { LONG bmWidth{}, bmHeight{}; unsigned short bmBitsPixel{}; };
struct DRAWTEXTPARAMS { UINT cbSize{}, iTabLength{}; int iLeftMargin{}, iRightMargin{}; UINT uiLengthDrawn{}; };
using LPDRAWTEXTPARAMS = DRAWTEXTPARAMS*;
struct DTTOPTS { DWORD dwSize{}, dwFlags{}; };
struct Window { std::wstring cls = L"TPage"; bool valid = true; };
using HWND = Window*;
struct DC {
    HWND owner{};
    int type = OBJ_DC, mode = 2;
    COLORREF text = 0x102030, background = 0xa0b0c0;
    RECT clip{1, 2, 119, 43};
    std::array<DWORD, 4> pixels{0x12345678, 0x87654321, 0xff000000, 0x00ffffff};
};
using HDC = DC*;
namespace Mock {
    inline thread_local DWORD error = 0;
    inline thread_local bool originalRoute = false;
    inline thread_local ULONGLONG clock = 100;
    inline thread_local std::vector<std::wstring> logs;
    struct Call {
        std::string api;
        HDC dc{};
        const void* text{};
        long long count{};
        const RECT* rect{};
        UINT flags{};
        int x{}, y{};
        const INT* dx{};
        LPDRAWTEXTPARAMS params{};
        int tabs{}, origin{};
        const INT* tabStops{};
        DWORD entry{};
        bool original{};
    };
    inline thread_local std::vector<Call> calls;
    inline DWORD nativeError = 67;
    inline int intResult = 47;
    inline BOOL boolResult = 7;
    inline LONG longResult = 0x7f00ee11;
    inline HRESULT hrResult = static_cast<HRESULT>(0x80004005u);
    inline bool leaveError = false, mutateOutput = false, failClass = false;
    inline LOGBRUSH brush{0, 0x102030, 0};
    inline LOGPEN pen{0, {1, 0}, 0x334455};
    inline BITMAP bitmap{120, 45, 32};
    inline int handle;
    inline std::set<std::string> available, registerFails, noOriginal;
    inline std::map<std::string, std::vector<void*>> occupants;
    struct Toggle { size_t index; bool enable; int before, after; };
    inline std::vector<Toggle> toggles;
    inline std::set<HDC> dcs;
    inline thread_local std::function<void()> duringNative;
    void* NativeAddress(std::string_view name);
}
inline DWORD GetLastError() { return Mock::error; }
inline void SetLastError(DWORD value) { Mock::error = value; }
namespace wil {
    template<class F> struct scope_exit {
        F fn;
        ~scope_exit() { fn(); }
    };
    template<class F> scope_exit(F) -> scope_exit<F>;
    struct srwlock {
        mutable std::shared_mutex mutex;
        auto lock_shared() const { SetLastError(101); return std::shared_lock(mutex); }
        auto lock_exclusive() { SetLastError(102); return std::unique_lock(mutex); }
    };
}
inline int GetClassNameW(HWND window, WCHAR* out, int size) {
    SetLastError(103);
    if (!window || !window->valid || Mock::failClass) return 0;
    const auto count = std::min(window->cls.size(), static_cast<size_t>(size - 1));
    std::copy_n(window->cls.c_str(), count, out); out[count] = 0;
    return static_cast<int>(count);
}
inline ULONGLONG GetTickCount64() { SetLastError(104); return Mock::clock; }
inline HWND WindowFromDC(HDC dc) { SetLastError(105); return dc ? dc->owner : nullptr; }
inline int GetClipBox(HDC dc, RECT* out) { SetLastError(106); *out = dc->clip; return 3; }
inline COLORREF GetTextColor(HDC dc) { SetLastError(107); return dc->text; }
inline COLORREF GetBkColor(HDC dc) { SetLastError(108); return dc->background; }
inline int GetBkMode(HDC dc) { SetLastError(109); return dc->mode; }
inline DWORD GetObjectType(HGDIOBJ object) {
    SetLastError(110);
    if (object == &Mock::brush) return OBJ_BRUSH;
    if (object == &Mock::pen) return OBJ_PEN;
    if (object == &Mock::bitmap) return OBJ_BITMAP;
    const auto dc = static_cast<HDC>(object);
    return Mock::dcs.count(dc) ? dc->type : 0;
}
inline HGDIOBJ GetCurrentObject(HDC, int type) {
    SetLastError(111);
    if (type == OBJ_BRUSH) return &Mock::brush;
    if (type == OBJ_PEN) return &Mock::pen;
    if (type == OBJ_BITMAP) return &Mock::bitmap;
    return nullptr;
}
inline int GetObjectW(HGDIOBJ object, int size, void* out) {
    SetLastError(112);
    if (object == &Mock::brush && size == sizeof(LOGBRUSH)) *static_cast<LOGBRUSH*>(out) = Mock::brush;
    else if (object == &Mock::pen && size == sizeof(LOGPEN)) *static_cast<LOGPEN*>(out) = Mock::pen;
    else if (object == &Mock::bitmap && size == sizeof(BITMAP)) *static_cast<BITMAP*>(out) = Mock::bitmap;
    else return 0;
    return size;
}
inline void DebugLog(int, LPCWSTR format, ...) {
    // Windows wchar printf uses %s for wide strings and 32-bit LONG for %ld.
    std::wstring portable = format;
    for (const auto& [from, to] : {std::pair{L"%s", L"%ls"}, {L"%ld", L"%d"}}) {
        size_t pos = 0;
        while ((pos = portable.find(from, pos)) != std::wstring::npos) {
            portable.replace(pos, wcslen(from), to); pos += wcslen(to);
        }
    }
    wchar_t buffer[4096]{};
    va_list args; va_start(args, format);
    const int length = std::vswprintf(buffer, std::size(buffer), portable.c_str(), args);
    va_end(args); assert(length >= 0);
    Mock::logs.emplace_back(buffer); SetLastError(113);
}
inline void Record(Mock::Call call) {
    call.entry = GetLastError(); call.original = Mock::originalRoute;
    Mock::calls.push_back(call);
    if (Mock::duringNative) Mock::duringNative();
    if (!Mock::leaveError) SetLastError(Mock::nativeError);
}
inline int DrawTextA(HDC dc, LPCSTR text, int count, LPRECT r, UINT flags) {
    Record({"DrawTextA", dc, text, count, r, flags}); return Mock::intResult;
}
inline int DrawTextW(HDC dc, LPCWSTR text, int count, LPRECT r, UINT flags) {
    Record({"DrawTextW", dc, text, count, r, flags}); return Mock::intResult;
}
template<class Char> int DrawTextEx(std::string api, HDC dc, Char* text, int count,
    LPRECT r, UINT flags, LPDRAWTEXTPARAMS params) {
    Mock::Call call{std::move(api), dc, text, count, r, flags}; call.params = params; Record(call);
    if (Mock::mutateOutput) {
        if (r) *r = {11, 12, 211, 35};
        if (params) params->uiLengthDrawn = 29;
        if (text) text[0] = static_cast<Char>('!');
    }
    return Mock::intResult;
}
inline int DrawTextExA(HDC dc, LPSTR text, int count, LPRECT r, UINT flags, LPDRAWTEXTPARAMS params) {
    return DrawTextEx("DrawTextExA", dc, text, count, r, flags, params);
}
inline int DrawTextExW(HDC dc, LPWSTR text, int count, LPRECT r, UINT flags, LPDRAWTEXTPARAMS params) {
    return DrawTextEx("DrawTextExW", dc, text, count, r, flags, params);
}
template<class Char> BOOL ExtTextOut(std::string api, HDC dc, int x, int y, UINT flags,
    const RECT* r, const Char* text, UINT count, const INT* dx) {
    Mock::Call call{std::move(api), dc, text, count, r, flags};
    call.x = x; call.y = y; call.dx = dx; Record(call); return Mock::boolResult;
}
inline BOOL ExtTextOutA(HDC dc, int x, int y, UINT flags, const RECT* r, LPCSTR text, UINT count, const INT* dx) {
    return ExtTextOut("ExtTextOutA", dc, x, y, flags, r, text, count, dx);
}
inline BOOL ExtTextOutW(HDC dc, int x, int y, UINT flags, const RECT* r, LPCWSTR text, UINT count, const INT* dx) {
    return ExtTextOut("ExtTextOutW", dc, x, y, flags, r, text, count, dx);
}
template<class Char> BOOL TextOut(std::string api, HDC dc, int x, int y, const Char* text, int count) {
    Mock::Call call{std::move(api), dc, text, count}; call.x = x; call.y = y;
    Record(call); return Mock::boolResult;
}
inline BOOL TextOutA(HDC dc, int x, int y, LPCSTR text, int count) { return TextOut("TextOutA", dc, x, y, text, count); }
inline BOOL TextOutW(HDC dc, int x, int y, LPCWSTR text, int count) { return TextOut("TextOutW", dc, x, y, text, count); }
template<class Char> LONG TabbedTextOut(std::string api, HDC dc, int x, int y,
    const Char* text, int count, int tabs, const INT* stops, int origin) {
    Mock::Call call{std::move(api), dc, text, count}; call.x = x; call.y = y;
    call.tabs = tabs; call.tabStops = stops; call.origin = origin; Record(call); return Mock::longResult;
}
inline LONG TabbedTextOutA(HDC dc, int x, int y, LPCSTR text, int count, int tabs, const INT* stops, int origin) {
    return TabbedTextOut("TabbedTextOutA", dc, x, y, text, count, tabs, stops, origin);
}
inline LONG TabbedTextOutW(HDC dc, int x, int y, LPCWSTR text, int count, int tabs, const INT* stops, int origin) {
    return TabbedTextOut("TabbedTextOutW", dc, x, y, text, count, tabs, stops, origin);
}
inline int FillRect(HDC dc, const RECT* r, HBRUSH brush) {
    Record({"FillRect", dc, brush, 0, r}); return Mock::intResult;
}
inline BOOL PatBlt(HDC dc, int, int, int, int, DWORD flags) { Record({"PatBlt", dc, nullptr, 0, nullptr, flags}); return Mock::boolResult; }
inline BOOL Rectangle(HDC dc, int, int, int, int) { Record({"Rectangle", dc}); return Mock::boolResult; }
inline BOOL RoundRect(HDC dc, int, int, int, int, int, int) { Record({"RoundRect", dc}); return Mock::boolResult; }
inline BOOL Ellipse(HDC dc, int, int, int, int) { Record({"Ellipse", dc}); return Mock::boolResult; }
inline BOOL BitBlt(HDC dc, int, int, int, int, HDC, int, int, DWORD) { Record({"BitBlt", dc}); return Mock::boolResult; }
inline BOOL StretchBlt(HDC dc, int, int, int, int, HDC, int, int, int, int, DWORD) { Record({"StretchBlt", dc}); return Mock::boolResult; }
inline BOOL DrawFocusRect(HDC dc, const RECT* r) { Record({"DrawFocusRect", dc, nullptr, 0, r}); return Mock::boolResult; }
inline HRESULT DrawThemeBackground(HTHEME, HDC dc, int, int, LPCRECT r, LPCRECT) {
    Record({"DrawThemeBackground", dc, nullptr, 0, r}); return Mock::hrResult;
}
inline HRESULT DrawThemeTextEx(HTHEME, HDC dc, int, int, LPCWSTR text, int count, DWORD flags, LPRECT r, const DTTOPTS*) {
    Record({"DrawThemeTextEx", dc, text, count, r, flags}); return Mock::hrResult;
}
inline HBRUSH CreateSolidBrush(COLORREF) { Record({"CreateSolidBrush"}); return &Mock::brush; }
inline HBRUSH CreateBrushIndirect(const LOGBRUSH*) { Record({"CreateBrushIndirect"}); return &Mock::brush; }
inline HGDIOBJ SelectObject(HDC dc, HGDIOBJ) { Record({"SelectObject", dc}); return &Mock::handle; }
namespace Mock {
    template<auto Fn> struct OriginalThunk;
    template<class R, class... Args, R(*Fn)(Args...)> struct OriginalThunk<Fn> {
        static R Invoke(Args... args) {
            const bool saved = originalRoute; originalRoute = true;
            const auto restore = wil::scope_exit([saved]() { originalRoute = saved; });
            return Fn(args...);
        }
    };
    inline void* NativeAddress(std::string_view name) {
#define ORIGINAL(api) if (name == #api) return reinterpret_cast<void*>(&OriginalThunk<&::api>::Invoke)
        ORIGINAL(DrawTextA); ORIGINAL(DrawTextW); ORIGINAL(DrawTextExA); ORIGINAL(DrawTextExW);
        ORIGINAL(ExtTextOutA); ORIGINAL(ExtTextOutW); ORIGINAL(TextOutA); ORIGINAL(TextOutW);
        ORIGINAL(TabbedTextOutA); ORIGINAL(TabbedTextOutW); ORIGINAL(FillRect); ORIGINAL(PatBlt);
        ORIGINAL(Rectangle); ORIGINAL(RoundRect); ORIGINAL(Ellipse); ORIGINAL(BitBlt); ORIGINAL(StretchBlt);
        ORIGINAL(DrawFocusRect); ORIGINAL(DrawThemeBackground); ORIGINAL(DrawThemeTextEx);
        ORIGINAL(CreateSolidBrush); ORIGINAL(CreateBrushIndirect); ORIGINAL(SelectObject);
#undef ORIGINAL
        return nullptr;
    }
}
namespace GlassCore::ModuleHandler { inline HMODULE g_entryPointModule = &Mock::handle; }
namespace GlassCore::Hooks {
    template<size_t N, size_t M> using HookDispatcherDependency =
        std::array<std::tuple<std::array<std::string_view, M>, LPCSTR, void*>, N>;
    template<size_t N, size_t M> struct HookDispatcher {
        const HookDispatcherDependency<N, M>& dependency;
        void* module;
        std::array<int, N> refs{};
        HookDispatcher(const HookDispatcherDependency<N, M>& dep, void* base) : dependency(dep), module(base) {}
        void CacheHookData() { SetLastError(114); }
        int GetRef(size_t index) const { SetLastError(115); return refs.at(index); }
        bool EnableHook(size_t index, bool enable) {
            SetLastError(116);
            const std::string name = std::get<1>(dependency.at(index));
            const int before = refs.at(index);
            const auto trace = wil::scope_exit([this, index, enable, before]() {
                Mock::toggles.push_back({index, enable, before, refs.at(index)});
            });
            if (!Mock::available.count(name) || (enable && Mock::registerFails.count(name))) return false;
            void* detour = std::get<2>(dependency.at(index));
            auto& entries = Mock::occupants[name];
            if (enable) {
                if (refs[index]++ != 0) return false;
                entries.push_back(detour);
            } else {
                if (!refs[index]) return false;
                if (--refs[index]) return false;
                const auto found = std::find(entries.begin(), entries.end(), detour);
                assert(found != entries.end()); entries.erase(found);
            }
            return true;
        }
        template<size_t Index, class Fn> Fn GetOrg() const {
            SetLastError(117);
            const std::string name = std::get<1>(dependency.at(Index));
            return refs[Index] && !Mock::noOriginal.count(name)
                ? reinterpret_cast<Fn>(Mock::NativeAddress(name)) : nullptr;
        }
    };
}
