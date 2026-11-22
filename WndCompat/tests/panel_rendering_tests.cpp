// Compile the actual panel hook and the actual WndCompat message bridge. The
// entry window starts unmarked; native erase supplies its real solid brush.
#include "panel_test_shim.h"
#include "../../GlassCore/src/Hooks/delphi/TPanelDraw.cpp"
#include "../Root/DelphiPanelPaint.h"
#include <root_panel_theme_test.inc>
#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

BOOL WINAPI TestPanelBegin(HWND w, UINT msg, HDC dc) noexcept
{ return GlassCore::DelphiPanel::BeginPaint(w, msg, dc); }
void WINAPI TestPanelEnd() noexcept { GlassCore::DelphiPanel::EndPaint(); }
FARPROC PanelGetProcAddress(HMODULE, LPCSTR name)
{
    if (std::strcmp(name, "GlassCore_BeginDelphiPanelPaint") == 0) return reinterpret_cast<FARPROC>(TestPanelBegin);
    if (PanelTest::endAvailable && std::strcmp(name, "GlassCore_EndDelphiPanelPaint") == 0)
        return reinterpret_cast<FARPROC>(TestPanelEnd);
    return nullptr;
}

namespace
{
    using namespace GlassCore::DelphiPanel;
    struct Context
    {
        TestModule module;
        TestWindow window, other;
        TestDC dc, foreign, memory;
        RECT client{0, 0, 198, 58};
        LOGBRUSH custom{BS_SOLID, RGB(72, 144, 216)};
        Context()
        {
            window.cls = L"TFrame"; other.cls = L"TButton";
            PanelTest::clients[&window] = client;
            dc.owner = &window; dc.brush = &custom;
            foreign.owner = &other; foreign.brush = &custom;
            memory.brush = &custom;
            module.originals = {
                {"FillRect", reinterpret_cast<void*>(&::FillRect)},
                {"PatBlt", reinterpret_cast<void*>(&::PatBlt)},
                {"ExtTextOutW", reinterpret_cast<void*>(&::ExtTextOutW)},
            };
            GlassCore::ModuleHandler::g_entryPointModule = &module;
            Test::topWindow = Test::host = &window;
            Startup();
        }
        ~Context()
        {
            Test::duringNativePaint = {}; PanelTest::duringBlend = {}; PanelTest::duringThemeBuffer = {};
            Shutdown();
            assert(PanelTest::liveDcs == 0 && PanelTest::liveBitmaps == 0);
        }
        void Refs(int expected)
        { for (const auto& [name, ref] : module.refs) { (void)name; CHECK(ref == expected); } }
        bool Message(UINT msg = WM_ERASEBKGND, HDC supplied = nullptr, LPARAM lp = 0)
        {
            LRESULT result = -1;
            if (!supplied && msg == WM_ERASEBKGND) supplied = &dc;
            const bool handled = WndCompat::TryHandleDelphiPanelPaint(&window, msg,
                reinterpret_cast<WPARAM>(supplied), lp, result);
            if (handled) CHECK(result == 777);
            return handled;
        }
        template <typename Fn, typename... Args> auto Call(const char* name, Fn, Args... args)
        {
            CHECK(module.hooks.count(name));
            return reinterpret_cast<Fn>(module.hooks.at(name))(args...);
        }
        void Fill(HDC target = nullptr, const RECT* area = nullptr, HBRUSH brush = nullptr)
        { CHECK(Call("FillRect", &::FillRect, target ? target : &dc, area ? area : &client, brush ? brush : &custom)); }
    };

    void ClassesAndNativeErase()
    {
        Context c;
        CHECK(c.window.kind == GlassShared::GlassControlKind::None);
        CHECK(!GlassShared::IsDelphiPanelClassName(nullptr));
        int expected = 0;
        for (const wchar_t* name : {L"TPanel", L"TFlowPanel", L"TGridPanel", L"TStackPanel", L"TRelativePanel",
            L"TCategoryPanel", L"TCategoryPanelGroup", L"TScrollBox", L"TFrame", L"TSampleFrame", L"TEmbeddedForm", L"tframe"})
        {
            c.window.cls = name;
            Test::duringNativePaint = [&](HWND w, UINT msg, WPARAM wp, LPARAM lp) {
                CHECK(w == &c.window && msg == WM_ERASEBKGND && reinterpret_cast<HDC>(wp) == &c.dc && lp == 0);
                c.Refs(1); c.Fill();
            };
            CHECK(c.Message()); c.Refs(0);
            CHECK(PanelTest::blends == ++expected && Test::nativePaint == expected);
            CHECK(c.module.hooks.empty());
        }
        for (const wchar_t* name : {L"", L"MyPanel", L"TFrame2", L"MyTSampleFrame", L"TPanelSuffix", L"TForm",
            L"TButton", L"TCoolBar", L"ReBarWindow32", L"WindowsForms10.TPanel.app.0"})
        { c.window.cls = name; CHECK(!GlassShared::IsDelphiPanelClassName(name)); CHECK(!c.Message()); }
        CHECK(Test::nativeFill == 0);
    }
    void CustomAlphaAndSystemColors()
    {
        Context c;
        Test::duringNativePaint = [&](HWND, UINT, WPARAM, LPARAM) { c.Fill(); };
        for (COLORREF color : {RGB(72,144,216), RGB(0,0,0), RGB(255,0,0)})
        {
            c.custom.lbColor = color;
            const int before = PanelTest::blends;
            CHECK(c.Message()); CHECK(PanelTest::blends == before+1);
            const auto p = PanelTest::blendPixel;
            const auto mul = [](unsigned x) { return (x*100+127)/255; };
            CHECK(p[0] == mul(GetBValue(color)) && p[1] == mul(GetGValue(color)) && p[2] == mul(GetRValue(color)));
            CHECK(p[3] == 100 && PanelTest::blendMode.SourceConstantAlpha == 255);
            CHECK(PanelTest::blendMode.AlphaFormat == AC_SRC_ALPHA);
            CHECK(PanelTest::blendMode.BlendOp == AC_SRC_OVER);
        }
        for (COLORREF color : {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_BTNFACE)})
        {
            c.custom.lbColor = color;
            const int before = PanelTest::blends, black = Test::black;
            CHECK(c.Message()); CHECK(PanelTest::blends == before && Test::black == black+1);
        }
        // Compare against the *current* system values, not hardcoded white/gray.
        PanelTest::windowColor = RGB(20,21,22); PanelTest::faceColor = RGB(40,41,42);
        c.custom.lbColor = RGB(255,255,255);
        int before = PanelTest::blends;
        CHECK(c.Message()); CHECK(PanelTest::blends == before+1);
        CHECK(PanelTest::blendPixel[0] == 100 && PanelTest::blendPixel[3] == 100);
        c.custom.lbColor = GetSysColor(COLOR_WINDOW);
        CHECK(c.Message()); CHECK(PanelTest::blends == before+1);
        // Repaint always clears before blending, never alpha-over-alpha buildup.
        c.custom.lbColor = RGB(70,80,90);
        before = Test::black;
        CHECK(c.Message() && c.Message()); CHECK(Test::black == before+2);
        CHECK(Test::nativeFill == 0);
    }
    void PanelBackgroundDefaultScope()
    {
        Context c;
        int api = 0;
        Test::duringNativePaint = [&](HWND, UINT, WPARAM, LPARAM) {
            if (api == 0) c.Fill();
            else if (api == 1)
                CHECK(c.Call("PatBlt", &::PatBlt, &c.dc, 0, 0, 198, 58, PATCOPY));
            else
                CHECK(c.Call("ExtTextOutW", &::ExtTextOutW, &c.dc, 0, 0,
                    ETO_OPAQUE, &c.client, nullptr, 0u, nullptr));
        };
        const auto check = [&](LPCWSTR name, COLORREF color, bool tint) {
            c.window.cls = name; c.custom.lbColor = color; c.dc.background = color;
            const int blends = PanelTest::blends, black = Test::black;
            for (api = 0; api < 3; ++api) { CHECK(c.Message()); c.Refs(0); }
            CHECK(PanelTest::blends == blends + (tint ? 3 : 0) && Test::black == black+3);
            if (tint)
            {
                const auto mul = [](unsigned x) { return (x*100+127)/255; };
                const auto p = PanelTest::blendPixel;
                CHECK(p[0] == mul(GetBValue(color)) && p[1] == mul(GetGValue(color)) && p[2] == mul(GetRValue(color)));
                CHECK(p[3] == 100 && PanelTest::blendMode.SourceConstantAlpha == 255);
            }
        };
        // BWM defaults now match for exact TPanel, category panels and frames.
        // This is the client fill, not a child's gradient header or an image.
        for (LPCWSTR name : {L"TPanel", L"TCategoryPanelGroup", L"TCategoryPanel", L"TFrame"})
        {
            check(name, GetSysColor(COLOR_WINDOW), false);
            check(name, GetSysColor(COLOR_BTNFACE), false);
            check(name, RGB(72,144,216), true);
            check(name, RGB(0,0,0), true);
        }
        PanelTest::windowColor = RGB(20,21,22); PanelTest::faceColor = RGB(40,41,42);
        check(L"TPanel", GetSysColor(COLOR_WINDOW), false);
        check(L"TPanel", GetSysColor(COLOR_BTNFACE), false);
        check(L"TPanel", RGB(255,255,255), true);
        check(L"TFrame", GetSysColor(COLOR_BTNFACE), false);
        // When the two current system RGBs coincide, WINDOW still wins.
        PanelTest::faceColor = PanelTest::windowColor;
        check(L"TPanel", GetSysColor(COLOR_BTNFACE), false);
        CHECK(Test::nativeFill == 0 && Test::nativeExt == 0);
    }
    void NativeBrushApisAndChrome()
    {
        Context c;
        Test::duringNativePaint = [&](HWND, UINT, WPARAM, LPARAM) {
            CHECK(c.Call("PatBlt", &::PatBlt, &c.dc, 0, 0, 198, 58, PATCOPY));
            c.dc.background = RGB(30,60,90);
            CHECK(c.Call("ExtTextOutW", &::ExtTextOutW, &c.dc, 0, 0, ETO_OPAQUE, &c.client, nullptr, 0u, nullptr));
            const RECT bevel{0,0,198,1}, interior{1,1,197,57}, small{4,4,20,20};
            c.Fill(&c.dc, &bevel); c.Fill(&c.dc, &small); c.Fill(&c.dc, &interior);
            LOGBRUSH hatch{2, RGB(50,60,70)}; c.Fill(&c.dc, &c.client, &hatch);
            c.Fill(&c.dc, &c.client, reinterpret_cast<HBRUSH>(static_cast<ULONG_PTR>(COLOR_WINDOW+1)));
        };
        CHECK(c.Message());
        CHECK(PanelTest::blends == 3 && Test::nativeFill == 3 && Test::nativeExt == 0);
        CHECK(Test::black == 4); // Three custom fills + encoded COLOR_WINDOW brush.
        CHECK(PanelTest::liveDcs == 0 && PanelTest::liveBitmaps == 0);
    }
    void DcOwnershipAndPrintClient()
    {
        Context c;
        Test::duringNativePaint = [&](HWND, UINT, WPARAM, LPARAM) {
            c.Fill(&c.foreign); c.Fill(&c.memory);
            Test::stackOwners[&c.memory] = &c.window;
            c.Fill(&c.memory);
            Test::topWindow = &c.other; c.Fill(&c.memory);
        };
        CHECK(c.Message(WM_PAINT));
        CHECK(PanelTest::blends == 1 && Test::nativeFill == 3);
        Test::duringNativePaint = [&](HWND w, UINT msg, WPARAM wp, LPARAM lp) {
            CHECK(w == &c.window && msg == WM_PRINTCLIENT && reinterpret_cast<HDC>(wp) == &c.foreign && lp == 4);
            c.Fill(&c.foreign);
        };
        CHECK(c.Message(WM_PRINTCLIENT, &c.foreign, 4));
        CHECK(PanelTest::blends == 2 && Test::nativePaint == 2);
        c.Refs(0);
    }
    void GatesAndBalancedSessions()
    {
        Context c;
        Test::full = false; CHECK(!c.Message()); Test::full = true;
        c.window.backdrop = false; CHECK(!c.Message()); c.window.backdrop = true;
        Test::shuttingDown = true; CHECK(!c.Message()); Test::shuttingDown = false;
        PanelTest::coreAvailable = false; CHECK(!c.Message()); PanelTest::coreAvailable = true;
        PanelTest::endAvailable = false; CHECK(!c.Message()); PanelTest::endAvailable = true;
        CHECK(!c.Message(WM_NCDESTROY)); c.Refs(0);
        Test::duringNativePaint = [&](HWND, UINT, WPARAM, LPARAM) {
            c.Refs(1); PanelTest::endAvailable = false;
        };
        CHECK(c.Message()); c.Refs(0); // End must be the earlier snapshot.
        PanelTest::endAvailable = true; Test::duringNativePaint = {};
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); c.Refs(1);
        CHECK(BeginPaint(&c.window, WM_ERASEBKGND, &c.dc)); c.Refs(2);
        c.window.backdrop = false;
        CHECK(!BeginPaint(&c.window, WM_PAINT, nullptr)); c.Refs(2);
        EndPaint(); c.Refs(1); c.window.backdrop = true;
        EndPaint(); c.Refs(0); EndPaint(); c.Refs(0);
        c.module.originals.erase("PatBlt");
        CHECK(c.Message()); c.Refs(0); CHECK(c.module.hooks.empty());
        const auto imports = c.module.originals;
        c.module.originals.clear(); CHECK(!c.Message()); c.Refs(0);
        c.module.originals = imports;
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        Shutdown(); c.Refs(0); EndPaint(); c.Refs(0);
        CHECK(!c.Message());
    }
    HRESULT WINAPI ChainedThemeOriginal(HTHEME theme, HDC dc, int part, int state,
        LPCWSTR text, int count, DWORD flags, LPRECT rect, const DTTOPTS* options)
    {
        ++PanelTest::chainedThemeCalls;
        return ::DrawThemeTextEx(theme, dc, part, state, text, count, flags, rect, options);
    }
    void RootThemedClassGateAndOriginal()
    {
        Context c;
        TestModule notTheExe;
        TestTheme theme;
        RECT rect{0,0,40,15};
        DTTOPTS options{}; options.dwSize = sizeof(options); options.dwFlags = 0x801;
        c.module.originals["DrawThemeTextEx"] = reinterpret_cast<void*>(ChainedThemeOriginal);
        const GlassCore::Hooks::HookDispatcherDependency<1, 1> dep{{std::tuple{
            std::array{std::string_view{"uxtheme.dll"}}, "DrawThemeTextEx",
            reinterpret_cast<void*>(GlassCore::Root::Root_MyDelphiDrawThemeTextEx)}}};
        GlassCore::Hooks::HookDispatcher<1, 1> rootDispatcher{dep, &c.module};
        for (const wchar_t* name : {L"TEmbeddedForm", L"TFrame", L"TSampleFrame",
            L"TPanel", L"TFlowPanel", L"TGridPanel", L"TStackPanel", L"TRelativePanel",
            L"TCategoryPanel", L"TCategoryPanelGroup", L"TScrollBox", L"TGroupBox", L"TTabSheet",
            L"TCard", L"TRadioGroup", L"tframe"})
        {
            c.window.cls = name;
            wchar_t rawClass[64]{};
            CHECK(GetClassNameW(&c.window, rawClass, static_cast<int>(_countof(rawClass))) > 0);
            const auto specs = GlassCore::Root::PanelTextSpecsForTest(&c.module, rawClass);
            CHECK(specs.paintHooks.size() == 1 && specs.dcHooks.size() == 1);
            CHECK(specs.paintHooks.front() == &c.module && specs.dcHooks.front() == &c.module);
            CHECK(rootDispatcher.EnableHook(0, true));
            const int before = PanelTest::themeBuffers, chained = PanelTest::chainedThemeCalls;
            CHECK(c.Call("DrawThemeTextEx", &::DrawThemeTextEx, &theme, &c.dc,
                1, 1, name, -1, 0x40u, &rect, &options) == S_OK);
            CHECK(PanelTest::themeBuffers == before+1 && PanelTest::chainedThemeCalls == chained+1);
            CHECK(PanelTest::rootOriginalModule == &c.module);
            CHECK(rootDispatcher.EnableHook(0, false)); c.Refs(0);
            const auto foreignSpecs = GlassCore::Root::PanelTextSpecsForTest(&notTheExe, name);
            CHECK(foreignSpecs.paintHooks.empty() && foreignSpecs.dcHooks.empty());
        }
        for (const wchar_t* name : {L"TCoolBar", L"TForm", L"MyTFrame", L"TFrame2", L"TButton", L"Edit", L""})
        {
            const auto specs = GlassCore::Root::PanelTextSpecsForTest(&c.module, name);
            CHECK(specs.paintHooks.empty() && specs.dcHooks.empty());
        }
        c.window.cls = L"TFrame";
        CHECK(rootDispatcher.EnableHook(0, true));
        const int buffers = PanelTest::themeBuffers, chained = PanelTest::chainedThemeCalls;
        GlassCore::Root::g_shutdown = true;
        CHECK(c.Call("DrawThemeTextEx", &::DrawThemeTextEx, &theme, &c.dc,
            1, 1, L"TFrame", 6, 0x40u, &rect, &options) == S_OK);
        CHECK(PanelTest::themeBuffers == buffers && PanelTest::chainedThemeCalls == chained+1);
        GlassCore::Root::g_shutdown = false;
        Shutdown(); // The panel compositor also becomes native during unload.
        CHECK(c.Call("DrawThemeTextEx", &::DrawThemeTextEx, &theme, &c.dc,
            1, 1, L"TFrame", 6, 0x40u, &rect, &options) == S_OK);
        CHECK(PanelTest::themeBuffers == buffers && PanelTest::chainedThemeCalls == chained+2);
        CHECK(rootDispatcher.EnableHook(0, false)); c.Refs(0);
        const int native = PanelTest::nativeThemeCalls;
        CHECK(GlassCore::Root::Root_MyDelphiDrawThemeTextEx(&theme, &c.dc,
            1, 1, L"TFrame", 6, 0x40u, &rect, &options) == S_OK);
        CHECK(PanelTest::nativeThemeCalls == native+1 && PanelTest::chainedThemeCalls == chained+2);
    }

    void ProbeThemedCaptions()
    {
        Context c;
        TestTheme theme;
        DTTOPTS options{}; options.dwSize = sizeof(options); options.dwFlags = 0x801;
        options.crText = RGB(12,23,34); options.crBorder = RGB(45,56,67);
        options.iGlowSize = 5; options.iFontPropId = 123; options.lParam = 0x2468;
        const DTTOPTS originalOptions = options;
        struct Probe { const wchar_t* name; RECT client, text; };
        const Probe probes[]{
            {L"TEmbeddedForm", {0,0,498,58}, {0,0,92,15}},
            {L"TFrame", {0,0,198,58}, {0,0,40,15}},
            {L"TSampleFrame", {0,0,260,60}, {0,0,79,15}},
        };
        for (const auto& p : probes)
        {
            c.window.cls = p.name; c.client = p.client;
            PanelTest::clients[&c.window] = p.client;
            RECT rect = p.text;
            // During pre-BeginPaint the dispatcher may still name the parent.
            // The native message bracket + actual window DC is stronger evidence.
            Test::host = &c.other;
            Test::duringNativePaint = [&](HWND w, UINT msg, WPARAM, LPARAM) {
                CHECK(w == &c.window && msg == WM_PAINT);
                c.Refs(1);
                c.Fill(); // The caption must not clear the just-tinted background.
                const int black = Test::black, blends = PanelTest::blends;
                CHECK(PanelTest::blendPixel[3] == 100);
                CHECK(DrawContainerThemeTextEx(&::DrawThemeTextEx, &theme, &c.dc,
                    1, 1, p.name, std::wcslen(p.name), 0x40, &rect, &options) == S_OK);
                CHECK(Test::black == black && PanelTest::blends == blends);
            };
            const int buffers = PanelTest::themeBuffers, native = PanelTest::nativeThemeCalls;
            CHECK(c.Message(WM_PAINT)); c.Refs(0);
            CHECK(PanelTest::themeBuffers == buffers+1 && PanelTest::nativeThemeCalls == native+1);
            CHECK(!PanelTest::bufferErased);
            CHECK(PanelTest::themeDc != &c.dc && PanelTest::theme == &theme);
            CHECK(PanelTest::themePart == 1 && PanelTest::themeState == 1 && PanelTest::themeFlags == 0x40);
            CHECK(PanelTest::themeText == p.name && PanelTest::themeCount == static_cast<int>(std::wcslen(p.name)));
            DTTOPTS expected = originalOptions; expected.dwFlags |= DTT_COMPOSITED;
            CHECK(std::memcmp(&PanelTest::themeOptions, &expected, sizeof(expected)) == 0);
            CHECK(std::memcmp(&options, &originalOptions, sizeof(options)) == 0);
            CHECK(rect.left == p.text.left && rect.right == p.text.right && rect.bottom == p.text.bottom);
        }
    }
    void CardAndRadioGroupProbe()
    {
        Context c;
        TestTheme theme;
        struct Probe { LPCWSTR name; RECT client, rect; int part; DWORD flags, options; };
        const Probe probes[]{
            {L"TCard", {0,0,248,40}, {0,0,31,15}, 1, 0x40, 0x801},
            {L"TRadioGroup", {0,0,200,68}, {8,0,78,15}, 4, 0, 1},
        };
        for (const auto& p : probes)
        {
            c.window.cls = p.name; PanelTest::clients[&c.window] = p.client;
            CHECK(!GlassShared::IsDelphiPanelWindow(&c.window));
            CHECK(!c.Message(WM_PAINT)); // Captions only: no new panel background bridge.
            RECT rect = p.rect;
            DTTOPTS options{}; options.dwSize = sizeof(options); options.dwFlags = p.options;
            options.crText = RGB(12,23,34); options.iGlowSize = 5;
            const auto before = options;
            const int buffers = PanelTest::themeBuffers;
            CHECK(DrawContainerThemeTextEx(&ChainedThemeOriginal, &theme, &c.dc,
                p.part, 1, p.name, -1, p.flags, &rect, &options) == S_OK);
            CHECK(PanelTest::themeBuffers == buffers+1 && !PanelTest::bufferErased);
            CHECK(PanelTest::themeDc != &c.dc && PanelTest::themePart == p.part);
            CHECK(PanelTest::themeState == 1 && PanelTest::themeFlags == p.flags);
            CHECK(PanelTest::themeText == p.name && PanelTest::themeCount == -1);
            auto expected = before; expected.dwFlags |= DTT_COMPOSITED;
            CHECK(std::memcmp(&PanelTest::themeOptions, &expected, sizeof(expected)) == 0);
            CHECK(std::memcmp(&options, &before, sizeof(before)) == 0);
            CHECK(std::memcmp(&rect, &p.rect, sizeof(rect)) == 0);
            CHECK(Test::black == 0 && PanelTest::blends == 0);
            const int native = PanelTest::nativeThemeCalls;
            // Parent replay into a child's unowned MEMDC is not a caption DC.
            CHECK(DrawContainerThemeTextEx(&ChainedThemeOriginal, &theme, &c.memory,
                p.part, 1, p.name, -1, p.flags, &rect, &options) == S_OK);
            CHECK(PanelTest::themeBuffers == buffers+1 && PanelTest::nativeThemeCalls == native+1);
        }
        for (LPCWSTR name : {L"TCard2", L"MyTRadioGroup", L"TGroupButton", L"RadioGroup"})
        {
            c.window.cls = name;
            RECT r{0,0,31,15}; const int buffers = PanelTest::themeBuffers;
            CHECK(DrawContainerThemeTextEx(&ChainedThemeOriginal, &theme, &c.dc,
                1, 1, name, -1, 0x40, &r, nullptr) == S_OK);
            CHECK(PanelTest::themeBuffers == buffers);
        }
    }
    void ThemedDcAndLegacyContainers()
    {
        Context c;
        TestTheme theme;
        RECT rect{0,0,40,15};
        auto draw = [&](HDC dc) {
            return DrawContainerThemeTextEx(&::DrawThemeTextEx, &theme, dc,
                1, 1, L"TFrame", 6, 0x40, &rect, nullptr);
        };
        CHECK(draw(&c.dc) == S_OK && PanelTest::themeBuffers == 1);
        CHECK(PanelTest::themeOptions.dwSize == sizeof(DTTOPTS));
        CHECK(PanelTest::themeOptions.dwFlags == DTT_COMPOSITED);
        CHECK(draw(&c.foreign) == S_OK && PanelTest::themeBuffers == 1);
        CHECK(draw(&c.memory) == S_OK && PanelTest::themeBuffers == 1);
        Test::host = &c.other;
        CHECK(draw(&c.dc) == S_OK && PanelTest::themeBuffers == 1);
        Test::host = &c.window;
        c.window.backdrop = false;
        CHECK(draw(&c.dc) == S_OK && PanelTest::themeBuffers == 1);
        c.window.backdrop = true; Test::full = false;
        CHECK(draw(&c.dc) == S_OK && PanelTest::themeBuffers == 1); Test::full = true;
        // The Edit theme alone must never turn an actual TButton into a panel.
        c.window.cls = L"TButton";
        CHECK(draw(&c.dc) == S_OK && PanelTest::themeBuffers == 1);
        for (const wchar_t* name : {L"TGroupBox", L"TTabSheet", L"TPanel", L"TScrollBox"})
        { c.window.cls = name; CHECK(draw(&c.dc) == S_OK); }
        CHECK(PanelTest::themeBuffers == 5); // Existing VCL captions stay supported.
        c.window.cls = L"TFrame";
        Test::duringNativePaint = [&](HWND, UINT msg, WPARAM wp, LPARAM) {
            CHECK(msg == WM_PRINTCLIENT && reinterpret_cast<HDC>(wp) == &c.memory);
            CHECK(draw(&c.memory) == S_OK);
        };
        CHECK(c.Message(WM_PRINTCLIENT, &c.memory, 4));
        CHECK(PanelTest::themeBuffers == 6); c.Refs(0);
    }
    void ThemedNativeSemanticsAndFailure()
    {
        Context c;
        TestTheme theme;
        RECT rect{0,0,40,15};
        DTTOPTS options{}; options.dwSize = sizeof(options); options.dwFlags = 0x801;
        auto draw = [&](DWORD flags = 0x40) {
            return DrawContainerThemeTextEx(&::DrawThemeTextEx, &theme, &c.dc,
                1, 1, L"TFrame", 6, flags, &rect, &options);
        };
        for (const auto& input : std::array{
            std::tuple{static_cast<HDC>(nullptr), L"TFrame", 6, &rect},
            std::tuple{&c.dc, static_cast<LPCWSTR>(nullptr), 6, &rect},
            std::tuple{&c.dc, L"TFrame", 0, &rect},
            std::tuple{&c.dc, L"TFrame", 6, static_cast<LPRECT>(nullptr)}})
        {
            const auto [dc, text, count, area] = input;
            CHECK(DrawContainerThemeTextEx(&::DrawThemeTextEx, &theme, dc,
                1, 1, text, count, 0x40, area, &options) == S_OK);
            CHECK(PanelTest::themeBuffers == 0 && PanelTest::optionsPointer == &options);
        }
        RECT empty{};
        CHECK(DrawContainerThemeTextEx(&::DrawThemeTextEx, &theme, &c.dc,
            1, 1, L"TFrame", 6, 0x40, &empty, &options) == S_OK);
        CHECK(PanelTest::themeBuffers == 0);
        CHECK(draw(DT_CALCRECT) == S_OK && rect.right == 45);
        CHECK(draw(DT_MODIFYSTRING) == S_OK && PanelTest::optionsPointer == &options);
        for (DWORD flag : {DTT_CALCRECT, DTT_CALLBACK})
        {
            options.dwFlags = flag;
            CHECK(draw() == S_OK && PanelTest::optionsPointer == &options);
        }
        options.dwSize = sizeof(options)-8;
        CHECK(draw() == S_OK && PanelTest::themeBuffers == 0);
        options.dwSize = sizeof(options); options.dwFlags = 0x801;
        PanelTest::bufferResult = E_FAIL;
        CHECK(draw() == S_OK && PanelTest::themeDc == &c.dc && PanelTest::optionsPointer == &options);
        CHECK(PanelTest::themeOptions.dwFlags == 0x801);
        PanelTest::bufferResult = S_OK; PanelTest::failThemeBufferDraw = true;
        const int native = PanelTest::nativeThemeCalls;
        CHECK(draw() == S_OK && PanelTest::nativeThemeCalls == native+2);
        CHECK(PanelTest::themeDc == &c.dc && PanelTest::optionsPointer == &options);
        PanelTest::failThemeBufferDraw = false;
        const int buffers = PanelTest::themeBuffers;
        PanelTest::duringThemeBuffer = [&] { CHECK(draw() == S_OK && PanelTest::themeDc == &c.dc); };
        CHECK(draw() == S_OK && PanelTest::themeBuffers == buffers+1);
        PanelTest::duringThemeBuffer = {};
        CHECK(draw() == S_OK && PanelTest::themeBuffers == buffers+2);
        CHECK(Test::black == 0 && PanelTest::blends == 0);
    }

    void FailureAndReentry()
    {
        Context c;
        Test::duringNativePaint = [&](HWND, UINT, WPARAM, LPARAM) { c.Fill(); };
        for (bool* fail : {&PanelTest::failDc, &PanelTest::failBitmap, &PanelTest::failSelect, &PanelTest::failBlend})
        {
            const int native = Test::nativeFill;
            *fail = true; CHECK(c.Message()); *fail = false;
            CHECK(Test::nativeFill == native+1);
            CHECK(PanelTest::liveDcs == 0 && PanelTest::liveBitmaps == 0);
        }
        PanelTest::duringBlend = [&] { c.Fill(); };
        const int before = PanelTest::blends;
        CHECK(c.Message());
        CHECK(PanelTest::blends == before+1 && Test::nativeFill == 5);
        c.Refs(0);
    }
}
int main(int argc, char** argv)
{
    const std::unordered_map<std::string, void(*)()> cases{
        {"classes-and-erase", ClassesAndNativeErase}, {"alpha-colors", CustomAlphaAndSystemColors},
        {"panel-default-scope", PanelBackgroundDefaultScope},
        {"apis-and-chrome", NativeBrushApisAndChrome}, {"dc-and-print", DcOwnershipAndPrintClient},
        {"gates-and-sessions", GatesAndBalancedSessions}, {"failures-and-reentry", FailureAndReentry},
        {"root-themed-route", RootThemedClassGateAndOriginal},
        {"probe-themed-captions", ProbeThemedCaptions}, {"themed-dc-and-legacy", ThemedDcAndLegacyContainers},
        {"themed-native-semantics", ThemedNativeSemanticsAndFailure},
        {"card-and-radio-captions", CardAndRadioGroupProbe},
    };
    try { CHECK(argc == 2); cases.at(argv[1])(); std::cout << argv[1] << ": OK\n"; return 0; }
    catch (const std::exception& e) { std::cerr << argv[1] << ": " << e.what() << '\n'; return 1; }
}
