// No replacement implementation of DDH: include its complete production unit.
#include "debug_detour_hooks_test_shim.h"
#include "../../GlassCore/src/Core/Intercept/DebugDetourHooks.cpp"
#include <Core/Intercept/DebugDetourHooks.h>
#include <thread>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
using namespace GlassCore::Hooks::DebugDetourHooks;
namespace
{
    constexpr std::array<uint64_t, 40> kMasks{
        DDH_DrawThemeBackground, DDH_DrawThemeText, DDH_DrawThemeTextEx,
        DDH_DrawTextW, DDH_DrawTextExW, DDH_ExtTextOutW, DDH_BitBlt, DDH_AnimateWindow,
        DDH_TextOutW, DDH_PatBlt, DDH_PaintBackground, DDH_DwmExtendFrameIntoClientArea,
        DDH_GetThemeColor, DDH_DwmSetWindowAttribute, DDH_SetClassLongPtrW,
        DDH_SetDCBrushColor, DDH_SetBkColor, DDH_SetDIBits, DDH_CreateSolidBrush,
        DDH_GetSysColorBrush, DDH_GetStockObject, DDH_GetSysColor,
        DDH_DrawThemeBackgroundEx, DDH_DrawTextWithGlow, DDH_SetSystemVisualStyle,
        DDH_DwmEnableBlurBehindWindow, DDH_SetWindowCompositionAttribute,
        DDH_Rectangle, DDH_FillRect, DDH_StretchBlt, DDH_BeginBufferedPaint,
        DDH_EndBufferedPaint, DDH_BufferedPaintClear, DDH_BufferedPaintSetAlpha,
        DDH_DrawTextA, DDH_DrawTextExA, DDH_ExtTextOutA, DDH_TextOutA,
        DDH_TabbedTextOutA, DDH_TabbedTextOutW};
    constexpr uint64_t kNew = ((1ull << 40) - 1) ^ ((1ull << 34) - 1);
    static_assert(DDH_All == (1ull << 40) - 1);
    static_assert(DDH_TextOutW == 1ull << 8);
    static_assert(DDH_ComboLBoxPaintProbe == ((1ull << 10) - 1) - (1ull << 7));

#if defined(_DEBUG)
    const std::array<void**, 6> kOriginals{
        reinterpret_cast<void**>(&g_trueDrawTextA), reinterpret_cast<void**>(&g_trueDrawTextExA),
        reinterpret_cast<void**>(&g_trueExtTextOutA), reinterpret_cast<void**>(&g_trueTextOutA),
        reinterpret_cast<void**>(&g_trueTabbedTextOutA), reinterpret_cast<void**>(&g_trueTabbedTextOutW)};
    const std::array<void*, 6> kHandlers{
        reinterpret_cast<void*>(&HookDrawTextA), reinterpret_cast<void*>(&HookDrawTextExA),
        reinterpret_cast<void*>(&HookExtTextOutA), reinterpret_cast<void*>(&HookTextOutA),
        reinterpret_cast<void*>(&HookTabbedTextOutA), reinterpret_cast<void*>(&HookTabbedTextOutW)};
    void ClearLogs() { DdhTest::logs.clear(); DdhTest::loggerCalls = 0; }
    void NoRefs()
    {
        for (size_t ref : g_ref) CHECK(ref == 0);
        CHECK(DdhTest::attached.empty());
        for (size_t i = 0; i < 6; ++i) CHECK(!DdhTest::hooks[i] && *kOriginals[i] == DdhTest::exports[i]);
    }
    void AllSlots()
    {
        CHECK(kSlotCount == 40 && std::size(g_ref) == 40);
        for (size_t i = 0; i < kMasks.size(); ++i) CHECK(kMasks[i] == 1ull << i);
        CHECK(SetEnabled(DDH_All, true) == S_OK);
        CHECK(DdhTest::committed.size() == 40 && DdhTest::attached.size() == 40);
        for (size_t i = 0; i < kSlotCount; ++i) CHECK(g_ref[i] == 1);
        for (size_t i = 0; i < 6; ++i)
        {
            const auto& op = DdhTest::committed.at(34+i);
            CHECK(op.enable && op.real == kOriginals[i] && op.hook == kHandlers[i]);
            CHECK(*kOriginals[i] == DdhTest::trampolines[i] && DdhTest::hooks[i] == kHandlers[i]);
        }
        CHECK(SetEnabled(1ull << 63, true) == S_OK && DdhTest::attached.size() == 40);
        CHECK(SetEnabled(DDH_All, false) == S_OK);
        CHECK(DdhTest::committed.size() == 80); NoRefs();
        for (size_t i = 0; i < 6; ++i)
        {
            const auto& op = DdhTest::committed.at(40+34+i);
            CHECK(!op.enable && op.real == kOriginals[i] && op.hook == kHandlers[i]);
        }
    }
    void RefsAndMixedMasks()
    {
        CHECK(SetEnabled(kNew, false) == S_OK); NoRefs();
        CHECK(SetEnabled(kNew, true) == S_OK && SetEnabled(kNew, true) == S_OK);
        CHECK(DdhTest::committed.size() == 6);
        CHECK(SetEnabled(DDH_DrawTextA, false) == S_OK);
        CHECK(g_ref[34] == 1 && g_ref[35] == 2 && DdhTest::committed.size() == 6);
        CHECK(SetEnabled(kNew, false) == S_OK);
        CHECK(g_ref[34] == 0 && g_ref[35] == 1 && DdhTest::committed.size() == 7);
        CHECK(SetEnabled(kNew & ~DDH_DrawTextA, false) == S_OK); NoRefs();
        CHECK(DdhTest::committed.size() == 12);
        CHECK(SetEnabled(DDH_TextOutW | DDH_DrawTextA | DDH_TabbedTextOutW, true) == S_OK);
        CHECK(g_ref[8] == 1 && g_ref[34] == 1 && g_ref[39] == 1 && g_ref[37] == 0);
        CHECK(DdhTest::attached.at(reinterpret_cast<void**>(&g_trueTextOutW)) == reinterpret_cast<void*>(&HookTextOutW));
        CHECK(SetEnabled(DDH_TextOutW, false) == S_OK && g_ref[8] == 0 && g_ref[34] == 1);
        CHECK(SetEnabled(kNew, false) == S_OK); NoRefs();
    }
    void FailedTransactions()
    {
        for (int phase : {0, 1, 2})
        {
            DdhTest::failBegin = phase == 0; DdhTest::failOperation = phase == 1 ? 3 : -1;
            DdhTest::failCommit = phase == 2;
            CHECK(FAILED(SetEnabled(kNew, true))); NoRefs();
            DdhTest::failBegin = DdhTest::failCommit = false; DdhTest::failOperation = -1;
            CHECK(SetEnabled(kNew, true) == S_OK && DdhTest::attached.size() == 6);
            DdhTest::failBegin = phase == 0; DdhTest::failOperation = phase == 1 ? 3 : -1;
            DdhTest::failCommit = phase == 2;
            CHECK(FAILED(SetEnabled(kNew, false)));
            for (size_t i = 0; i < 6; ++i)
            {
                CHECK(g_ref[34+i] == 1 && *kOriginals[i] == DdhTest::trampolines[i]);
                CHECK(DdhTest::hooks[i] == kHandlers[i]);
            }
            DdhTest::failBegin = DdhTest::failCommit = false; DdhTest::failOperation = -1;
            CHECK(SetEnabled(kNew, false) == S_OK); NoRefs();
        }
    }
    void MissingOriginal()
    {
        for (size_t missing = 0; missing < 6; ++missing)
        {
            *kOriginals[missing] = nullptr;
            CHECK(FAILED(SetEnabled(kNew, true)));
            for (size_t ref : g_ref) CHECK(ref == 0);
            CHECK(DdhTest::attached.empty());
            *kOriginals[missing] = DdhTest::exports[missing];
            CHECK(SetEnabled(kNew, true) == S_OK);
            CHECK(SetEnabled(kNew, false) == S_OK); NoRefs();
        }
    }
    LONG Invoke(size_t api, HDC dc, const void* text, std::int64_t count,
        RECT* rect, UINT flags, const void* extra)
    {
        const auto a = static_cast<LPCSTR>(text);
        const auto w = static_cast<LPCWSTR>(text);
        switch (api)
        {
        case 0: return ::DrawTextA(dc, a, int(count), rect, flags);
        case 1: return ::DrawTextExA(dc, const_cast<char*>(a), int(count), rect, flags,
            static_cast<LPDRAWTEXTPARAMS>(const_cast<void*>(extra)));
        case 2: return ::ExtTextOutA(dc, -19, 31, flags, rect, a, UINT(count), static_cast<const INT*>(extra));
        case 3: return ::TextOutA(dc, -19, 31, a, int(count));
        case 4: return ::TabbedTextOutA(dc, -19, 31, a, int(count), -3, static_cast<const INT*>(extra), -911);
        case 5: return ::TabbedTextOutW(dc, -19, 31, w, int(count), -3, static_cast<const INT*>(extra), -911);
        default: throw std::runtime_error("unknown API");
        }
    }
    void Passthrough(size_t api)
    {
        CHECK(SetEnabled(kMasks[34+api], true) == S_OK); ClearLogs();
        TestWindow window; window.cls = L"TPanel"; TestDC dc; dc.owner = &window;
        dc.text = 0x123456; dc.background = 0xabcdef; TestFont font; LOGBRUSH brush;
        dc.font = &font; dc.brush = &brush;
        char a[]{'A', '&', 'B'}; const wchar_t w[]{L'A', L'\t', L'B'};
        RECT rect{2,3,40,20}; const RECT before = rect;
        const int extra[]{4,8}; const void* text = api == 5 ? static_cast<const void*>(w) : a;
        int total = 0;
        for (std::int64_t count : std::array<std::int64_t, 6>{INT32_MIN, -2, -1, 0, 3, INT32_MAX})
        {
            if (api == 2 && count < 0) count = UINT32_MAX;
            for (bool leaveError : {false,true})
            {
                DdhTest::nativeLeavesError = leaveError; SetLastError(0x1234);
                const UINT flags = api <= 2 ? 0x10000410u : 0;
                CHECK(Invoke(api, &dc, text, count, &rect, flags, extra) == DdhTest::results[api]);
                CHECK(DdhTest::nativeCalls[api] == ++total);
                CHECK(GetLastError() == (leaveError ? 0x1234u : DdhTest::nativeError));
                const auto& c = DdhTest::calls[api];
                CHECK(c.dc == &dc && c.text == text && c.count == count && c.entryError == 0x1234);
                CHECK(c.flags == flags && c.rect == (api <= 2 ? &rect : nullptr));
                CHECK(c.extra == (api == 1 || api == 2 || api >= 4 ? static_cast<const void*>(extra) : nullptr));
                CHECK(c.x == (api >= 2 ? -19 : 0) && c.y == (api >= 2 ? 31 : 0));
                CHECK(c.tabCount == (api >= 4 ? -3 : 0) && c.tabOrigin == (api >= 4 ? -911 : 0));
                CHECK(dc.text == 0x123456 && dc.background == 0xabcdef && dc.bkMode == OPAQUE);
                CHECK(dc.font == &font && dc.brush == &brush && dc.saved.empty());
                CHECK(std::memcmp(&rect, &before, sizeof(rect)) == 0 && a[0] == 'A');
            }
        }
        for (size_t i = 0; i < 6; ++i) CHECK(DdhTest::nativeCalls[i] == (i == api ? total : 0));
        CHECK(DdhTest::logs.size() == size_t(total));
        CHECK(SetEnabled(kMasks[34+api], false) == S_OK); NoRefs();
    }
    void NoTextOrArrayReads()
    {
        CHECK(SetEnabled(kNew, true) == S_OK); ClearLogs();
        const void* unreadable = reinterpret_cast<const void*>(std::uintptr_t(1));
        auto* rect = reinterpret_cast<RECT*>(std::uintptr_t(2));
        for (size_t i = 0; i < 6; ++i)
        {
            SetLastError(0x4567);
            const auto count = i == 2 ? std::int64_t(UINT32_MAX) : -1;
            CHECK(Invoke(i, nullptr, unreadable, count, rect, 0x10000400, unreadable) == DdhTest::results[i]);
            CHECK(DdhTest::nativeCalls[i] == 1 && DdhTest::calls[i].entryError == 0x4567);
            CHECK(Invoke(i, nullptr, nullptr, 0, nullptr, 0, nullptr) == DdhTest::results[i]);
            CHECK(DdhTest::nativeCalls[i] == 2);
        }
        CHECK(DdhTest::logs.size() == 12);
        CHECK(SetEnabled(kNew, false) == S_OK); NoRefs();
    }
    void NativeMutations()
    {
        CHECK(SetEnabled(DDH_DrawTextA | DDH_DrawTextExA, true) == S_OK);
        DdhTest::mutateNative = true;
        for (size_t i : {0u, 1u})
        {
            char text[16] = "Panel"; RECT rect{2,3,40,20}; DRAWTEXTPARAMS params{17};
            CHECK(Invoke(i, nullptr, text, -1, &rect, DT_CALCRECT | DT_MODIFYSTRING, &params) == DdhTest::results[i]);
            CHECK(text[0] == 'X' && rect.right == 47 && DdhTest::nativeCalls[i] == 1);
            CHECK(params.cbSize == 17 + i);
        }
        CHECK(SetEnabled(DDH_DrawTextA | DDH_DrawTextExA, false) == S_OK); NoRefs();
    }
    void Metadata()
    {
        CHECK(SetEnabled(kNew, true) == S_OK); ClearLogs();
        TestWindow window; window.cls = L"TPanel"; TestDC dc; dc.owner = &window;
        dc.text = 0x123456; dc.background = 0xabcdef;
        for (size_t i = 0; i < 6; ++i)
            CHECK(Invoke(i, &dc, i == 5 ? static_cast<const void*>(L"Do not capture") : "Do not capture", 14,
                nullptr, 0x21, nullptr) == DdhTest::results[i]);
        CHECK(DdhTest::logs.size() == 6);
        for (const auto& log : DdhTest::logs)
        {
            for (LPCWSTR field : {L"[GlassCore][DebugDetour]", L"caller=Host.exe", L"hdc=", L"count=14",
                L"flags=0x", L"textColor=0x00123456", L"bkColor=0x00ABCDEF", L"bkMode=2", L"cls=\"TPanel\""})
                CHECK(log.find(field) != log.npos);
            CHECK(log.find(L"Do not capture") == log.npos && log.find(L"text=\"") == log.npos);
        }
        dc.owner = nullptr; ClearLogs();
        CHECK(::TextOutA(&dc, 0, 0, nullptr, 0) == DdhTest::results[3]);
        CHECK(DdhTest::logs.at(0).find(L"hdcWin=-") != std::wstring::npos);
        CHECK(SetEnabled(kNew, false) == S_OK); NoRefs();
    }
    void ReentrantLogger()
    {
        CHECK(SetEnabled(DDH_DrawTextA | DDH_TextOutA, true) == S_OK); ClearLogs();
        DdhTest::duringLog = [] { CHECK(::TextOutA(nullptr, -1, 2, nullptr, -1) == DdhTest::results[3]); };
        SetLastError(0x1234);
        CHECK(::DrawTextA(nullptr, nullptr, -1, nullptr, 0) == DdhTest::results[0]);
        CHECK(DdhTest::loggerCalls == 1 && DdhTest::nativeCalls[0] == 1 && DdhTest::nativeCalls[3] == 1);
        CHECK(DdhTest::calls[0].entryError == 0x1234 && GetLastError() == DdhTest::nativeError && !g_inTextMetadata);
        DdhTest::duringLog = {};
        CHECK(SetEnabled(DDH_DrawTextA | DDH_TextOutA, false) == S_OK); NoRefs();
    }
    void OtherThreadLogger()
    {
        CHECK(SetEnabled(DDH_DrawTextA | DDH_TextOutA, true) == S_OK); ClearLogs();
        const auto owner = std::this_thread::get_id(); bool passed = false;
        DdhTest::duringLog = [&] {
            if (std::this_thread::get_id() != owner) return;
            std::thread other([&] {
                SetLastError(0x5678);
                passed = ::TextOutA(nullptr, 7, 9, nullptr, -1) == DdhTest::results[3]
                    && DdhTest::calls[3].entryError == 0x5678 && GetLastError() == DdhTest::nativeError;
            });
            other.join();
        };
        SetLastError(0x1234);
        CHECK(::DrawTextA(nullptr, nullptr, -1, nullptr, 0) == DdhTest::results[0]);
        CHECK(passed && DdhTest::loggerCalls == 2 && DdhTest::logs.size() == 2);
        CHECK(DdhTest::nativeCalls[0] == 1 && DdhTest::nativeCalls[3] == 1 && !g_inTextMetadata);
        CHECK(DdhTest::calls[0].entryError == 0x1234 && GetLastError() == DdhTest::nativeError);
        DdhTest::duringLog = {};
        CHECK(SetEnabled(DDH_DrawTextA | DDH_TextOutA, false) == S_OK); NoRefs();
    }
    void FailedLogger()
    {
        CHECK(SetEnabled(DDH_DrawTextA, true) == S_OK); ClearLogs(); DdhTest::throwLog = true;
        SetLastError(0x1234);
        CHECK(::DrawTextA(nullptr, nullptr, -1, nullptr, 0) == DdhTest::results[0]);
        CHECK(DdhTest::nativeCalls[0] == 1 && DdhTest::calls[0].entryError == 0x1234 && !g_inTextMetadata);
        DdhTest::throwLog = false;
        CHECK(::DrawTextA(nullptr, nullptr, -1, nullptr, 0) == DdhTest::results[0]);
        CHECK(DdhTest::nativeCalls[0] == 2 && DdhTest::logs.size() == 1);
        CHECK(SetEnabled(DDH_DrawTextA, false) == S_OK); NoRefs();
    }
    void CallerFilter()
    {
        CHECK(SetEnabled(DDH_DrawTextA, true) == S_OK); ClearLogs();
        DdhTest::returnAddress = DdhTest::coreAddress; SetLastError(0x1234);
        CHECK(::DrawTextA(nullptr, nullptr, -1, nullptr, 0) == DdhTest::results[0]);
        CHECK(DdhTest::logs.empty() && DdhTest::calls[0].entryError == 0x1234);
        DdhTest::returnAddress = DdhTest::externalAddress; DdhTest::unknownCaller = true; DdhTest::unknownStack = true;
        CHECK(::DrawTextA(nullptr, nullptr, -1, nullptr, 0) == DdhTest::results[0]);
        CHECK(DdhTest::logs.at(0).find(L"caller=?") != std::wstring::npos);
        CHECK(DdhTest::logs.at(0).find(L"textColor=0xFFFFFFFF") != std::wstring::npos);
        CHECK(DdhTest::nativeCalls[0] == 2);
        CHECK(SetEnabled(DDH_DrawTextA, false) == S_OK); NoRefs();
    }
#endif
    void ReleaseNoop()
    {
        for (size_t i = 0; i < kMasks.size(); ++i) CHECK(kMasks[i] == 1ull << i);
        CHECK(SetEnabled(DDH_All, true) == S_OK && SetEnabled(DDH_All, false) == S_OK);
#if !defined(_DEBUG)
        CHECK(DdhTest::writes == 0 && DdhTest::attached.empty() && DdhTest::logs.empty());
#endif
    }
}
int main(int argc, char** argv)
{
    try
    {
        CHECK(argc == 2); const std::string name = argv[1];
        if (name == "release") ReleaseNoop();
#if defined(_DEBUG)
        else if (name == "slots") AllSlots();
        else if (name == "refs") RefsAndMixedMasks();
        else if (name == "transactions") FailedTransactions();
        else if (name == "missing-original") MissingOriginal();
        else if (name == "no-reads") NoTextOrArrayReads();
        else if (name == "mutations") NativeMutations();
        else if (name == "metadata") Metadata();
        else if (name == "reentry") ReentrantLogger();
        else if (name == "thread-logger") OtherThreadLogger();
        else if (name == "logger-failure") FailedLogger();
        else if (name == "caller") CallerFilter();
        else if (name.size() == 4 && name.starts_with("api")) Passthrough(size_t(name[3] - '0'));
#endif
        else throw std::runtime_error("unknown case");
        std::cout << name << ": OK\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << argv[1] << ": " << error.what() << '\n'; return 1; }
}
