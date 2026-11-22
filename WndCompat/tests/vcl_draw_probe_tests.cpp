// Compile the real header, dependency table, complete probe block and root
// enable function. Unrelated root hook bodies are link-only no-op doubles.
#include "vcl_draw_probe_test_shim.h"
#include <Hooks/RootGdiHooks.h>
using namespace GlassCore::Hooks;
using namespace std::literals;
namespace GlassCore::Root {
    [[maybe_unused]] static std::atomic<bool> g_shutdown{false};
#include "vcl_draw_probe_root_stubs.inc"
#include "vcl_draw_probe_production.inc"
}
#define CHECK(value) do { if (!(value)) throw std::runtime_error(std::string(__func__) + ": " + #value); } while (false)
using namespace GlassCore::Root;

#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
namespace {
    constexpr DWORD kEntryError = 0x9876;
    const std::array<std::string, 23> kNames{
        "FillRect", "PatBlt", "Rectangle", "RoundRect", "BitBlt", "ExtTextOutW", "DrawTextW",
        "DrawThemeBackground", "CreateSolidBrush", "SelectObject", "Ellipse", "DrawTextExW",
        "CreateBrushIndirect", "DrawThemeTextEx", "StretchBlt", "DrawTextA", "DrawTextExA",
        "ExtTextOutA", "TextOutA", "TextOutW", "TabbedTextOutA", "TabbedTextOutW", "DrawFocusRect"
    };
    size_t FindProbe(std::string_view name) {
        for (size_t i = OHK_VCLPROBE_FILL; i <= OHK_VCLPROBE_FOCUSRECT; ++i)
            if (name == std::get<1>(g_rootHookDep[i])) return i;
        return OHK_COUNT;
    }
    template<class Fn, class... Args> auto Import(const char* name, Fn native, Args... args) {
        const auto& entries = Mock::occupants[name];
        const auto fn = entries.empty() ? native : reinterpret_cast<Fn>(entries.front());
        return fn(args...);
    }
    template<class Fn, class... Args> auto Direct(const char* name, Fn, Args... args) {
        const size_t index = FindProbe(name); CHECK(index != OHK_COUNT);
        return reinterpret_cast<Fn>(std::get<2>(g_rootHookDep[index]))(args...);
    }
    size_t ApiLogs() {
        return std::count_if(Mock::logs.begin(), Mock::logs.end(), [](const auto& line) {
            return line.find(L"] api=") != std::wstring::npos;
        });
    }
    bool HasLog(std::wstring_view part) {
        return std::any_of(Mock::logs.begin(), Mock::logs.end(), [part](const auto& line) {
            return line.find(part) != std::wstring::npos;
        });
    }
    struct Context {
        Window page, other;
        DC dc, memory;
        RECT rect{7, 8, 117, 43};
        Context() {
            CHECK(g_vclProbeStack.empty()); g_rootMap.clear();
            Mock::available = {kNames.begin(), kNames.end()};
            Mock::registerFails.clear(); Mock::noOriginal.clear(); Mock::occupants.clear(); Mock::toggles.clear();
            Mock::nativeError = 67; Mock::intResult = 47; Mock::boolResult = 7; Mock::longResult = 0x7f00ee11;
            Mock::leaveError = Mock::mutateOutput = Mock::failClass = false;
            Mock::duringNative = {}; Mock::originalRoute = false;
            GlassCore::ModuleHandler::g_entryPointModule = &Mock::handle; g_shutdown = false;
            dc.owner = &page; memory.type = OBJ_MEMDC; Mock::dcs = {&dc, &memory};
            ResetTrace();
        }
        ~Context() {
            Mock::duringNative = {};
            // Drain the real matching/session algorithm even after an assertion fails.
            for (size_t count = 0; !g_vclProbeStack.empty() && count < 100; ++count) {
                const auto top = g_vclProbeStack.back(); EndVclDrawProbe(top.hwnd, top.paint);
            }
            g_vclProbeStack.clear(); g_rootMap.clear(); Mock::occupants.clear(); Mock::dcs.clear();
        }
        void Start(HWND window = nullptr, bool paint = true) { BeginVclDrawProbe(window ? window : &page, paint); }
        void ResetTrace() { Mock::calls.clear(); Mock::logs.clear(); SetLastError(kEntryError); }
        void One(std::string_view name, bool original = true) {
            CHECK(Mock::calls.size() == 1);
            const auto& call = Mock::calls[0];
            CHECK(call.api == name && call.original == original && call.entry == kEntryError);
            CHECK(GetLastError() == (Mock::leaveError ? kEntryError : Mock::nativeError));
        }
        void Refs(int expected) {
            for (size_t i = OHK_VCLPROBE_FILL; i <= OHK_VCLPROBE_FOCUSRECT; ++i)
                CHECK(g_rootMap.at(&Mock::handle).refs[i] == expected);
        }
    };
    void Slots() {
        Context c;
        CHECK(OHK_VCLPROBE_FOCUSRECT - OHK_VCLPROBE_FILL + 1 == kNames.size());
        for (size_t i = 0; i < kNames.size(); ++i)
            CHECK(std::get<1>(g_rootHookDep[OHK_VCLPROBE_FILL + i]) == kNames[i]);
        for (const char* name : {"ExtTextOutA", "TextOutA", "TextOutW"}) {
            const size_t index = FindProbe(name); CHECK(index != OHK_COUNT);
            const auto& dlls = std::get<0>(g_rootHookDep[index]);
            CHECK(dlls[0] == "gdi32.dll" && dlls[1] == "gdi32full.dll");
        }
        for (const char* name : {"DrawTextA", "DrawTextExA", "TabbedTextOutA", "TabbedTextOutW"})
            CHECK(std::get<0>(g_rootHookDep[FindProbe(name)])[0] == "user32.dll");
        c.Start(); c.Refs(1); EndVclDrawProbe(&c.page, true); c.Refs(0);
    }
    void Classes() {
        Context c;
        for (LPCWSTR name : {L"TPage", L"tpage", L"TPAGE", L"TBitBtn", L"TToggleSwitch", L"TListBox",
                            L"TCheckListBox", L"TColorListBox", L"TNumberBox", L"THotKey", L"TRichEdit"}) {
            c.page.cls = name; c.Start(); CHECK(g_vclProbeStack.size() == 1);
            CHECK(HasLog(L"channel=paint")); EndVclDrawProbe(&c.page, true); c.Refs(0); c.ResetTrace();
        }
        for (LPCWSTR name : {L"TPageControl", L"TPage2", L"MyTPage", L"TPanel", L"TNotebook", L"Button", L""}) {
            c.page.cls = name; c.Start(); CHECK(g_vclProbeStack.empty() && Mock::logs.empty());
        }
        c.page.cls = L"TPage"; Mock::failClass = true; c.Start(); CHECK(g_vclProbeStack.empty());
    }
    void BeginEndErrors() {
        Context c;
        c.Start(); CHECK(GetLastError() == kEntryError && g_vclProbeStack.size() == 1);
        EndVclDrawProbe(&c.page, true); CHECK(GetLastError() == kEntryError); c.Refs(0);
        EndVclDrawProbe(&c.page, false); CHECK(GetLastError() == kEntryError);
        c.Start(&c.page, false); CHECK(GetLastError() == kEntryError && HasLog(L"channel=getdc"));
        EndVclDrawProbe(&c.page, false); CHECK(GetLastError() == kEntryError); c.Refs(0);
        c.page.cls = L"TPageOther"; c.Start(); CHECK(GetLastError() == kEntryError && g_vclProbeStack.empty());
        c.page.cls = L"TPage"; GlassCore::ModuleHandler::g_entryPointModule = nullptr;
        c.Start(); CHECK(GetLastError() == kEntryError && g_vclProbeStack.empty());
        GlassCore::ModuleHandler::g_entryPointModule = &Mock::handle;
        g_shutdown = true; c.Start(); BeginVclDrawProbe(nullptr, true);
        CHECK(GetLastError() == kEntryError && g_vclProbeStack.empty()); g_shutdown = false;
    }
    void DrawAnsi() {
        Context c; c.Start();
        const char text[]{'a', '\0', static_cast<char>(0x81), static_cast<char>(0xfe)};
        for (HDC dc : {&c.dc, &c.memory}) for (int count : {4, 1, 0, -1, -9})
            for (UINT flags : {0u, 0x400u, 0x100014u, 0x10000u}) {
                c.ResetTrace(); CHECK(Import("DrawTextA", &::DrawTextA, dc, text, count, &c.rect, flags) == 47);
                c.One("DrawTextA"); const auto& call = Mock::calls[0];
                CHECK(call.dc == dc && call.text == text && call.count == count && call.rect == &c.rect && call.flags == flags);
                CHECK(HasLog(L"api=DrawTextA "));
            }
    }
    void DrawExAnsi() {
        Context c; c.Start();
        for (bool withParams : {false, true}) {
            char text[8]{'a', '\0', 'b', static_cast<char>(0xff)};
            DRAWTEXTPARAMS params{sizeof(DRAWTEXTPARAMS), 7, 2, 3, 0};
            Mock::mutateOutput = true; c.ResetTrace();
            auto* ptr = withParams ? &params : nullptr;
            CHECK(Import("DrawTextExA", &::DrawTextExA, &c.dc, text, 4, &c.rect, 0x10400u, ptr) == 47);
            c.One("DrawTextExA"); const auto& call = Mock::calls[0];
            CHECK(call.text == text && call.count == 4 && call.params == ptr && call.flags == 0x10400u);
            CHECK(call.rect == &c.rect && (c.rect == RECT{11, 12, 211, 35}) && text[0] == '!');
            CHECK(!ptr || (params.uiLengthDrawn == 29 && params.iTabLength == 7 && params.iLeftMargin == 2));
            CHECK(HasLog(L"api=DrawTextExA "));
        }
    }
    void ExtAnsi() {
        Context c; c.Start(); const INT dx[]{3, -2, 4, 1};
        const LPCSTR opaquePointer = reinterpret_cast<LPCSTR>(static_cast<std::uintptr_t>(0x11));
        for (UINT count : {0u, 4u, std::numeric_limits<UINT>::max()}) for (UINT flags : {0u, 6u, 0x2010u}) {
            c.ResetTrace();
            CHECK(Import("ExtTextOutA", &::ExtTextOutA, &c.memory, -17, 26, flags,
                static_cast<LPCRECT>(nullptr), opaquePointer, count, dx) == 7);
            c.One("ExtTextOutA"); const auto& call = Mock::calls[0];
            CHECK(call.dc == &c.memory && call.x == -17 && call.y == 26 && call.rect == nullptr);
            CHECK(call.text == opaquePointer && call.count == count && call.dx == dx && call.flags == flags);
            CHECK(HasLog(L"api=ExtTextOutA "));
            if (count == std::numeric_limits<UINT>::max()) CHECK(HasLog(L"c=4294967295"));
        }
    }
    void DirectText() {
        Context c; c.Start(); const char ansi[]{'x', static_cast<char>(0xff)}; const wchar_t wide[]{L'x', L'\0'};
        for (int count : {2, 0, -1}) {
            c.ResetTrace(); CHECK(Import("TextOutA", &::TextOutA, &c.dc, -8, 15, ansi, count) == 7);
            c.One("TextOutA"); CHECK(Mock::calls[0].text == ansi && Mock::calls[0].count == count);
            CHECK(Mock::calls[0].x == -8 && Mock::calls[0].y == 15 && HasLog(L"api=TextOutA "));
            c.ResetTrace(); CHECK(Import("TextOutW", &::TextOutW, &c.memory, 31, -4, wide, count) == 7);
            c.One("TextOutW"); CHECK(Mock::calls[0].text == wide && Mock::calls[0].count == count);
            CHECK(Mock::calls[0].x == 31 && Mock::calls[0].y == -4 && HasLog(L"api=TextOutW "));
        }
    }
    void Tabbed() {
        Context c; c.Start(); const INT stops[]{8, 17, 36}; const char ansi[]{'a', '\t', 'b'}; const wchar_t wide[]{L'a', L'\t', L'b'};
        for (int tabs : {0, 3, -1}) for (int count : {3, 0, -1}) {
            c.ResetTrace(); CHECK(Import("TabbedTextOutA", &::TabbedTextOutA, &c.dc, -8, 15, ansi, count, tabs, stops, -37) == Mock::longResult);
            c.One("TabbedTextOutA"); auto call = Mock::calls[0];
            CHECK(call.text == ansi && call.count == count && call.tabs == tabs && call.tabStops == stops && call.origin == -37);
            CHECK(call.x == -8 && call.y == 15 && HasLog(L"api=TabbedTextOutA "));
            c.ResetTrace(); CHECK(Import("TabbedTextOutW", &::TabbedTextOutW, &c.memory, 31, -4, wide, count, tabs, stops, 54) == Mock::longResult);
            c.One("TabbedTextOutW"); call = Mock::calls[0];
            CHECK(call.text == wide && call.count == count && call.tabs == tabs && call.tabStops == stops && call.origin == 54);
            CHECK(call.x == 31 && call.y == -4 && HasLog(L"api=TabbedTextOutW "));
        }
    }
    void AllTextOriginalsAndFallback() {
        Context c; c.Start(); char a[8]{'a'}; wchar_t w[8]{L'w'}; const INT dx[]{8, 12}; DRAWTEXTPARAMS params{};
        for (bool original : {true, false}) {
            Mock::noOriginal = original ? std::set<std::string>{} : Mock::available;
#define TEXT_CALL(api, expected, ...) do { c.ResetTrace(); CHECK(Direct(#api, &::api, __VA_ARGS__) == (expected)); c.One(#api, original); CHECK(HasLog(L"api=")); } while(false)
            TEXT_CALL(DrawTextA, 47, &c.dc, a, -1, &c.rect, 0u);
            TEXT_CALL(DrawTextW, 47, &c.dc, w, -1, &c.rect, 0u);
            TEXT_CALL(DrawTextExA, 47, &c.dc, a, 1, &c.rect, 0u, &params);
            TEXT_CALL(DrawTextExW, 47, &c.dc, w, 1, &c.rect, 0u, &params);
            TEXT_CALL(ExtTextOutA, 7, &c.dc, 2, 3, 6u, &c.rect, a, 1u, dx);
            TEXT_CALL(ExtTextOutW, 7, &c.dc, 2, 3, 6u, &c.rect, w, 1u, dx);
            TEXT_CALL(TextOutA, 7, &c.dc, 2, 3, a, 1);
            TEXT_CALL(TextOutW, 7, &c.dc, 2, 3, w, 1);
            TEXT_CALL(TabbedTextOutA, Mock::longResult, &c.dc, 2, 3, a, 1, 2, dx, 4);
            TEXT_CALL(TabbedTextOutW, Mock::longResult, &c.dc, 2, 3, w, 1, 2, dx, 4);
#undef TEXT_CALL
        }
    }
    void LegacyGraphics() {
        Context c; c.Start();
        for (bool original : {true, false}) {
            Mock::noOriginal = original ? std::set<std::string>{} : Mock::available;
#define GRAPHICS_CALL(api, expected, ...) do { c.ResetTrace(); CHECK(Direct(#api, &::api, __VA_ARGS__) == (expected)); c.One(#api, original); } while(false)
            GRAPHICS_CALL(FillRect, 47, &c.dc, &c.rect, &Mock::brush);
            GRAPHICS_CALL(PatBlt, 7, &c.dc, 1, 2, 13, 17, 0x42u);
            GRAPHICS_CALL(Rectangle, 7, &c.dc, 1, 2, 13, 17);
            GRAPHICS_CALL(RoundRect, 7, &c.dc, 1, 2, 13, 17, 3, 4);
            GRAPHICS_CALL(Ellipse, 7, &c.dc, 1, 2, 13, 17);
            GRAPHICS_CALL(BitBlt, 7, &c.dc, 1, 2, 13, 17, &c.memory, 5, 6, 0xcc0020u);
            GRAPHICS_CALL(StretchBlt, 7, &c.dc, 1, 2, 13, 17, &c.memory, 5, 6, 7, 8, 0xcc0020u);
            GRAPHICS_CALL(DrawFocusRect, 7, &c.dc, &c.rect);
            GRAPHICS_CALL(DrawThemeBackground, Mock::hrResult, static_cast<HTHEME>(&Mock::handle), &c.dc, 1, 2, &c.rect, &c.rect);
            GRAPHICS_CALL(DrawThemeTextEx, Mock::hrResult, static_cast<HTHEME>(&Mock::handle), &c.dc, 1, 2, L"x", -1, 0u, &c.rect, static_cast<const DTTOPTS*>(nullptr));
            GRAPHICS_CALL(CreateSolidBrush, static_cast<HBRUSH>(&Mock::brush), 0x443322u);
            GRAPHICS_CALL(CreateBrushIndirect, static_cast<HBRUSH>(&Mock::brush), &Mock::brush);
            GRAPHICS_CALL(SelectObject, static_cast<HGDIOBJ>(&Mock::handle), &c.dc, &Mock::brush);
#undef GRAPHICS_CALL
        }
    }
    void Nested() {
        Context c; c.Start(); c.Refs(1); c.Start(&c.other, false); c.Refs(2); c.Start(); c.Refs(3);
        CHECK(g_vclProbeStack.size() == 3);
        // Match by HWND + channel, including a non-top matching scope.
        EndVclDrawProbe(&c.page, false); c.Refs(3);
        EndVclDrawProbe(&c.other, false); c.Refs(2); CHECK(g_vclProbeStack.size() == 2);
        EndVclDrawProbe(&c.page, true); c.Refs(1); EndVclDrawProbe(&c.page, true); c.Refs(0);
        CHECK(g_vclProbeStack.empty());
        for (const auto& [name, entries] : Mock::occupants) { (void)name; CHECK(entries.empty()); }
    }
    void PartialAcquisition() {
        Context c; Mock::available = {"DrawTextA", "ExtTextOutA", "DrawTextW"}; Mock::registerFails = {"ExtTextOutA"};
        const size_t ansi = FindProbe("DrawTextA"); CHECK(ansi != OHK_COUNT);
        CHECK(EnableRootHook(&Mock::handle, ansi, true));
        c.Start(); c.Start(&c.other, false);
        CHECK(g_rootMap.at(&Mock::handle).refs[ansi] == 3);
        EndVclDrawProbe(&c.page, true); EndVclDrawProbe(&c.other, false);
        CHECK(g_rootMap.at(&Mock::handle).refs[ansi] == 1);
        for (const auto& toggle : Mock::toggles) if (!toggle.enable) CHECK(toggle.before > 0);
        CHECK(EnableRootHook(&Mock::handle, ansi, false));
        for (const auto& [name, entries] : Mock::occupants) { (void)name; CHECK(entries.empty()); }
    }
    void Budget() {
        Context c; c.Start(); c.ResetTrace();
        for (int i = 0; i < 173; ++i) {
            SetLastError(kEntryError); CHECK(Import("DrawTextA", &::DrawTextA, &c.dc, "x", 1, &c.rect, 0u) == 47);
            CHECK(GetLastError() == 67 && Mock::calls.back().entry == kEntryError);
        }
        CHECK(Mock::calls.size() == 173 && ApiLogs() == 160);
        Mock::clock += 1000; SetLastError(kEntryError);
        Import("DrawTextA", &::DrawTextA, &c.dc, "x", 1, &c.rect, 0u); CHECK(ApiLogs() == 161);
    }
    void ThreadIsolation() {
        Context c; c.Start(); c.ResetTrace(); bool okay = false;
        std::thread worker([&]() {
            SetLastError(kEntryError);
            Import("DrawTextA", &::DrawTextA, &c.dc, "x", 1, &c.rect, 0u);
            okay = Mock::calls.size() == 1 && Mock::calls[0].original && Mock::calls[0].entry == kEntryError
                && Mock::logs.empty() && GetLastError() == 67 && g_vclProbeStack.empty();
            BeginVclDrawProbe(&c.other, false);
            okay = okay && g_vclProbeStack.size() == 1;
            EndVclDrawProbe(&c.other, false);
        });
        worker.join(); CHECK(okay && g_vclProbeStack.size() == 1); c.Refs(1);
        CHECK(Mock::logs.empty() && Mock::calls.empty());
    }
    void NativeFailureAndUnsetError() {
        Context c; c.Start(); Mock::intResult = 0; Mock::boolResult = FALSE; Mock::longResult = 0;
        for (bool leave : {false, true}) {
            Mock::leaveError = leave;
            c.ResetTrace(); CHECK(Import("DrawTextA", &::DrawTextA, &c.dc, "x", 1, &c.rect, 0u) == 0); c.One("DrawTextA");
            c.ResetTrace(); CHECK(Import("ExtTextOutA", &::ExtTextOutA, &c.dc, 2, 3, 0u, &c.rect, "x", 1u, static_cast<const INT*>(nullptr)) == FALSE); c.One("ExtTextOutA");
            c.ResetTrace(); CHECK(Import("TextOutA", &::TextOutA, &c.dc, 2, 3, "x", 1) == FALSE); c.One("TextOutA");
            c.ResetTrace(); CHECK(Import("TabbedTextOutA", &::TabbedTextOutA, &c.dc, 2, 3, "x", 1, 0, static_cast<const INT*>(nullptr), 0) == 0); c.One("TabbedTextOutA");
        }
    }
    void ShutdownAndEmptyContext() {
        Context c; c.Start(); g_shutdown = true; c.ResetTrace();
        CHECK(Import("DrawTextA", &::DrawTextA, &c.dc, "x", 1, &c.rect, 0u) == 47); c.One("DrawTextA");
        CHECK(Mock::logs.empty()); EndVclDrawProbe(&c.page, true); c.Refs(0); g_shutdown = false;
        c.ResetTrace(); Direct("DrawTextA", &::DrawTextA, &c.dc, "x", 1, &c.rect, 0u); c.One("DrawTextA", false);
        CHECK(Mock::logs.empty());
    }
    void PrivacyAndState() {
        Context c; c.Start(); const auto dc = c.dc, memory = c.memory;
        for (HDC target : {&c.dc, &c.memory, static_cast<HDC>(nullptr)}) {
            c.ResetTrace();
            Import("DrawTextA", &::DrawTextA, target, "private ANSI contents", -1, static_cast<LPRECT>(nullptr), 0u);
            c.One("DrawTextA"); CHECK(!HasLog(L"private ANSI contents"));
            CHECK(c.dc.clip == dc.clip && c.dc.pixels == dc.pixels && c.memory.clip == memory.clip && c.memory.pixels == memory.pixels);
            CHECK(c.dc.text == dc.text && c.dc.background == dc.background && c.dc.mode == dc.mode);
            CHECK(HasLog(L"api=DrawTextA ") && HasLog(L"a=-1"));
        }
        c.ResetTrace(); Import("DrawTextA", &::DrawTextA, &c.memory, "x", 1, &c.rect, 0u);
        CHECK(HasLog(L"textColor=0x00102030") && HasLog(L"bkColor=0x00A0B0C0") && HasLog(L"bkMode=2"));
    }
    int WINAPI ForeignDrawTextA(HDC dc, LPCSTR text, int count, LPRECT r, UINT flags) {
        Record({"foreign DrawTextA", dc, text, count, r, flags}); return 91;
    }
    void FirstOccupant() {
        Context c; Mock::occupants["DrawTextA"].push_back(reinterpret_cast<void*>(ForeignDrawTextA));
        c.Start(); c.ResetTrace();
        CHECK(Import("DrawTextA", &::DrawTextA, &c.dc, "x", 1, &c.rect, 0u) == 91);
        CHECK(Mock::calls.size() == 1 && Mock::calls[0].api == "foreign DrawTextA" && Mock::logs.empty());
        auto& entries = Mock::occupants["DrawTextA"]; entries.erase(entries.begin());
        c.ResetTrace(); Import("DrawTextA", &::DrawTextA, &c.dc, "x", 1, &c.rect, 0u); c.One("DrawTextA");
        CHECK(HasLog(L"api=DrawTextA "));
    }
}
#endif
int main(int argc, char** argv) {
    try {
        CHECK(argc == 2); const std::string name = argv[1];
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        if (name == "slots") Slots(); else if (name == "classes") Classes();
        else if (name == "session-errors") BeginEndErrors(); else if (name == "draw-ansi") DrawAnsi();
        else if (name == "drawex-ansi") DrawExAnsi(); else if (name == "ext-ansi") ExtAnsi();
        else if (name == "direct-text") DirectText(); else if (name == "tabbed") Tabbed();
        else if (name == "original-fallback") AllTextOriginalsAndFallback(); else if (name == "legacy") LegacyGraphics();
        else if (name == "nested") Nested(); else if (name == "partial") PartialAcquisition();
        else if (name == "budget") Budget(); else if (name == "thread") ThreadIsolation();
        else if (name == "native-failures") NativeFailureAndUnsetError(); else if (name == "shutdown") ShutdownAndEmptyContext();
        else if (name == "privacy-state") PrivacyAndState(); else if (name == "occupant") FirstOccupant();
        else throw std::runtime_error("unknown case");
#else
        CHECK(name == "disabled"); CHECK(OHK_COUNT == 30 && g_rootHookDep.size() == 30);
#endif
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
