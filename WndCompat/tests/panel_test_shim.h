// Panel-only extensions to portable Win32/IAT/Detours doubles. No real
// Windows rasterization is claimed; the production DIB bytes are inspected.
#pragma once
#if !defined(GLASS_TEST_REAL_OPAQUE_BITMAP)
#define HookDispatcher LegacyPanelHookDispatcher
#endif
#include "coolbar_test_shim.h"
#if !defined(GLASS_TEST_REAL_OPAQUE_BITMAP)
#undef HookDispatcher
#endif
#include <limits>
#include <stdexcept>
using BYTE = std::uint8_t;
using ULONG_PTR = std::uintptr_t;
using LONG_PTR = std::intptr_t;
using FARPROC = void(*)();
struct BITMAP
{
    LONG bmType{}, bmWidth{1}, bmHeight{1}, bmWidthBytes{4};
    std::uint16_t bmPlanes{1}, bmBitsPixel{32};
    void* bmBits{};
};
struct TestBitmap
{
    std::array<BYTE, 4> pixel{};
    BITMAP info{};
    std::vector<BYTE> pixels;
    BYTE* Data() { return pixels.empty() ? pixel.data() : pixels.data(); }
};
using HBITMAP = TestBitmap*;
inline constexpr int MM_TEXT = 1, OBJ_BITMAP = 7, GWLP_HINSTANCE = -6;
struct BITMAPINFOHEADER
{
    DWORD biSize{}; LONG biWidth{}, biHeight{}; unsigned short biPlanes{}, biBitCount{};
    DWORD biCompression{};
};
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader{}; };
struct BLENDFUNCTION { BYTE BlendOp{}, BlendFlags{}, SourceConstantAlpha{}, AlphaFormat{}; };
inline constexpr DWORD BI_RGB = 0;
inline constexpr UINT DIB_RGB_COLORS = 0;
inline constexpr BYTE AC_SRC_OVER = 0, AC_SRC_ALPHA = 1;
inline constexpr int COLOR_MENUBAR = 30;
inline const HGDIOBJ HGDI_ERROR = reinterpret_cast<HGDIOBJ>(static_cast<std::intptr_t>(-1));
inline BYTE GetRValue(COLORREF c) { return c & 255; }
inline BYTE GetGValue(COLORREF c) { return (c >> 8) & 255; }
inline BYTE GetBValue(COLORREF c) { return (c >> 16) & 255; }
namespace PanelTest
{
    inline COLORREF windowColor = 0xffffff, faceColor = 0xf0f0f0;
    inline std::unordered_map<HWND, RECT> clients;
    inline std::unordered_map<HDC, HGDIOBJ> bitmaps;
    inline bool failDc = false, failBitmap = false, failSelect = false, failBlend = false;
    inline bool coreAvailable = true, endAvailable = true;
    inline TestModule user32;
    inline bool user32Available = true, textExportAvailable = true;
    inline FARPROC beginExport{}, endExport{};
    inline int textResolutions = 0;
    inline int liveDcs = 0, liveBitmaps = 0, blends = 0;
    inline RECT blendRect{};
    inline BLENDFUNCTION blendMode{};
    inline std::array<BYTE, 4> blendPixel{};
    inline std::function<void()> duringBlend;
    inline HGDIOBJ stockBitmap = reinterpret_cast<HGDIOBJ>(std::uintptr_t(1));
    inline std::unordered_map<HWND, HWND> parents;
    inline std::unordered_map<HWND, HMODULE> instances;
    inline std::unordered_map<HDC, int> maps;
    inline thread_local DWORD error{};
}
inline HWND GetParent(HWND w) { return PanelTest::parents.count(w) ? PanelTest::parents.at(w) : nullptr; }
inline LONG_PTR GetWindowLongPtrW(HWND w, int index)
{
    assert(index == GWLP_HINSTANCE);
    return reinterpret_cast<LONG_PTR>(PanelTest::instances.count(w) ? PanelTest::instances.at(w)
        : GlassCore::ModuleHandler::g_entryPointModule);
}
inline int GetMapMode(HDC dc) { return PanelTest::maps.count(dc) ? PanelTest::maps.at(dc) : MM_TEXT; }
inline DWORD GetLastError() { return PanelTest::error; }
inline void SetLastError(DWORD error) { PanelTest::error = error; }
inline HGDIOBJ PanelCurrentObject(HDC dc, UINT type)
{
    if (type == OBJ_BITMAP)
        return PanelTest::bitmaps.count(dc) ? PanelTest::bitmaps.at(dc) : PanelTest::stockBitmap;
    return GetCurrentObject(dc, type);
}
inline int GetObjectW(HGDIOBJ object, int bytes, void* out)
{
    if (bytes != sizeof(BITMAP)) return GetObjectW(static_cast<HBRUSH>(object), bytes, out);
    if (reinterpret_cast<ULONG_PTR>(object) <= 5) return 0;
    *static_cast<BITMAP*>(out) = static_cast<HBITMAP>(object)->info; return bytes;
}
inline BOOL StretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD) { return TRUE; }
#if !defined(GLASS_TEST_REAL_OPAQUE_BITMAP)
// Background/caption suite never prepares images. The dedicated VCL image
// suite defines this macro and compiles the real source/selection helper.
namespace GlassCore::GdiUtils
{
    struct OpaqueBitmapSource
    {
        explicit OpaqueBitmapSource(HDC) {}
        explicit operator bool() const { return false; }
    };
}
#endif
inline COLORREF PanelGetSysColor(int i)
{ return i == COLOR_WINDOW ? PanelTest::windowColor : PanelTest::faceColor; }
inline BOOL PanelGetClientRect(HWND w, LPRECT r)
{
    if (auto it = PanelTest::clients.find(w); it != PanelTest::clients.end()) *r = it->second;
    else *r = RECT{0, 0, 500, 40};
    return TRUE;
}
inline HDC CreateCompatibleDC(HDC)
{
    if (PanelTest::failDc) return nullptr;
    auto* dc = new TestDC; ++PanelTest::liveDcs;
    PanelTest::bitmaps[dc] = PanelTest::stockBitmap; return dc;
}
inline HBITMAP CreateDIBSection(HDC, const BITMAPINFO* bi, UINT, void** bits, void*, DWORD)
{
    assert(bi->bmiHeader.biWidth == 1 && bi->bmiHeader.biHeight == -1);
    assert(bi->bmiHeader.biBitCount == 32 && bi->bmiHeader.biPlanes == 1);
    if (PanelTest::failBitmap) return nullptr;
    auto* bmp = new TestBitmap; ++PanelTest::liveBitmaps;
    *bits = bmp->pixel.data(); return bmp;
}
inline HGDIOBJ SelectObject(HDC dc, HGDIOBJ bitmap)
{
    if (PanelTest::failSelect) return HGDI_ERROR;
    return std::exchange(PanelTest::bitmaps.at(dc), bitmap);
}
inline HGDIOBJ SelectObject(HDC dc, HBITMAP bitmap)
{ return SelectObject(dc, static_cast<HGDIOBJ>(bitmap)); }
inline BOOL DeleteObject(HBITMAP bmp)
{ delete bmp; --PanelTest::liveBitmaps; return TRUE; }
inline BOOL DeleteDC(HDC dc)
{ PanelTest::bitmaps.erase(dc); delete dc; --PanelTest::liveDcs; return TRUE; }
inline BOOL GdiAlphaBlend(HDC, int x, int y, int w, int h, HDC src,
    int sx, int sy, int sw, int sh, BLENDFUNCTION blend)
{
    assert(sx == 0 && sy == 0 && sw == 1 && sh == 1);
    ++PanelTest::blends; PanelTest::blendRect = RECT{x, y, x+w, y+h};
    PanelTest::blendMode = blend;
    PanelTest::blendPixel = static_cast<HBITMAP>(PanelTest::bitmaps.at(src))->pixel;
    if (PanelTest::duringBlend) PanelTest::duringBlend();
    return !PanelTest::failBlend;
}
inline HMODULE PanelGetModuleHandleW(LPCWSTR name)
{
    if (std::wstring_view(name) == L"user32.dll")
        return PanelTest::user32Available ? &PanelTest::user32 : nullptr;
    return PanelTest::coreAvailable && std::wstring_view(name) == L"GlassCore.dll"
        ? GlassCore::ModuleHandler::g_entryPointModule : nullptr;
}
FARPROC PanelGetProcAddress(HMODULE, LPCSTR);
// DrawThemeTextEx structures keep the x64 SDK layout/fields used by VCL.
struct POINT { LONG x{}, y{}; };
struct DTTOPTS
{
    DWORD dwSize{}, dwFlags{};
    COLORREF crText{}, crBorder{}, crShadow{};
    int iTextShadowType{};
    POINT ptShadowOffset{};
    int iBorderSize{}, iFontPropId{}, iColorPropId{}, iStateId{};
    BOOL fApplyOverlay{};
    int iGlowSize{};
    void* pfnDrawTextCallback{};
    LPARAM lParam{};
};
static_assert(sizeof(DTTOPTS) == 72);
inline constexpr DWORD DTT_TEXTCOLOR = 1, DTT_CALCRECT = 0x200, DTT_GLOWSIZE = 0x800;
inline constexpr DWORD DTT_CALLBACK = 0x1000, DTT_COMPOSITED = 0x2000;
using HPAINTBUFFER = void*;
struct RGBQUAD { BYTE rgbBlue{}, rgbGreen{}, rgbRed{}, rgbReserved{}; };
namespace PanelTest
{
    inline int themeBuffers = 0, nativeThemeCalls = 0, chainedThemeCalls = 0;
    inline HMODULE rootOriginalModule{};
    inline HRESULT bufferResult = S_OK;
    inline bool failThemeBufferDraw = false;
    inline HDC themeDc{};
    inline HTHEME theme{};
    inline int themePart{}, themeState{}, themeCount{};
    inline DWORD themeFlags{};
    inline LPCWSTR themeText{};
    inline const DTTOPTS* optionsPointer{};
    inline DTTOPTS themeOptions{};
    inline RECT themeRect{}, bufferedRect{};
    inline bool bufferErased = true;
    inline std::function<void()> duringThemeBuffer;
}
inline HRESULT DrawThemeTextEx(HTHEME theme, HDC dc, int part, int state,
    LPCWSTR text, int count, DWORD flags, LPRECT rect, const DTTOPTS* options)
{
    ++PanelTest::nativeThemeCalls;
    PanelTest::theme = theme; PanelTest::themeDc = dc;
    PanelTest::themePart = part; PanelTest::themeState = state;
    PanelTest::themeText = text; PanelTest::themeCount = count; PanelTest::themeFlags = flags;
    PanelTest::optionsPointer = options;
    PanelTest::themeOptions = options ? *options : DTTOPTS{};
    PanelTest::themeRect = rect ? *rect : RECT{};
    if (rect && ((flags & DT_CALCRECT) || (options && (options->dwFlags & DTT_CALCRECT)))) rect->right += 5;
    return PanelTest::failThemeBufferDraw && dc && !dc->owner ? E_FAIL : S_OK;
}
namespace GlassCore::Root
{
    // Only environment doubles for the production snippets included by the
    // runner: cache storage, root dispatcher lookup and shutdown state.
    inline constexpr size_t OHK_DELPHI_DRAWTHEMETEXTEX = 0;
    inline std::atomic<bool> g_shutdown{false};
    struct RootHookCache { std::vector<HMODULE> paintHooks, dcHooks; };
    inline bool StrEqI(LPCWSTR a, LPCWSTR b) { return a && b && _wcsicmp(a, b) == 0; }
    inline void PushPaint(HMODULE module, size_t slot, RootHookCache& cache)
    { assert(slot == OHK_DELPHI_DRAWTHEMETEXTEX); cache.paintHooks.push_back(module); }
    inline void PushDc(HMODULE module, size_t slot, RootHookCache& cache)
    { assert(slot == OHK_DELPHI_DRAWTHEMETEXTEX); cache.dcHooks.push_back(module); }
    template <size_t Slot, typename Fn> Fn GetRootOrg(HMODULE module)
    {
        static_assert(Slot == OHK_DELPHI_DRAWTHEMETEXTEX);
        PanelTest::rootOriginalModule = module;
        if (!module || !module->refs["DrawThemeTextEx"]) return nullptr;
        const auto it = module->originals.find("DrawThemeTextEx");
        return it == module->originals.end() ? nullptr : reinterpret_cast<Fn>(it->second);
    }
}
namespace GlassCore::Text
{
    inline HRESULT DrawThemeContent(HDC dc, const RECT& rect, const RECT*, const RECT*, DWORD,
        const std::function<void(HDC, HPAINTBUFFER, RGBQUAD*, int)>& callback,
        BYTE alpha, bool useBlt, bool update, bool erase)
    {
        assert(alpha == 255 && !useBlt && update);
        ++PanelTest::themeBuffers; PanelTest::bufferedRect = rect; PanelTest::bufferErased = erase;
        if (!SUCCEEDED(PanelTest::bufferResult)) return PanelTest::bufferResult;
        TestDC memory;
        static_cast<DcState&>(memory) = *dc;
        if (PanelTest::duringThemeBuffer) PanelTest::duringThemeBuffer();
        callback(&memory, nullptr, nullptr, 0);
        return PanelTest::bufferResult;
    }
}
// Narrow ANSI/codepage doubles. They deliberately model only the fixture byte
// sequences below, not the Windows NLS tables or GDI's actual font selection.
struct CHARSETINFO { UINT ciCharset{}, ciACP{}; };
inline constexpr int DEFAULT_CHARSET = 1, SYMBOL_CHARSET = 2, TCI_SRCCHARSET = 1, CP_ACP = 0;
// Keep this new fixture constant local: the ListView suite also reuses this
// shim indirectly and already defines its own global DT_NOCLIP.
namespace GlassCore::DelphiPanel { inline constexpr UINT DT_NOCLIP = 0x100; }
namespace PanelTest
{
    inline std::unordered_map<HFONT, int> fontCharsets;
    inline UINT systemCodepage = 1252, codepage{};
    inline int conversions = 0, translations = 0;
    inline bool failTranslate = false, failConvert = false, partialConvert = false, throwConvert = false;
    inline bool failSave = false, failMode = false, mutateRenderer = false, throwRenderer = false;
    inline int nativeAnsi = 0, chainedAnsi = 0, renderedResult = 15, nativeResult = 43;
    inline HDC ansiDc{}, alphaDc{};
    inline LPCSTR ansiPointer{};
    inline LPRECT ansiRect{}, alphaRectPointer{};
    inline int ansiCount{}, alphaCount{};
    inline UINT ansiFlags{}, alphaFlags{};
    inline DWORD nativeEntryError{}, nativeError = 0x5151;
    inline bool nativeLeavesError = false;
    inline DcState nativeState{}, alphaState{};
    struct DcDetails { RECT clip{2,3,90,40}; int viewportX{4}, viewportY{6}; };
    inline std::unordered_map<HDC, DcDetails> dcDetails;
    struct SavedDetails { int index{}; DcDetails details{}; int map{}; };
    inline std::unordered_map<HDC, std::vector<SavedDetails>> savedDetails;
    inline DcDetails alphaDetails{}, nativeDetails{};
    inline RECT alphaRect{};
    inline std::wstring alphaText;
    inline thread_local ULONGLONG clock = 100;
    inline thread_local std::vector<std::wstring> logFormats;
    inline thread_local std::vector<std::wstring> logStrings;
    inline std::unordered_map<HMODULE, std::unordered_map<std::string, std::vector<void*>>> occupants;
    inline std::unordered_map<HMODULE, std::vector<std::string>> registrationFailures, noOriginal;
}
inline int GetTextCharset(HDC dc)
{
    SetLastError(0xd101);
    const auto it = PanelTest::fontCharsets.find(dc ? dc->font : nullptr);
    return it == PanelTest::fontCharsets.end() ? DEFAULT_CHARSET : it->second;
}
inline BOOL TranslateCharsetInfo(DWORD* source, CHARSETINFO* out, DWORD mode)
{
    SetLastError(0xd102); ++PanelTest::translations;
    assert(mode == TCI_SRCCHARSET);
    if (PanelTest::failTranslate) return FALSE;
    const auto charset = reinterpret_cast<ULONG_PTR>(source);
    if (charset != 0 && charset != 128 && charset != 204) return FALSE;
    out->ciACP = charset == 128 ? 932 : charset == 204 ? 1251 : 1252; return TRUE;
}
inline int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR text, int bytes, LPWSTR out, int capacity)
{
    SetLastError(0xd103); ++PanelTest::conversions; PanelTest::codepage = cp;
    assert(flags == 0 && text && (bytes > 0 || bytes == -1));
    if (PanelTest::throwConvert) throw std::bad_alloc{};
    if (PanelTest::failConvert || (out && PanelTest::partialConvert)) return 0;
    const UINT actual = cp == CP_ACP ? PanelTest::systemCodepage : cp;
    const size_t n = bytes == -1 ? std::strlen(text) + 1 : static_cast<size_t>(bytes);
    std::wstring value;
    for (size_t i = 0; i < n; ++i)
    {
        const auto ch = static_cast<unsigned char>(text[i]);
        if (actual == 932 && ch == 0x82 && i+1 < n && static_cast<unsigned char>(text[i+1]) == 0xa0)
        { value.push_back(L'\u3042'); ++i; }
        else if (actual == 65001 && ch == 0xf0 && i+3 < n
            && static_cast<unsigned char>(text[i+1]) == 0x9f
            && static_cast<unsigned char>(text[i+2]) == 0x99
            && static_cast<unsigned char>(text[i+3]) == 0x82)
        { value.push_back(wchar_t(0xd83d)); value.push_back(wchar_t(0xde42)); i += 3; }
        else if (actual == 1251 && ch >= 0xc0) value.push_back(wchar_t(0x410 + ch - 0xc0));
        else if (actual == 1252 && ch == 0xe9) value.push_back(L'\u00e9');
        else value.push_back(wchar_t(ch));
    }
    if (out)
    {
        assert(capacity >= static_cast<int>(value.size()));
        std::copy(value.begin(), value.end(), out);
    }
    return static_cast<int>(value.size());
}
inline int PanelNativeDrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
{
    ++PanelTest::nativeAnsi; PanelTest::nativeEntryError = GetLastError();
    PanelTest::ansiDc = dc; PanelTest::ansiPointer = text; PanelTest::ansiCount = count;
    PanelTest::ansiRect = rect; PanelTest::ansiFlags = flags;
    if (dc) { PanelTest::nativeState = *dc; PanelTest::nativeDetails = PanelTest::dcDetails[dc]; }
    if (rect && (flags & DT_CALCRECT)) rect->right += 7;
    if (text && (flags & DT_MODIFYSTRING)) const_cast<char*>(text)[0] = 'X';
    if (!PanelTest::nativeLeavesError) SetLastError(PanelTest::nativeError);
    return PanelTest::nativeResult;
}
inline int WINAPI ChainedPanelAnsi(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
{ ++PanelTest::chainedAnsi; return PanelNativeDrawTextA(dc, text, count, rect, flags); }
namespace PanelTest
{
    using AnsiFn = int(WINAPI*)(HDC, LPCSTR, int, LPRECT, UINT);
    inline AnsiFn textExport = &ChainedPanelAnsi, trampolineTarget{};
    inline void* textDetour{};
    inline bool failDetourBegin = false, failAttach = false, failCommit = false, failDetach = false;
    inline int detourWrites = 0, attachAttempts = 0, detachAttempts = 0;
    inline int attaches = 0, detaches = 0, trampolines = 0;
    struct PendingDetour { void** real{}; void* hook{}; bool enable{}; };
    inline PendingDetour pending;
}
// Export dispatch and trampoline are distinct: calling DrawTextA again on a
// native fallback would re-enter the installed hook, not bypass it. This is a
// behavioral double, not executable instruction patching or a real Detours ABI.
inline int DrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
{
    const auto target = PanelTest::textDetour
        ? reinterpret_cast<PanelTest::AnsiFn>(PanelTest::textDetour) : PanelTest::textExport;
    return target(dc, text, count, rect, flags);
}
inline int WINAPI PanelAnsiTrampoline(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
{
    ++PanelTest::trampolines;
    assert(PanelTest::trampolineTarget && PanelTest::textDetour);
    return PanelTest::trampolineTarget(dc, text, count, rect, flags);
}
inline FARPROC PanelGetProcAddress(HMODULE module, LPCSTR name)
{
    if (module == &PanelTest::user32 && std::strcmp(name, "DrawTextA") == 0)
    {
        ++PanelTest::textResolutions;
        if (!PanelTest::textExportAvailable) return nullptr;
        return reinterpret_cast<FARPROC>(PanelTest::textDetour ? &::DrawTextA : PanelTest::textExport);
    }
    if (module != GlassCore::ModuleHandler::g_entryPointModule) return nullptr;
    if (std::strcmp(name, "GlassCore_BeginDelphiPanelPaint") == 0) return PanelTest::beginExport;
    if (PanelTest::endAvailable && std::strcmp(name, "GlassCore_EndDelphiPanelPaint") == 0)
        return PanelTest::endExport;
    return nullptr;
}
namespace GlassCore::Hooks::Detours
{
    inline void Attach(void** real, void* hook)
    {
        ++PanelTest::attachAttempts;
        if (PanelTest::failAttach) throw std::runtime_error("DetourAttach failed");
        assert(!PanelTest::pending.real && real && *real && hook && !PanelTest::textDetour);
        assert(*real == reinterpret_cast<void*>(PanelTest::textExport));
        PanelTest::pending = {real, hook, true};
    }
    inline void Detach(void** real, void* hook)
    {
        ++PanelTest::detachAttempts;
        if (PanelTest::failDetach) throw std::runtime_error("DetourDetach failed");
        assert(!PanelTest::pending.real && real && *real == reinterpret_cast<void*>(&PanelAnsiTrampoline));
        assert(PanelTest::textDetour == hook);
        PanelTest::pending = {real, hook, false};
    }
    inline HRESULT Write(const std::function<void()>& callback) noexcept
    {
        ++PanelTest::detourWrites;
        if (PanelTest::failDetourBegin) return E_FAIL;
        PanelTest::pending = {};
        try { callback(); }
        catch (...) { PanelTest::pending = {}; return E_FAIL; }
        const auto op = std::exchange(PanelTest::pending, {});
        if (PanelTest::failCommit) return E_FAIL; // No pointer/patch mutation on aborted commit.
        assert(op.real);
        if (op.enable)
        {
            ++PanelTest::attaches;
            PanelTest::trampolineTarget = reinterpret_cast<PanelTest::AnsiFn>(*op.real);
            PanelTest::textDetour = op.hook;
            *op.real = reinterpret_cast<void*>(&PanelAnsiTrampoline);
        }
        else
        {
            ++PanelTest::detaches;
            *op.real = reinterpret_cast<void*>(PanelTest::trampolineTarget);
            PanelTest::textDetour = nullptr;
            PanelTest::trampolineTarget = nullptr;
        }
        return S_OK;
    }
}

#if !defined(GLASS_TEST_REAL_OPAQUE_BITMAP)
inline BOOL FAILED(HRESULT hr) { return hr < 0; }
namespace GlassCore::Hooks
{
    // Ref-delta + first-active-occupant model, not executable PE/IAT machinery.
    // A nested EnableHook returns false even though it acquired another ref.
    // Other registrants may hold refs without becoming the active occupant.
    template <size_t N, size_t M> struct HookDispatcher
    {
        const HookDispatcherDependency<N, M>& dependency;
        void* moduleAddress{};
        std::array<int, N> refs{};
        HookDispatcher(const HookDispatcherDependency<N, M>& dep, void* base)
            : dependency(dep), moduleAddress(base) {}
        void CacheHookData() {}
        int GetRef(size_t i) const { return refs.at(i); }
        bool EnableHook(size_t i, bool enable)
        {
            auto* mod = static_cast<HMODULE>(moduleAddress);
            const std::string name = std::get<1>(dependency.at(i));
            if (!mod || !mod->originals.count(name)) return false;
            auto& entries = PanelTest::occupants[mod][name];
            void* detour = std::get<2>(dependency.at(i));
            if (enable)
            {
                const auto& fails = PanelTest::registrationFailures[mod];
                if (std::find(fails.begin(), fails.end(), name) != fails.end()) return false;
                ++mod->refs[name];
                if (refs[i]++) return false;
                entries.push_back(detour);
            }
            else
            {
                if (!refs[i]) return false;
                --mod->refs[name];
                if (--refs[i]) return false;
                const auto found = std::find(entries.begin(), entries.end(), detour);
                assert(found != entries.end()); entries.erase(found);
            }
            if (entries.empty()) mod->hooks.erase(name);
            else mod->hooks[name] = entries.front();
            return true;
        }
        template <size_t I, typename Fn> Fn GetOrg() const
        {
            SetLastError(0xd104);
            auto* mod = static_cast<HMODULE>(moduleAddress);
            if (!mod || !refs[I]) return nullptr;
            const std::string name = std::get<1>(dependency.at(I));
            const auto& missing = PanelTest::noOriginal[mod];
            if (std::find(missing.begin(), missing.end(), name) != missing.end()) return nullptr;
            return reinterpret_cast<Fn>(mod->originals.at(name));
        }
        void DisableAllHooks()
        { for (size_t i = N; i > 0; --i) while (refs[i-1]) EnableHook(i-1, false); }
    };
}
inline int PanelSaveDC(HDC dc)
{
    SetLastError(0xd105);
    if (PanelTest::failSave) return 0;
    const int index = ::SaveDC(dc);
    PanelTest::savedDetails[dc].push_back({index, PanelTest::dcDetails[dc], GetMapMode(dc)});
    return index;
}
inline BOOL PanelRestoreDC(HDC dc, int index)
{
    SetLastError(0xd106);
    auto& details = PanelTest::savedDetails.at(dc);
    const auto it = std::find_if(details.begin(), details.end(), [=](const auto& s) { return s.index == index; });
    assert(it != details.end());
    PanelTest::dcDetails[dc] = it->details; PanelTest::maps[dc] = it->map;
    details.erase(it, details.end());
    return ::RestoreDC(dc, index);
}
inline int PanelSetBkMode(HDC dc, int mode)
{ SetLastError(0xd107); return PanelTest::failMode ? 0 : ::SetBkMode(dc, mode); }
inline ULONGLONG PanelTickCount64() { SetLastError(0xd108); return PanelTest::clock; }
template <typename T> inline void PanelCaptureLogString(T value)
{
    if constexpr (std::is_convertible_v<T, LPCWSTR>)
        if (value) PanelTest::logStrings.emplace_back(value);
}
template <typename... Args> inline void PanelDebugLog(int, LPCWSTR format, Args... args)
{
    PanelTest::logFormats.emplace_back(format);
    (PanelCaptureLogString(args), ...);
    SetLastError(0xd109);
}
namespace GlassCore::Text
{
    inline HRESULT PanelDrawTextWithAlpha(HDC dc, LPCWSTR text, int count,
        LPRECT rect, UINT flags, int& result, bool transparent)
    {
        assert(transparent);
        PanelTest::alphaDc = dc; PanelTest::alphaText.assign(text, count);
        PanelTest::alphaCount = count; PanelTest::alphaFlags = flags;
        PanelTest::alphaRect = *rect; PanelTest::alphaRectPointer = rect;
        PanelTest::alphaState = *dc; PanelTest::alphaDetails = PanelTest::dcDetails[dc];
        const HRESULT hr = DrawTextWithAlpha(dc, text, count, rect, flags, result, transparent);
        if (PanelTest::mutateRenderer)
        {
            dc->text = 0x123456; dc->background = 0x654321; dc->bkMode = OPAQUE;
            dc->font = nullptr; dc->brush = nullptr; *rect = RECT{-1,-2,-3,-4};
            PanelTest::maps[dc] = 5; PanelTest::dcDetails[dc] = {{9,8,7,6}, -5, -4};
        }
        if (PanelTest::throwRenderer) throw std::runtime_error("injected renderer failure");
        result = PanelTest::renderedResult; SetLastError(0xd110); return hr;
    }
}
#define SaveDC PanelSaveDC
#define RestoreDC PanelRestoreDC
#define SetBkMode PanelSetBkMode
#define GetTickCount64 PanelTickCount64
#define DebugLog PanelDebugLog
#define DrawTextWithAlpha PanelDrawTextWithAlpha
#endif
#define GetCurrentObject PanelCurrentObject
#define GetSysColor PanelGetSysColor
#define GetClientRect PanelGetClientRect
#define GetModuleHandleW PanelGetModuleHandleW
#define GetProcAddress PanelGetProcAddress
