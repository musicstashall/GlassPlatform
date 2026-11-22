// Environment only: compile the complete actual DebugDetourHooks.cpp and header.
// Models transactional pointer changes/export dispatch, not executable Windows
// Detours patches, Windows stack unwinding, SDK ABI, concurrency or GDI pixels.
#pragma once
#include "coolbar_test_shim.h"
#include <cstdarg>
#include <limits>
#include <stdexcept>

#define CONST const
using VOID = void;
using BYTE = std::uint8_t;
using WORD = std::uint16_t;
using USHORT = std::uint16_t;
using WCHAR = wchar_t;
using LPSTR = char*;
using LPCVOID = const void*;
using SIZE_T = std::size_t;
using ULONG_PTR = std::uintptr_t;
using LONG_PTR = std::intptr_t;
using HBITMAP = void*;
using FARPROC = void(*)();
inline constexpr int MAX_PATH = 260, DBG_IMPORTANT = 3;
inline constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4;
inline constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 2;
inline constexpr DWORD ERROR_PROC_NOT_FOUND = 127;
inline constexpr HRESULT E_POINTER = -2, E_INVALIDARG = -3;
inline constexpr COLORREF CLR_INVALID = 0xffffffffu;
inline bool FAILED(HRESULT hr) { return hr < 0; }
inline HRESULT HRESULT_FROM_WIN32(DWORD error) { return error ? -HRESULT(error) : S_OK; }
#define MAKEINTRESOURCEA(value) reinterpret_cast<LPCSTR>(std::uintptr_t(value))
#define THROW_HR(hr) throw std::runtime_error(std::to_string(hr))
#define THROW_HR_IF(hr, condition) do { if (condition) { THROW_HR(hr); } } while (false)
#define THROW_IF_FAILED(hr) do { if (FAILED(hr)) { THROW_HR(hr); } } while (false)
namespace wil { inline HRESULT ResultFromCaughtException() { return E_FAIL; } }

enum
{
    GCLP_MENUNAME = -8, GCLP_HBRBACKGROUND = -10, GCLP_HMODULE = -16,
    GCL_CBWNDEXTRA = -18, GCL_CBCLSEXTRA = -20, GCLP_WNDPROC = -24,
    GCL_STYLE = -26, GCLP_HICON = -14, GCLP_HICONSM = -34,
    WHITE_BRUSH = 0, LTGRAY_BRUSH = 1, GRAY_BRUSH = 2, DKGRAY_BRUSH = 3,
    BLACK_BRUSH = 4, NULL_BRUSH = 5, WHITE_PEN = 6, BLACK_PEN = 7, NULL_PEN = 8,
    OEM_FIXED_FONT = 10, ANSI_FIXED_FONT = 11, ANSI_VAR_FONT = 12, SYSTEM_FONT = 13,
    DEVICE_DEFAULT_FONT = 14, DEFAULT_PALETTE = 15, SYSTEM_FIXED_FONT = 16,
    DEFAULT_GUI_FONT = 17, DC_BRUSH = 18, DC_PEN = 19,
    COLOR_SCROLLBAR = 0, COLOR_BACKGROUND = 1, COLOR_ACTIVECAPTION = 2,
    COLOR_INACTIVECAPTION = 3, COLOR_MENU = 4, COLOR_WINDOWFRAME = 6,
    COLOR_MENUTEXT = 7, COLOR_WINDOWTEXT = 8, COLOR_CAPTIONTEXT = 9,
    COLOR_ACTIVEBORDER = 10, COLOR_INACTIVEBORDER = 11, COLOR_APPWORKSPACE = 12,
    COLOR_HIGHLIGHT = 13, COLOR_HIGHLIGHTTEXT = 14, COLOR_BTNSHADOW = 16,
    COLOR_GRAYTEXT = 17, COLOR_BTNTEXT = 18, COLOR_INACTIVECAPTIONTEXT = 19,
    COLOR_BTNHIGHLIGHT = 20,
    DWMSBT_AUTO = 0, DWMSBT_NONE = 1, DWMSBT_MAINWINDOW = 2,
    DWMSBT_TRANSIENTWINDOW = 3, DWMSBT_TABBEDWINDOW = 4,
};
inline constexpr DWORD DTBG_CLIPRECT = 1, DTBG_DRAWSOLID = 2, DTBG_OMITBORDER = 4;
inline constexpr DWORD DTBG_OMITCONTENT = 8, DTBG_COMPUTINGREGION = 16;
inline constexpr DWORD DTBG_MIRRORDC = 32, DTBG_NOMIRROR = 64, DTBG_VALIDBITS = 127;
inline constexpr DWORD DWM_BB_ENABLE = 1, DWM_BB_BLURREGION = 2, DWM_BB_TRANSITIONONMAXIMIZED = 4;
struct DTBGOPTS { DWORD dwSize{}, dwFlags{}; RECT rcClip{}; };
struct DTTOPTS { DWORD dwSize{}, dwFlags{}; };
struct MARGINS { int cxLeftWidth{}, cxRightWidth{}, cyTopHeight{}, cyBottomHeight{}; };
struct DWM_BLURBEHIND { DWORD dwFlags{}; BOOL fEnable{}; void* hRgnBlur{}; BOOL fTransitionOnMaximized{}; };
struct BITMAPINFOHEADER { LONG biWidth{}, biHeight{}; WORD biBitCount{}; };
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader{}; };
using HPAINTBUFFER = void*;
using BP_BUFFERFORMAT = int;
struct BP_PAINTPARAMS { DWORD cbSize{}, dwFlags{}; const void* pBlendFunction{}; };
using DTT_CALLBACK_PROC = int(WINAPI*)(HDC, LPWSTR, int, LPRECT, UINT, LPARAM);

namespace DdhTest
{
    inline TestModule core, app, system;
    inline const void* externalAddress = reinterpret_cast<const void*>(std::uintptr_t(0x1000));
    inline const void* coreAddress = reinterpret_cast<const void*>(std::uintptr_t(0x2000));
    inline const void* returnAddress = externalAddress;
    inline bool unknownCaller = false, unknownStack = false;
    inline thread_local DWORD error{};
    inline std::vector<std::wstring> logs;
    inline std::function<void()> duringLog;
    inline bool throwLog = false;
    inline int loggerCalls = 0;
    inline bool mutateNative = false, nativeLeavesError = false;
    inline DWORD nativeError = 0x5151;
    struct Call
    {
        HDC dc{}; const void* text{}; std::int64_t count{}; UINT flags{};
        const RECT* rect{}; const void* extra{}; int x{}, y{}, tabCount{}, tabOrigin{};
        DWORD entryError{};
    };
    inline std::array<Call, 6> calls{};
    inline std::array<int, 6> nativeCalls{};
    inline std::array<LONG, 6> results{23, -29, TRUE, FALSE, LONG(0xfedcba98u), LONG(0x76543210u)};
    inline std::array<void*, 6> hooks{};
    struct Op { void** real{}; void* hook{}; bool enable{}; };
    inline std::vector<Op> pending, committed;
    inline std::unordered_map<void**, void*> attached;
    inline bool failBegin = false, failCommit = false;
    inline int failOperation = -1, operations = 0, writes = 0, commits = 0, aborts = 0;
}
inline DWORD GetLastError() { return DdhTest::error; }
inline void SetLastError(DWORD error) { DdhTest::error = error; }
#define _ReturnAddress() const_cast<void*>(DdhTest::returnAddress)

inline std::wstring DdhFormat(LPCWSTR format)
{
    // MSVC's wide printf %s consumes wchar_t*, unlike libc's %s.
    std::wstring out;
    for (size_t i = 0; format[i]; ++i)
    {
        if (format[i] == L'%' && format[i+1] == L's') { out += L"%ls"; ++i; }
        else out.push_back(format[i]);
    }
    return out;
}
inline int swprintf_s(wchar_t* out, size_t capacity, LPCWSTR format, ...)
{
    va_list args; va_start(args, format);
    const int result = std::vswprintf(out, capacity, DdhFormat(format).c_str(), args);
    va_end(args); return result;
}
template <size_t N, typename... Args>
inline int swprintf_s(wchar_t (&out)[N], LPCWSTR format, Args... args)
{ return swprintf_s(out, N, format, args...); }
inline int wcscpy_s(wchar_t* out, size_t capacity, LPCWSTR text)
{
    assert(std::wcslen(text) < capacity);
    std::wcscpy(out, text); return 0;
}
template <size_t N> inline int wcscpy_s(wchar_t (&out)[N], LPCWSTR text)
{ return wcscpy_s(out, N, text); }
inline int wcscat_s(wchar_t* out, size_t capacity, LPCWSTR text)
{
    assert(std::wcslen(out) + std::wcslen(text) < capacity);
    std::wcscat(out, text); return 0;
}
inline int wmemcpy_s(wchar_t* out, size_t capacity, LPCWSTR text, size_t count)
{ assert(count <= capacity); std::wmemcpy(out, text, count); return 0; }
inline void DdhDebugLog(int, LPCWSTR format, ...)
{
    ++DdhTest::loggerCalls;
    if (DdhTest::throwLog) throw std::runtime_error("logger failed");
    wchar_t output[2048]{};
    va_list args; va_start(args, format);
    const int result = std::vswprintf(output, std::size(output), DdhFormat(format).c_str(), args);
    va_end(args); assert(result >= 0);
    DdhTest::logs.emplace_back(output);
    if (DdhTest::duringLog) DdhTest::duringLog();
    SetLastError(0xd109);
}
inline void DdhDebugLog(LPCWSTR text) { DdhDebugLog(DBG_NORMAL, L"%s", text); }
#define DebugLog DdhDebugLog

inline BOOL GetModuleHandleExW(DWORD flags, LPCWSTR address, HMODULE* out)
{
    assert(flags == (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT));
    SetLastError(0xd101);
    const void* ptr = reinterpret_cast<const void*>(address);
    if (DdhTest::unknownCaller && ptr == DdhTest::returnAddress) { *out = nullptr; return FALSE; }
    *out = ptr == DdhTest::externalAddress ? &DdhTest::app : &DdhTest::core; return TRUE;
}
inline DWORD GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD capacity)
{
    SetLastError(0xd102);
    const auto* path = module == &DdhTest::app ? LR"(C:\App\Host.exe)" : LR"(C:\Glass\GlassCore.dll)";
    wcscpy_s(out, capacity, path); return DWORD(std::wcslen(out));
}
inline USHORT CaptureStackBackTrace(DWORD, DWORD capacity, void** frames, DWORD*)
{
    SetLastError(0xd103); assert(capacity);
    if (DdhTest::unknownStack) return 0;
    frames[0] = const_cast<void*>(DdhTest::externalAddress); return 1;
}
inline HMODULE DdhGetModuleHandleW(LPCWSTR) { return &DdhTest::system; }
inline HMODULE LoadLibraryW(LPCWSTR) { return &DdhTest::system; }
inline void WINAPI DdhLazyExport() {}
inline FARPROC GetProcAddress(HMODULE, LPCSTR) { return &DdhLazyExport; }
#define GetModuleHandleW DdhGetModuleHandleW

inline BYTE GetRValue(COLORREF c) { return BYTE(c); }
inline BYTE GetGValue(COLORREF c) { return BYTE(c >> 8); }
inline BYTE GetBValue(COLORREF c) { return BYTE(c >> 16); }
inline COLORREF GetPixel(HDC, int, int) { return 0; }
inline int GetStretchBltMode(HDC) { return 1; }
inline HGDIOBJ GetStockObject(int) { return nullptr; }
inline HBRUSH GetStockBrush(int) { return nullptr; }
inline HBRUSH GetSysColorBrush(int) { return nullptr; }
inline HBRUSH CreateSolidBrush(COLORREF) { return nullptr; }
inline COLORREF SetDCBrushColor(HDC, COLORREF) { return 0; }
inline COLORREF SetBkColor(HDC dc, COLORREF c) { return std::exchange(dc->background, c); }
inline LONG_PTR SetClassLongPtrW(HWND, int, LONG_PTR) { return 0; }
inline int SetDIBits(HDC, HBITMAP, UINT, UINT, CONST VOID*, CONST BITMAPINFO*, UINT) { return 0; }
inline BOOL Rectangle(HDC, int, int, int, int) { return TRUE; }
inline BOOL BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD) { return TRUE; }
inline BOOL StretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD) { return TRUE; }
inline BOOL AnimateWindow(HWND, DWORD, DWORD) { return TRUE; }
inline BOOL TextOutW(HDC, int, int, LPCWSTR, int) { return TRUE; }
inline HRESULT DrawThemeBackground(HTHEME, HDC, int, int, LPCRECT, LPCRECT) { return S_OK; }
inline HRESULT DrawThemeBackgroundEx(HTHEME, HDC, int, int, LPCRECT, const DTBGOPTS*) { return S_OK; }
inline HRESULT DrawThemeTextEx(HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS*) { return S_OK; }
inline HRESULT DwmExtendFrameIntoClientArea(HWND, const MARGINS*) { return S_OK; }
inline HRESULT DwmSetWindowAttribute(HWND, DWORD, LPCVOID, DWORD) { return S_OK; }
inline HPAINTBUFFER BeginBufferedPaint(HDC, const RECT*, BP_BUFFERFORMAT, BP_PAINTPARAMS*, HDC*) { return nullptr; }
inline HRESULT EndBufferedPaint(HPAINTBUFFER, BOOL) { return S_OK; }
inline HRESULT BufferedPaintClear(HPAINTBUFFER, const RECT*) { return S_OK; }
inline HRESULT BufferedPaintSetAlpha(HPAINTBUFFER, const RECT*, BYTE) { return S_OK; }
namespace Shared::ThemeHelper
{
    inline BOOL GetThemeClass(HTHEME, LPWSTR out, size_t capacity)
    { wcscpy_s(out, capacity, L"Panel"); return TRUE; }
}

inline LONG DdhNative(size_t api, DdhTest::Call call)
{
    ++DdhTest::nativeCalls[api]; call.entryError = GetLastError(); DdhTest::calls[api] = call;
    if (DdhTest::mutateNative && api < 2)
    {
        if (call.rect && (call.flags & DT_CALCRECT)) const_cast<RECT*>(call.rect)->right += 7;
        if (call.text && (call.flags & DT_MODIFYSTRING)) static_cast<char*>(const_cast<void*>(call.text))[0] = 'X';
        if (api == 1 && call.extra) ++static_cast<DRAWTEXTPARAMS*>(const_cast<void*>(call.extra))->cbSize;
    }
    if (!DdhTest::nativeLeavesError) SetLastError(DdhTest::nativeError);
    return DdhTest::results[api];
}
inline int WINAPI DdhNativeDrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
{ return DdhNative(0, {dc,text,count,flags,rect}); }
inline int WINAPI DdhNativeDrawTextExA(HDC dc, LPSTR text, int count, LPRECT rect, UINT flags, LPDRAWTEXTPARAMS params)
{ return DdhNative(1, {dc,text,count,flags,rect,params}); }
inline BOOL WINAPI DdhNativeExtTextOutA(HDC dc, int x, int y, UINT flags, const RECT* rect,
    LPCSTR text, UINT count, const INT* dx)
{ return DdhNative(2, {dc,text,count,flags,rect,dx,x,y}); }
inline BOOL WINAPI DdhNativeTextOutA(HDC dc, int x, int y, LPCSTR text, int count)
{ return DdhNative(3, {dc,text,count,0,nullptr,nullptr,x,y}); }
inline LONG WINAPI DdhNativeTabbedTextOutA(HDC dc, int x, int y, LPCSTR text, int count,
    int tabCount, const INT* tabs, int tabOrigin)
{ return DdhNative(4, {dc,text,count,0,nullptr,tabs,x,y,tabCount,tabOrigin}); }
inline LONG WINAPI DdhNativeTabbedTextOutW(HDC dc, int x, int y, LPCWSTR text, int count,
    int tabCount, const INT* tabs, int tabOrigin)
{ return DdhNative(5, {dc,text,count,0,nullptr,tabs,x,y,tabCount,tabOrigin}); }
inline int DrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
{
    using Fn = decltype(&DdhNativeDrawTextA);
    return (DdhTest::hooks[0] ? reinterpret_cast<Fn>(DdhTest::hooks[0]) : &DdhNativeDrawTextA)(dc,text,count,rect,flags);
}
inline int DrawTextExA(HDC dc, LPSTR text, int count, LPRECT rect, UINT flags, LPDRAWTEXTPARAMS params)
{
    using Fn = decltype(&DdhNativeDrawTextExA);
    return (DdhTest::hooks[1] ? reinterpret_cast<Fn>(DdhTest::hooks[1]) : &DdhNativeDrawTextExA)(dc,text,count,rect,flags,params);
}
inline BOOL ExtTextOutA(HDC dc, int x, int y, UINT flags, const RECT* rect, LPCSTR text, UINT count, const INT* dx)
{
    using Fn = decltype(&DdhNativeExtTextOutA);
    return (DdhTest::hooks[2] ? reinterpret_cast<Fn>(DdhTest::hooks[2]) : &DdhNativeExtTextOutA)(dc,x,y,flags,rect,text,count,dx);
}
inline BOOL TextOutA(HDC dc, int x, int y, LPCSTR text, int count)
{
    using Fn = decltype(&DdhNativeTextOutA);
    return (DdhTest::hooks[3] ? reinterpret_cast<Fn>(DdhTest::hooks[3]) : &DdhNativeTextOutA)(dc,x,y,text,count);
}
inline LONG TabbedTextOutA(HDC dc, int x, int y, LPCSTR text, int count, int tabCount, const INT* tabs, int tabOrigin)
{
    using Fn = decltype(&DdhNativeTabbedTextOutA);
    return (DdhTest::hooks[4] ? reinterpret_cast<Fn>(DdhTest::hooks[4]) : &DdhNativeTabbedTextOutA)(dc,x,y,text,count,tabCount,tabs,tabOrigin);
}
inline LONG TabbedTextOutW(HDC dc, int x, int y, LPCWSTR text, int count, int tabCount, const INT* tabs, int tabOrigin)
{
    using Fn = decltype(&DdhNativeTabbedTextOutW);
    return (DdhTest::hooks[5] ? reinterpret_cast<Fn>(DdhTest::hooks[5]) : &DdhNativeTabbedTextOutW)(dc,x,y,text,count,tabCount,tabs,tabOrigin);
}
namespace DdhTest
{
    inline const std::array<void*, 6> exports{
        reinterpret_cast<void*>(&::DrawTextA), reinterpret_cast<void*>(&::DrawTextExA),
        reinterpret_cast<void*>(&::ExtTextOutA), reinterpret_cast<void*>(&::TextOutA),
        reinterpret_cast<void*>(&::TabbedTextOutA), reinterpret_cast<void*>(&::TabbedTextOutW)};
    inline const std::array<void*, 6> trampolines{
        reinterpret_cast<void*>(&DdhNativeDrawTextA), reinterpret_cast<void*>(&DdhNativeDrawTextExA),
        reinterpret_cast<void*>(&DdhNativeExtTextOutA), reinterpret_cast<void*>(&DdhNativeTextOutA),
        reinterpret_cast<void*>(&DdhNativeTabbedTextOutA), reinterpret_cast<void*>(&DdhNativeTabbedTextOutW)};
}
namespace GlassCore::Hooks::Detours
{
    inline void Attach(void** real, void* hook)
    {
        assert(real && *real && hook && !DdhTest::attached.count(real));
        if (++DdhTest::operations == DdhTest::failOperation) throw std::runtime_error("Attach failed");
        DdhTest::pending.push_back({real,hook,true});
    }
    inline void Detach(void** real, void* hook)
    {
        assert(real && *real && DdhTest::attached.at(real) == hook);
        if (++DdhTest::operations == DdhTest::failOperation) throw std::runtime_error("Detach failed");
        DdhTest::pending.push_back({real,hook,false});
    }
    inline HRESULT Write(const std::function<void()>& callback) noexcept
    {
        ++DdhTest::writes; DdhTest::operations = 0; DdhTest::pending.clear();
        if (DdhTest::failBegin) return E_FAIL;
        try { callback(); }
        catch (...) { ++DdhTest::aborts; DdhTest::pending.clear(); return E_FAIL; }
        if (DdhTest::failCommit) { ++DdhTest::aborts; DdhTest::pending.clear(); return E_FAIL; }
        for (const auto& op : DdhTest::pending)
        {
            for (size_t i = 0; i < DdhTest::exports.size(); ++i)
            {
                if (*op.real != (op.enable ? DdhTest::exports[i] : DdhTest::trampolines[i])) continue;
                assert(op.enable ? !DdhTest::hooks[i] : DdhTest::hooks[i] == op.hook);
                DdhTest::hooks[i] = op.enable ? op.hook : nullptr;
                *op.real = op.enable ? DdhTest::trampolines[i] : DdhTest::exports[i];
                break;
            }
            if (op.enable) DdhTest::attached.emplace(op.real, op.hook);
            else DdhTest::attached.erase(op.real);
            DdhTest::committed.push_back(op);
        }
        DdhTest::pending.clear(); ++DdhTest::commits; return S_OK;
    }
}
