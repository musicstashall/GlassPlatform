// Execute the production CoolBar rendering/session code against a small Win32
// shim. No duplicate implementation of the routing logic lives in this test.
#include "coolbar_test_shim.h"
#include "../../GlassCore/src/Hooks/delphi/TCoolBarDraw.cpp"
#include "../Controls/TCoolBarSubclass.cpp"
#include "../Providers/CoolBarEventProvider.cpp"
#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

namespace
{
    using namespace GlassCore::CoolBarControl;
    constexpr size_t New = 0, Legacy = 1, Exe = 2, Ux = 3;

    struct Context
    {
        std::array<TestModule, 4> modules;
        TestWindow window, parent;
        TestDC dc, mem, foreign;
        LOGBRUSH white{BS_SOLID, RGB(255, 255, 255)};
        TestFont font{1};
        RECT textRect{11, 4, 80, 20};
        wchar_t text[16] = L"TCoolBar";

        Context()
        {
            for (auto& m : modules)
                m.originals = {
                    {"ExtTextOutW", reinterpret_cast<void*>(&::ExtTextOutW)},
                    {"FillRect", reinterpret_cast<void*>(&::FillRect)},
                    {"PatBlt", reinterpret_cast<void*>(&::PatBlt)},
                    {"DrawTextW", reinterpret_cast<void*>(&::DrawTextW)},
                    {"DrawTextExW", reinterpret_cast<void*>(&::DrawTextExW)},
                    {"DrawThemeText", reinterpret_cast<void*>(&::DrawThemeText)},
                };
            GlassCore::ModuleHandler::g_comctl32New = &modules[New];
            GlassCore::ModuleHandler::g_comctl32Legacy = &modules[Legacy];
            GlassCore::ModuleHandler::g_entryPointModule = &modules[Exe];
            Test::ux = &modules[Ux];
            dc.owner = &window; dc.brush = &white; dc.font = &font;
            mem.brush = &white; mem.font = &font;
            foreign.owner = &parent; foreign.brush = &white;
            Test::topWindow = Test::host = &window;
        }
        ~Context()
        {
            Test::duringNativePaint = {}; Test::duringAlpha = {};
            WndCompat::CoolBar_DetachAll();
            WndCompat::CoolBar::SetCoolBarEventProvider({});
            Shutdown();
        }
        void CheckRefs(int expected)
        {
            for (auto& m : modules)
                for (const auto& [name, ref] : m.refs)
                { (void)name; CHECK(ref == expected); }
        }
        void Start() { Startup(); }
        void Begin(HDC supplied = nullptr)
        {
            WndCompat::CoolBar_Attach(&window);
            CHECK(OnPaintBegin(&window, supplied));
        }
    };

    template <typename Fn, typename... Args>
    auto Call(TestModule& module, const char* name, Fn, Args... args)
    {
        CHECK(module.hooks.count(name));
        return reinterpret_cast<Fn>(module.hooks.at(name))(args...);
    }
    int WINAPI LegacyDraw(HDC, LPCWSTR, int, LPRECT, UINT)
    { ++Test::nativeText; return 102; }

    void ExactClassAttachAndCatchUp()
    {
        Context c; c.Start();
        CHECK(!WndCompat::CoolBar_IsClassName(nullptr));
        for (const wchar_t* name : {L"TCoolBar", L"CoolBar", L"tcoolbar", L"COOLBAR"})
        {
            c.window.cls = name;
            CHECK(c.window.kind == GlassShared::GlassControlKind::None);
            CHECK(WndCompat::CoolBar_IsClassName(name));
            WndCompat::CoolBar_Attach(&c.window);
            CHECK(c.window.subclass && WndCompat::CoolBar_IsTracked(&c.window));
            CHECK(c.window.kind == GlassShared::GlassControlKind::CoolBar);
            WndCompat::CoolBar_Detach(&c.window);
        }
        for (const wchar_t* name : {L"", L"TCustomCoolBar", L"MyCoolBar", L"TCoolBar2",
            L"CoolBarSuffix", L".TCoolBar", L"ReBarWindow32", L"SizeableRebar", L"Rebar"})
        {
            c.window.cls = name;
            CHECK(!WndCompat::CoolBar_IsClassName(name));
            WndCompat::CoolBar_Attach(&c.window);
            CHECK(!c.window.subclass && !WndCompat::CoolBar_IsTracked(&c.window));
            CHECK(c.window.kind == GlassShared::GlassControlKind::None);
        }
        // An existing TCoolBar may already carry the old Unknown -> Root mark.
        // Catch-up must still find it by the real class and install its own path.
        c.window.cls = L"TCoolBar";
        c.window.kind = GlassShared::GlassControlKind::Root;
        c.parent.cls = L"TForm";
        TestWindow panel; panel.cls = L"TPanel";
        c.parent.children = {&panel}; panel.children = {&c.window};
        WndCompat::CoolBar_CatchUp(&c.parent);
        CHECK(c.window.subclass && WndCompat::CoolBar_IsTracked(&c.window));
        CHECK(c.window.kind == GlassShared::GlassControlKind::CoolBar);
        const auto proc = c.window.subclass;
        WndCompat::CoolBar_CatchUp(&c.parent);
        CHECK(c.window.subclass == proc);
        WndCompat::CoolBar_Detach(&c.window);
        CHECK(!WndCompat::CoolBar_IsTracked(&c.window));
    }

    void ProbeTextThroughSubclass()
    {
        Context c; c.Start();
        // Probe evidence: hdcWin class="TCoolBar" in both modes, not "CoolBar".
        CHECK(c.window.cls == L"TCoolBar");
        CHECK(c.window.kind == GlassShared::GlassControlKind::None && !c.window.subclass);
        // Native drawing may run before PaintDispatcher has a BeginPaint host.
        // The message subclass must already own the complete hook scope.
        Test::host = Test::topWindow = nullptr;
        WndCompat::CoolBar::SetCoolBarEventProvider({&OnPaintBegin, &OnPaintEnd});
        WndCompat::CoolBar_Attach(&c.window);
        CHECK(c.window.subclass && c.window.kind == GlassShared::GlassControlKind::CoolBar);
        TestTheme theme;
        bool themed = true;
        Test::duringNativePaint = [&](HWND w, UINT msg, WPARAM wp, LPARAM) {
            CHECK(w == &c.window && msg == WM_PAINT && wp == 0);
            CHECK(!Test::host && !Test::topWindow);
            c.CheckRefs(1);
            constexpr UINT hidePrefix = 0x100000;
            if (themed)
            {
                RECT rect{11, 4, 59, 19};
                CHECK(Call(c.modules[New], "DrawThemeText", &::DrawThemeText,
                    &theme, &c.dc, 0, 0, c.text, 8, hidePrefix, 0u, &rect) == S_OK);
            }
            else
            {
                RECT gripper{4, 2, 5, 22}, rect{9, 4, 57, 19};
                Call(c.modules[New], "ExtTextOutW", &::ExtTextOutW,
                    &c.dc, 0, 0, ETO_OPAQUE, &gripper, nullptr, 0u, nullptr);
                CHECK(Call(c.modules[New], "DrawTextW", &::DrawTextW,
                    &c.dc, c.text, 8, &rect, hidePrefix) == 15);
            }
        };
        const auto proc = c.window.subclass;
        c.window.theme = &theme;
        CHECK(proc(&c.window, WM_PAINT, 0, 0, 1, 0) == 777);
        c.CheckRefs(0);
        CHECK(Test::alphaText == 1 && Test::nativeTheme == 0);
        themed = false; c.window.theme = nullptr;
        CHECK(proc(&c.window, WM_PAINT, 0, 0, 1, 0) == 777);
        c.CheckRefs(0);
        CHECK(Test::alphaText == 2 && Test::nativeText == 0 && Test::nativePaint == 2);
        CHECK(Test::alphaBkMode == TRANSPARENT && c.dc.bkMode == OPAQUE);
        CHECK(Test::nativeExt == 1 && Test::black == 0); // Native gripper is preserved.
    }

    void TextRoutes()
    {
        Context c;
        // Model actual alternative imports: v6 has themed text, v5 plain text,
        // uxtheme DrawTextExW, EXE glyph output. Missing slots must be harmless.
        c.modules[New].originals.erase("DrawTextW");
        c.modules[Legacy].originals.erase("DrawThemeText");
        c.modules[Ux].originals.erase("DrawTextW");
        c.Start(); c.Begin();
        CHECK(!c.window.theme); // GetWindowTheme is not a rendering-path gate.
        TestTheme theme;
        CHECK(Call(c.modules[New], "DrawThemeText", &::DrawThemeText,
            &theme, &c.dc, 0, 0, c.text, 8, 0u, 0u, &c.textRect) == S_OK);
        CHECK(Test::alphaText == 1 && Test::nativeTheme == 0);
        CHECK(Test::alphaTextColor == RGB(12, 34, 56));
        CHECK(c.dc.font == &c.font && c.dc.text == 0);
        CHECK(Call(c.modules[Legacy], "DrawTextW", &::DrawTextW,
            &c.dc, c.text, 8, &c.textRect, 0u) == 15);
        CHECK(Call(c.modules[Ux], "DrawTextExW", &::DrawTextExW,
            &c.dc, c.text, 8, &c.textRect, 0u, nullptr) == 15);
        CHECK(Test::alphaText == 3 && Test::nativeText == 0);
        CHECK(Call(c.modules[Exe], "ExtTextOutW", &::ExtTextOutW,
            &c.dc, 11, 4, 0u, nullptr, c.text, 8u, nullptr));
        CHECK(Test::alphaGlyph == 1);
        OnPaintEnd(); c.CheckRefs(0);
        for (auto& m : c.modules) CHECK(m.hooks.empty());
    }

    void DcOwnership()
    {
        Context c; c.Start(); c.Begin();
        Call(c.modules[Legacy], "DrawTextW", &::DrawTextW,
            &c.foreign, c.text, 8, &c.textRect, 0u);
        Call(c.modules[Legacy], "DrawTextW", &::DrawTextW,
            &c.mem, c.text, 8, &c.textRect, 0u);
        CHECK(Test::alphaText == 0 && Test::nativeText == 2);
        Test::stackOwners[&c.mem] = &c.window;
        Call(c.modules[Legacy], "DrawTextW", &::DrawTextW,
            &c.mem, c.text, 8, &c.textRect, 0u);
        CHECK(Test::alphaText == 1);
        Test::topWindow = &c.parent; // A nested foreign paint is not this session.
        Call(c.modules[Legacy], "DrawTextW", &::DrawTextW,
            &c.mem, c.text, 8, &c.textRect, 0u);
        CHECK(Test::alphaText == 1 && Test::nativeText == 3);
        OnPaintEnd();
        // Explicit WM_PRINTCLIENT DC is authoritative, even a parent's DC.
        c.Begin(&c.foreign);
        Call(c.modules[Legacy], "DrawTextW", &::DrawTextW,
            &c.foreign, c.text, 8, &c.textRect, 0u);
        CHECK(Test::alphaText == 2);
        OnPaintEnd(); c.CheckRefs(0);
    }

    void TransparentTextState()
    {
        Context c; c.Start(); c.Begin();
        CHECK(c.dc.bkMode == OPAQUE);
        Call(c.modules[Legacy], "DrawTextW", &::DrawTextW,
            &c.dc, c.text, 8, &c.textRect, 0u);
        CHECK(Test::alphaBkMode == TRANSPARENT && c.dc.bkMode == OPAQUE);
        const INT advances[]{7, 8, 9, 10, 11, 12, 13, 14};
        constexpr UINT options = ETO_CLIPPED | ETO_GLYPH_INDEX | ETO_OPAQUE;
        Call(c.modules[Legacy], "ExtTextOutW", &::ExtTextOutW,
            &c.dc, 11, 4, options, &c.textRect, c.text, 8u, advances);
        CHECK(Test::alphaBkMode == TRANSPARENT && c.dc.bkMode == OPAQUE);
        CHECK(Test::glyphOptions == (options & ~ETO_OPAQUE));
        CHECK(Test::glyphDx == advances && Test::glyphCount == 8u);
        CHECK(Test::glyphOriginal == c.modules[Legacy].originals.at("ExtTextOutW"));
        CHECK(Test::black == 1 && Test::nativeExt == 0);
        // BkMode alone used to create a white box without ETO_OPAQUE.
        Call(c.modules[Legacy], "ExtTextOutW", &::ExtTextOutW,
            &c.dc, 11, 4, 0u, nullptr, c.text, 8u, advances);
        CHECK(Test::alphaBkMode == TRANSPARENT && c.dc.bkMode == OPAQUE);
        Test::alphaResult = E_FAIL;
        Call(c.modules[Legacy], "ExtTextOutW", &::ExtTextOutW,
            &c.dc, 11, 4, 0u, nullptr, c.text, 8u, advances);
        CHECK(Test::nativeExt == 1 && c.dc.saved.empty());
        OnPaintEnd(); c.CheckRefs(0);
    }

    void NativeSemanticsAndReentry()
    {
        Context c; c.Start(); c.Begin();
        for (UINT flag : {DT_CALCRECT, DT_MODIFYSTRING})
            CHECK(Call(c.modules[Legacy], "DrawTextW", &::DrawTextW,
                &c.dc, c.text, 8, &c.textRect, flag) == 17);
        DRAWTEXTPARAMS params{};
        CHECK(Call(c.modules[Ux], "DrawTextExW", &::DrawTextExW,
            &c.dc, c.text, 8, &c.textRect, 0u, &params) == 18);
        CHECK(Test::alphaText == 0 && Test::nativeText == 3);
        Test::duringAlpha = [&] {
            CHECK(Call(c.modules[Ux], "DrawTextExW", &::DrawTextExW,
                &c.dc, c.text, 8, &c.textRect, 0u, nullptr) == 18);
        };
        Call(c.modules[Legacy], "DrawTextW", &::DrawTextW,
            &c.dc, c.text, 8, &c.textRect, 0u);
        CHECK(Test::alphaText == 1 && Test::nativeText == 4);
        CHECK(c.dc.saved.empty() && c.dc.bkMode == OPAQUE);
        // A thunk must select the legacy original even when called by our test
        // (or another IAT detour), not by an address inside comctl32.
        OnPaintEnd();
        c.modules[Legacy].originals["DrawTextW"] = reinterpret_cast<void*>(LegacyDraw);
        c.Begin();
        CHECK(Call(c.modules[Legacy], "DrawTextW", &::DrawTextW,
            &c.foreign, c.text, 8, &c.textRect, 0u) == 102);
        OnPaintEnd(); c.CheckRefs(0);
    }

    void SessionsAndModules()
    {
        Context c;
        GlassCore::ModuleHandler::g_comctl32Legacy = &c.modules[New];
        c.Start(); c.Begin(); c.CheckRefs(1); // Aliased module is acquired once.
        c.Begin(); c.CheckRefs(2);
        OnPaintEnd(); c.CheckRefs(1);
        TestWindow rejected; rejected.backdrop = false;
        CHECK(!OnPaintBegin(&rejected, nullptr));
        c.CheckRefs(1); // Caller must not call End after a refused Begin.
        OnPaintEnd(); c.CheckRefs(0);
        GlassCore::ModuleHandler::g_comctl32Legacy = &c.modules[Legacy];
        Test::ux = nullptr;
        c.Begin();
        Test::ux = &c.modules[Ux]; // DLL appeared after Begin; no unmatched release.
        OnPaintEnd(); c.CheckRefs(0);
        OnPaintEnd(); c.CheckRefs(0); // Empty End is harmless.
        Shutdown();
        CHECK(!OnPaintBegin(&c.window, nullptr));
    }

    void BackgroundAndChrome()
    {
        Context c; c.Start(); c.Begin();
        RECT band{0, 0, 496, 40}, bottom{0, 39, 496, 40};
        Call(c.modules[Ux], "ExtTextOutW", &::ExtTextOutW,
            &c.dc, 0, 0, ETO_OPAQUE, &band, nullptr, 0u, nullptr);
        CHECK(Test::black == 1 && Test::nativeExt == 0);
        Call(c.modules[Ux], "ExtTextOutW", &::ExtTextOutW,
            &c.dc, 0, 0, ETO_OPAQUE, &bottom, nullptr, 0u, nullptr);
        CHECK(Test::black == 1 && Test::nativeExt == 1);
        Call(c.modules[Exe], "FillRect", &::FillRect, &c.dc, &bottom, &c.white);
        CHECK(Test::nativeFill == 1 && Test::black == 1);
        Call(c.modules[Exe], "FillRect", &::FillRect, &c.dc, &band, &c.white);
        CHECK(Test::nativeFill == 1 && Test::black == 2);
        LOGBRUSH accent{BS_SOLID, RGB(34, 78, 111)};
        c.dc.brush = &accent;
        Call(c.modules[Exe], "PatBlt", &::PatBlt, &c.dc, 0, 0, 496, 40, PATCOPY);
        CHECK(Test::lastRop == PATCOPY && Test::black == 2);
        c.dc.brush = &c.white;
        Call(c.modules[Exe], "PatBlt", &::PatBlt, &c.dc, 0, 0, 496, 40, PATCOPY);
        CHECK(Test::lastRop == BLACKNESS && Test::black == 3);
        Call(c.modules[Exe], "PatBlt", &::PatBlt, &c.dc, 0, 39, 496, 1, WHITENESS);
        CHECK(Test::lastRop == WHITENESS && Test::black == 3);
        OnPaintEnd(); c.CheckRefs(0);
    }

    int providerBegins = 0, providerEnds = 0;
    void PrintClientBridge()
    {
        Context c; c.Start();
        // A standalone print uses the supplied DC: no BeginPaint/GetDC provider
        // and no paint owner are simulated anywhere in this message.
        Test::host = Test::topWindow = nullptr;
        WndCompat::CoolBar::SetCoolBarEventProvider({&OnPaintBegin, &OnPaintEnd});
        WndCompat::CoolBar_Attach(&c.window);
        CHECK(c.window.subclass && WndCompat::CoolBar_IsTracked(&c.window));
        Test::duringNativePaint = [&](HWND w, UINT msg, WPARAM wp, LPARAM lp) {
            CHECK(w == &c.window && msg == WM_PRINTCLIENT && lp == 4);
            CHECK(reinterpret_cast<HDC>(wp) == &c.foreign);
            CHECK(!Test::host && !Test::topWindow);
            c.CheckRefs(1);
            Call(c.modules[Legacy], "DrawTextW", &::DrawTextW,
                &c.foreign, c.text, 8, &c.textRect, 0u);
        };
        const auto proc = c.window.subclass;
        CHECK(proc(&c.window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(&c.foreign), 4, 1, 0) == 777);
        CHECK(Test::nativePaint == 1 && Test::alphaText == 1 && Test::black == 0);
        c.CheckRefs(0);
        Test::duringNativePaint = {};
        CHECK(proc(&c.window, WM_ERASEBKGND, reinterpret_cast<WPARAM>(&c.dc), 0, 1, 0) == TRUE);
        CHECK(Test::black == 1 && Test::nativePaint == 1);

        WndCompat::CoolBar::SetCoolBarEventProvider({
            +[](HWND, HDC) { ++providerBegins; return false; },
            +[]() { ++providerEnds; }
        });
        CHECK(proc(&c.window, WM_PAINT, 0, 0, 1, 0) == 777);
        CHECK(providerBegins == 1 && providerEnds == 0);
        // The end callback is snapshotted before native painting resets it.
        WndCompat::CoolBar::SetCoolBarEventProvider({
            +[](HWND, HDC) { ++providerBegins; return true; },
            +[]() { ++providerEnds; }
        });
        Test::duringNativePaint = [](HWND, UINT, WPARAM, LPARAM) {
            WndCompat::CoolBar::SetCoolBarEventProvider({});
        };
        CHECK(proc(&c.window, WM_PAINT, 0, 0, 1, 0) == 777);
        CHECK(providerBegins == 2 && providerEnds == 1);
    }
}

int main(int argc, char** argv)
{
    const std::unordered_map<std::string, void(*)()> cases{
        {"text-routes", TextRoutes}, {"dc-ownership", DcOwnership},
        {"transparent-text-state", TransparentTextState}, {"native-semantics", NativeSemanticsAndReentry},
        {"sessions-and-modules", SessionsAndModules}, {"background-and-chrome", BackgroundAndChrome},
        {"printclient-bridge", PrintClientBridge},
        {"exact-class-attach", ExactClassAttachAndCatchUp},
        {"probe-through-subclass", ProbeTextThroughSubclass}
    };
    try
    {
        CHECK(argc == 2);
        cases.at(argv[1])();
        std::cout << argv[1] << ": OK\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << argv[1] << ": " << error.what() << '\n';
        return 1;
    }
}
