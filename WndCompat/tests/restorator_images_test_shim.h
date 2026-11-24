// Environment doubles for the complete production RestoratorImages translation
// unit. The strict bitmap/selection doubles of the VCL image suite are reused;
// this file adds only an export/trampoline model for two gdi32 detours, faults
// in the prepared copy and captured diagnostics. It is not Windows GDI or DWM,
// a real PE export, the Detours ABI or instruction patching.
#pragma once
#include "vcl_images_test_shim.h"

namespace RestoratorTest
{
    using BitFn = BOOL (WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
    using StretchFn = BOOL (WINAPI*)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
    inline TestModule gdi32;
    inline bool gdiAvailable = true, bitExport = true, stretchExport = true;
    inline int resolutions = 0;
    inline void* bitDetour{};
    inline void* stretchDetour{};
    inline int exportCalls = 0, trampolineCalls = 0;
    inline bool failBegin = false, failCommit = false;
    inline int failAttachAt = 0, failDetachAt = 0; // 1-based call index, 0 = never
    inline int writes = 0, attachCalls = 0, detachCalls = 0, attaches = 0, detaches = 0;
    struct Op { void** real{}; void* hook{}; bool attach{}; };
    inline std::vector<Op> pending;
    inline std::unordered_map<HWND, unsigned> items;
    inline int countQueries = 0;
    inline thread_local ULONGLONG clock = 100;
    inline thread_local std::vector<std::wstring> logFormats, logStrings;
    inline thread_local std::vector<size_t> logSizes; // every %zu argument, in order
    inline thread_local int dibQueries = 0;
    inline int copyFault = 0; // corrupts what the second DIBSECTION query of a blit (the prepared copy) reports
}

inline BOOL WINAPI RestoratorTrampolineBitBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    ++RestoratorTest::trampolineCalls;
    assert(RestoratorTest::bitDetour); // exists only while attached
    return BitBlt(dc, x, y, w, h, src, sx, sy, rop);
}
inline BOOL WINAPI RestoratorTrampolineStretchBlt(HDC dc, int x, int y, int w, int h,
    HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
    ++RestoratorTest::trampolineCalls;
    assert(RestoratorTest::stretchDetour);
    return StretchBlt(dc, x, y, w, h, src, sx, sy, sw, sh, rop);
}
// What an application reaches through gdi32: the installed detour, else native.
// Calling it again from a detour would re-enter that detour, not bypass it.
inline BOOL WINAPI RestoratorExportBitBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    ++RestoratorTest::exportCalls; RestoratorTest::dibQueries = 0;
    if (const auto hook = reinterpret_cast<RestoratorTest::BitFn>(RestoratorTest::bitDetour))
        return hook(dc, x, y, w, h, src, sx, sy, rop);
    return BitBlt(dc, x, y, w, h, src, sx, sy, rop);
}
inline BOOL WINAPI RestoratorExportStretchBlt(HDC dc, int x, int y, int w, int h,
    HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
    ++RestoratorTest::exportCalls; RestoratorTest::dibQueries = 0;
    if (const auto hook = reinterpret_cast<RestoratorTest::StretchFn>(RestoratorTest::stretchDetour))
        return hook(dc, x, y, w, h, src, sx, sy, sw, sh, rop);
    return StretchBlt(dc, x, y, w, h, src, sx, sy, sw, sh, rop);
}

namespace GlassCore::Hooks::RestoratorDetours
{
    // One transaction may carry both functions. Commit applies all operations or,
    // on an injected failure, none: pointers and installed detours stay untouched.
    inline void Attach(void** real, void* hook)
    {
        using namespace RestoratorTest;
        if (++attachCalls == failAttachAt) throw std::runtime_error("DetourAttach failed");
        assert(real && hook && (*real == reinterpret_cast<void*>(&RestoratorExportBitBlt)
            || *real == reinterpret_cast<void*>(&RestoratorExportStretchBlt)));
        pending.push_back({real, hook, true});
    }
    inline void Detach(void** real, void* hook)
    {
        using namespace RestoratorTest;
        if (++detachCalls == failDetachAt) throw std::runtime_error("DetourDetach failed");
        assert(real && hook && (*real == reinterpret_cast<void*>(&RestoratorTrampolineBitBlt)
            || *real == reinterpret_cast<void*>(&RestoratorTrampolineStretchBlt)));
        pending.push_back({real, hook, false});
    }
    inline HRESULT Write(const std::function<void()>& callback) noexcept
    {
        using namespace RestoratorTest;
        ++writes;
        if (failBegin) return E_FAIL;
        pending.clear();
        try { callback(); }
        catch (...) { pending.clear(); return E_FAIL; }
        const auto operations = std::exchange(pending, {});
        if (failCommit) return E_FAIL;
        for (const auto& op : operations)
        {
            const bool bit = *op.real == reinterpret_cast<void*>(&RestoratorExportBitBlt)
                || *op.real == reinterpret_cast<void*>(&RestoratorTrampolineBitBlt);
            void*& installed = bit ? bitDetour : stretchDetour;
            if (op.attach)
            {
                assert(!installed);
                installed = op.hook; ++attaches;
                *op.real = bit ? reinterpret_cast<void*>(&RestoratorTrampolineBitBlt)
                    : reinterpret_cast<void*>(&RestoratorTrampolineStretchBlt);
            }
            else
            {
                assert(installed == op.hook);
                installed = nullptr; ++detaches;
                *op.real = bit ? reinterpret_cast<void*>(&RestoratorExportBitBlt)
                    : reinterpret_cast<void*>(&RestoratorExportStretchBlt);
            }
        }
        return S_OK;
    }
}

inline HMODULE DetourGetContainingModule(void* address) { return static_cast<HMODULE>(address); }
inline UINT TreeView_GetCount(HWND w)
{
    ++RestoratorTest::countQueries;
    const auto it = RestoratorTest::items.find(w);
    return it == RestoratorTest::items.end() ? 0u : it->second;
}
inline HMODULE RestoratorGetModuleHandleW(LPCWSTR name)
{
    if (std::wstring_view(name) == L"gdi32.dll")
        return RestoratorTest::gdiAvailable ? &RestoratorTest::gdi32 : nullptr;
    return ImageGetModuleHandleW(name);
}
inline FARPROC RestoratorGetProcAddress(HMODULE module, LPCSTR name)
{
    if (module != &RestoratorTest::gdi32) return PanelGetProcAddress(module, name);
    ++RestoratorTest::resolutions;
    if (std::strcmp(name, "BitBlt") == 0)
        return RestoratorTest::bitExport ? reinterpret_cast<FARPROC>(&RestoratorExportBitBlt) : nullptr;
    if (std::strcmp(name, "StretchBlt") == 0)
        return RestoratorTest::stretchExport ? reinterpret_cast<FARPROC>(&RestoratorExportStretchBlt) : nullptr;
    return nullptr;
}
inline int RestoratorGetObjectW(HGDIOBJ object, int bytes, void* out)
{
    const bool copy = bytes == sizeof(DIBSECTION) && ++RestoratorTest::dibQueries == 2;
    const int result = ImageGetObjectW(object, bytes, out);
    if (!copy || !RestoratorTest::copyFault) return result;
    auto& info = static_cast<DIBSECTION*>(out)->dsBm;
    switch (RestoratorTest::copyFault)
    {
    case 1: return 0;
    case 2: return sizeof(BITMAP);
    case 3: info.bmBits = nullptr; break;
    case 4: info.bmBitsPixel = 24; break;
    case 5: info.bmPlanes = 2; break;
    case 6: ++info.bmWidth; break;
    case 7: --info.bmHeight; break;
    }
    return result;
}
inline ULONGLONG RestoratorTickCount64() { return RestoratorTest::clock; }
template <typename T> inline void RestoratorCaptureLogString(T value)
{
    if constexpr (std::is_convertible_v<T, LPCWSTR>)
    { if (value) RestoratorTest::logStrings.emplace_back(value); }
    else if constexpr (std::is_same_v<T, size_t>) RestoratorTest::logSizes.push_back(value);
}
template <typename... Args> inline void RestoratorDebugLog(int, LPCWSTR format, Args... args)
{
    RestoratorTest::logFormats.emplace_back(format);
    (RestoratorCaptureLogString(args), ...);
    SetLastError(0xd109); // a logger must not leak its error into the caller's result
}
#undef GetModuleHandleW
#define GetModuleHandleW RestoratorGetModuleHandleW
#undef GetProcAddress
#define GetProcAddress RestoratorGetProcAddress
#undef GetObject
#define GetObject RestoratorGetObjectW
#undef GetObjectW
#define GetObjectW RestoratorGetObjectW
#define GetTickCount64 RestoratorTickCount64
#define DebugLog RestoratorDebugLog
