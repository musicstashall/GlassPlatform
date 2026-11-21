// Win32/environment doubles only. The runner compiles the complete production
// memo implementation plus actual root acquisition/providers/thunks. These
// doubles do not prove GDI alpha, DWM output, Windows codepages or MSVC ABI.
#pragma once
#include "coolbar_test_shim.h"
#include <Hooks/RootGdiHooks.h>
#include <Hooks/RootWindowHooks.h>
#include <set>
#include <thread>
using BYTE = std::uint8_t;
using ULONG_PTR = std::uintptr_t;
struct PAINTSTRUCT { HDC hdc{}; BOOL fErase{}; RECT rcPaint{}; };
using LPPAINTSTRUCT = PAINTSTRUCT*;
struct BITMAP { LONG bmWidth{}, bmHeight{}; std::uint16_t bmPlanes{1}, bmBitsPixel{32}; };
struct CHARSETINFO { UINT ciCharset{}, ciACP{}; };
struct RGBQUAD { BYTE rgbBlue{}, rgbGreen{}, rgbRed{}, rgbReserved{}; };
struct MemoBuffer { RGBQUAD sample{}; };
using HPAINTBUFFER = MemoBuffer*;
inline constexpr int MAX_PATH = 260, MM_TEXT = 1, COLOR_MENUBAR = 30, OBJ_BITMAP = 7;
inline constexpr int DEFAULT_CHARSET = 1, SYMBOL_CHARSET = 2, TCI_SRCCHARSET = 1, CP_ACP = 0;
inline constexpr UINT DT_NOCLIP = 0x100, DT_NOPREFIX = 0x800, DT_SINGLELINE = 0x20, DT_VCENTER = 4;
inline constexpr int GA_ROOT = 2, DBG_IMPORTANT = 3;
#define THROW_HR_IF(hr, condition) do { if (condition) throw HRESULT(hr); } while (false)
#define THROW_IF_FAILED(expr) do { const HRESULT h = (expr); if (!SUCCEEDED(h)) throw h; } while (false)
namespace MemoTest
{
    inline std::wstring exePath = LR"(C:\Tools\Restorator.exe)";
    inline std::unordered_map<HDC, int> maps, charsets;
    inline BITMAP bitmap{640, 480, 1, 32};
    inline bool haveBitmap = true, failBlack = false, failSave = false, failBrush = false;
    inline bool failConvert = false, failTranslate = false, failBuffer = false, failFlush = false, failAlpha = false;
    inline int nativeAnsi = 0, chainAnsi = 0, nativeBrush = 0, chainBrush = 0, nativeColors = 0;
    inline int nativeFill = 0, chainFill = 0, buffers = 0, commits = 0, discards = 0, normalized = 0;
    inline int nativeMode{}, ansiCount{}, alphaCount{};
    inline LPCSTR ansiPointer{};
    inline LPRECT ansiRect{};
    inline UINT ansiFlags{}, alphaFlags{}, codepage{};
    inline RECT blackRect{}, alphaRect{};
    inline COLORREF windowColor = 0xffffff, faceColor = 0xf0f0f0;
    inline COLORREF nativeBackground{};
    inline LOGBRUSH lastBrush{};
    inline const LOGBRUSH* brushPointer{};
    inline std::wstring alphaText;
    inline RGBQUAD committed{};
    inline std::unordered_map<HDC, MemoBuffer*> memoryBuffers;
    inline std::function<void()> duringBuffer;
    inline int formAttachCalls = 0;

    template <size_t Count, size_t Imports> struct Dispatcher
    {
        const GlassCore::Hooks::HookDispatcherDependency<Count, Imports>& dependency;
        void* moduleAddress{};
        std::array<int, Count> refs{};
        Dispatcher(const GlassCore::Hooks::HookDispatcherDependency<Count, Imports>& dep, void* base)
            : dependency(dep), moduleAddress(base) {}
        void CacheHookData() {}
        int GetRef(size_t i) const { return refs[i]; }
        bool EnableHook(size_t i, bool enable)
        {
            auto* mod = static_cast<HMODULE>(moduleAddress);
            const auto name = std::get<1>(dependency[i]);
            if (!name || !mod->originals.count(name)) return false;
            const auto detour = std::get<2>(dependency[i]);
            if (enable)
            {
                if (refs[i]) { ++refs[i]; ++mod->refs[name]; return false; }
                // First owner wins, not a chain: match production IATRegistry.
                if (const auto h = mod->hooks.find(name); h != mod->hooks.end() && h->second != detour) return false;
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
        { for (size_t i = 0; i < Count; ++i) while (refs[i]) EnableHook(i, false); }
    };
}
inline DWORD GetModuleFileNameW(HMODULE, LPWSTR out, DWORD capacity)
{
    if (MemoTest::exePath.size() >= capacity) return capacity;
    std::wcscpy(out, MemoTest::exePath.c_str()); return MemoTest::exePath.size();
}
inline UINT GetWindowModuleFileNameW(HWND, LPWSTR out, UINT capacity)
{ return GetModuleFileNameW(GlassCore::ModuleHandler::g_entryPointModule, out, capacity); }
inline HWND GetAncestor(HWND w, UINT) { return w; }
inline int GetMapMode(HDC dc) { return MemoTest::maps.count(dc) ? MemoTest::maps.at(dc) : MM_TEXT; }
inline int GetTextCharset(HDC dc) { return MemoTest::charsets.count(dc) ? MemoTest::charsets.at(dc) : DEFAULT_CHARSET; }
inline BOOL TranslateCharsetInfo(DWORD* source, CHARSETINFO* out, DWORD mode)
{
    assert(mode == TCI_SRCCHARSET);
    if (MemoTest::failTranslate) return FALSE;
    const auto charset = reinterpret_cast<ULONG_PTR>(source);
    if (charset != 0 && charset != 128 && charset != 204) return FALSE;
    out->ciACP = charset == 128 ? 932 : charset == 204 ? 1251 : 1252; return TRUE;
}
inline int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR text, int bytes, LPWSTR out, int capacity)
{
    assert(flags == 0 && text && (bytes > 0 || bytes == -1));
    MemoTest::codepage = cp;
    if (MemoTest::failConvert) return 0;
    const size_t n = bytes == -1 ? std::strlen(text) + 1 : static_cast<size_t>(bytes);
    std::wstring value;
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char c = text[i];
        if (cp == 932 && c == 0x82 && i + 1 < n && static_cast<unsigned char>(text[i+1]) == 0xa0)
        { value.push_back(L'\u3042'); ++i; }
        else if (cp == 1251 && c >= 0xc0) value.push_back(wchar_t(0x410 + c - 0xc0));
        else value.push_back(wchar_t(c));
    }
    if (out)
    {
        assert(capacity >= static_cast<int>(value.size()));
        std::copy(value.begin(), value.end(), out);
    }
    return value.size();
}
inline DWORD MemoSysColor(int index)
{ return index == COLOR_WINDOW ? MemoTest::windowColor : index == COLOR_BTNFACE ? MemoTest::faceColor : RGB(9,8,7); }
inline DWORD ChainSysColor(int index) { ++MemoTest::nativeColors; return MemoSysColor(index); }
inline HBRUSH CreateBrushIndirect(const LOGBRUSH* brush)
{
    ++MemoTest::nativeBrush; MemoTest::brushPointer = brush;
    if (!brush || MemoTest::failBrush) return nullptr;
    MemoTest::lastBrush = *brush; return new LOGBRUSH(*brush);
}
inline HBRUSH ChainBrush(const LOGBRUSH* brush) { ++MemoTest::chainBrush; return CreateBrushIndirect(brush); }
inline HBRUSH CreateSolidBrush(COLORREF color)
{ return MemoTest::failBrush ? nullptr : new LOGBRUSH{BS_SOLID, color}; }
inline HGDIOBJ MemoCurrentObject(HDC dc, UINT type)
{ return type == OBJ_BITMAP ? (MemoTest::haveBitmap ? &MemoTest::bitmap : nullptr) : GetCurrentObject(dc, type); }
inline int MemoGetObjectW(HGDIOBJ object, int bytes, void* out)
{
    if (bytes == sizeof(BITMAP) && object == &MemoTest::bitmap)
    { std::memcpy(out, object, bytes); return bytes; }
    return GetObjectW(static_cast<HBRUSH>(object), bytes, out);
}
inline BOOL MemoPatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    MemoTest::blackRect = {x,y,x+w,y+h};
    if (MemoTest::failBlack) return FALSE;
    return PatBlt(dc,x,y,w,h,rop);
}
inline int MemoSaveDC(HDC dc) { return MemoTest::failSave ? 0 : SaveDC(dc); }
inline int DrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
{
    ++MemoTest::nativeAnsi; MemoTest::nativeMode = dc ? dc->bkMode : 0;
    MemoTest::nativeBackground = dc ? dc->background : 0;
    MemoTest::ansiPointer = text; MemoTest::ansiCount = count;
    MemoTest::ansiRect = rect; MemoTest::ansiFlags = flags;
    if (rect && (flags & DT_CALCRECT)) rect->right += 7;
    if (text && (flags & DT_MODIFYSTRING)) const_cast<char*>(text)[0] = 'X';
    return 43;
}
inline int ChainAnsi(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
{ ++MemoTest::chainAnsi; return DrawTextA(dc, text, count, rect, flags); }
inline int MemoNativeFill(HDC dc, const RECT*, HBRUSH brush)
{
    ++MemoTest::nativeFill;
    if (auto it = MemoTest::memoryBuffers.find(dc); it != MemoTest::memoryBuffers.end())
        it->second->sample = {BYTE(brush->lbColor >> 16), BYTE(brush->lbColor >> 8), BYTE(brush->lbColor), 0};
    return 37;
}
inline int ChainFill(HDC dc, const RECT* rect, HBRUSH brush)
{ ++MemoTest::chainFill; return MemoNativeFill(dc,rect,brush); }
inline BOOL GdiFlush() { return !MemoTest::failFlush; }
inline HRESULT BufferedPaintSetAlpha(HPAINTBUFFER buffer, const RECT* rect, BYTE alpha)
{
    assert(buffer && !rect && alpha == 255);
    if (MemoTest::failAlpha) return E_FAIL;
    ++MemoTest::normalized; buffer->sample.rgbReserved = alpha; return S_OK;
}
namespace wil { using unique_hbrush = std::unique_ptr<LOGBRUSH>; }
namespace GlassCore::Text
{
    inline HRESULT MemoDrawTextWithAlpha(HDC dc, LPCWSTR text, int count,
        LPRECT rect, UINT flags, int& result, bool transparent)
    {
        MemoTest::alphaText.assign(text, count); MemoTest::alphaCount = count;
        MemoTest::alphaFlags = flags; MemoTest::alphaRect = *rect;
        return DrawTextWithAlpha(dc,text,count,rect,flags,result,transparent);
    }
    inline HRESULT DrawThemeContent(HDC dc, const RECT&, const RECT*, const RECT*, DWORD,
        const std::function<void(HDC,HPAINTBUFFER,RGBQUAD*,int)>& callback,
        BYTE alpha, bool blt, bool update, bool erase)
    {
        assert(alpha == 255 && !blt && update && !erase);
        ++MemoTest::buffers;
        if (MemoTest::failBuffer) return E_FAIL;
        TestDC memory; static_cast<DcState&>(memory) = *dc;
        MemoBuffer buffer;
        MemoTest::memoryBuffers[&memory] = &buffer;
        auto clean = wil::scope_exit([&] { MemoTest::memoryBuffers.erase(&memory); });
        try
        {
            if (MemoTest::duringBuffer) MemoTest::duringBuffer();
            callback(&memory, &buffer, &buffer.sample, 1);
        }
        catch (...) { ++MemoTest::discards; return E_FAIL; }
        ++MemoTest::commits; MemoTest::committed = buffer.sample; return S_OK;
    }
}
namespace GlassCore::FormsHooks
{ inline void DrawTextExAttach(bool) { ++MemoTest::formAttachCalls; } }
namespace GlassCore::TextGlow
{
    enum class TextGlowSpreadCategory { Global };
    inline void RegisterTextGlowClient(HWND, TextGlowSpreadCategory) {}
}
#define GetSysColor MemoSysColor
#define GetCurrentObject MemoCurrentObject
#define GetObjectW MemoGetObjectW
#define PatBlt MemoPatBlt
#define SaveDC MemoSaveDC
#define FillRect MemoNativeFill
#define DrawTextWithAlpha MemoDrawTextWithAlpha
