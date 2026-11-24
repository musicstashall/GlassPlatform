// Compile the actual panel hook and the actual WndCompat message bridge. The
// entry window starts unmarked; native erase supplies its real solid brush.
#include "panel_test_shim.h"
// Count environment queries, not a replacement for the production admission.
namespace PanelTest { inline thread_local int classQueries = 0; }
inline int PanelGetClassNameW(HWND hwnd, LPWSTR name, int capacity)
{
    ++PanelTest::classQueries;
    return GetClassNameW(hwnd, name, capacity);
}
#define GetClassNameW PanelGetClassNameW
#include "../../GlassCore/src/Hooks/delphi/TPanelDraw.cpp"
#include "../Root/DelphiPanelPaint.h"
#include <root_panel_theme_test.inc>
#include <stdexcept>
#include <thread>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

BOOL WINAPI TestPanelBegin(HWND w, UINT msg, HDC dc) noexcept
{ return GlassCore::DelphiPanel::BeginPaint(w, msg, dc); }
void WINAPI TestPanelEnd() noexcept { GlassCore::DelphiPanel::EndPaint(); }

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
        explicit Context(bool ansi = false)
        {
            window.cls = ansi ? L"TPanel" : L"TFrame"; other.cls = L"TButton";
            PanelTest::clients[&window] = client;
            dc.owner = &window; dc.brush = &custom;
            foreign.owner = &other; foreign.brush = &custom;
            memory.brush = &custom;
            module.originals = {
                {"FillRect", reinterpret_cast<void*>(&::FillRect)},
                {"PatBlt", reinterpret_cast<void*>(&::PatBlt)},
                {"ExtTextOutW", reinterpret_cast<void*>(&::ExtTextOutW)},
            };
            PanelTest::beginExport = reinterpret_cast<FARPROC>(&TestPanelBegin);
            PanelTest::endExport = reinterpret_cast<FARPROC>(&TestPanelEnd);
            GlassCore::ModuleHandler::g_entryPointModule = &module;
            Test::topWindow = Test::host = &window;
            Startup();
        }
        ~Context()
        {
            Test::duringNativePaint = {}; Test::duringAlpha = {};
            PanelTest::duringBlend = {}; PanelTest::duringThemeBuffer = {};
            Shutdown();
            assert(!PanelTest::textDetour && g_ansiTextRefs == 0);
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

    using AnsiFn = decltype(&::DrawTextA);
    AnsiFn AnsiHook(Context& c)
    {
        CHECK(!c.module.hooks.count("DrawTextA"));
        CHECK(PanelTest::textDetour && g_ansiTextAttached && g_ansiTextRefs);
        return reinterpret_cast<AnsiFn>(PanelTest::textDetour);
    }
    bool SameState(const DcState& a, const DcState& b)
    { return a.bkMode == b.bkMode && a.text == b.text && a.background == b.background
        && a.brush == b.brush && a.font == b.font; }
    bool SameDetails(const PanelTest::DcDetails& a, const PanelTest::DcDetails& b)
    { return std::memcmp(&a.clip, &b.clip, sizeof(RECT)) == 0
        && a.viewportX == b.viewportX && a.viewportY == b.viewportY; }
    void AnsiExportWithoutImportRoute()
    {
        Context c{true};
        Shutdown();
        c.module.originals.clear(); // No named import, no acquired background/image IAT refs.
        Startup();
        CHECK(c.module.hooks.empty());
        RECT rect{3,4,90,20};
        Test::duringNativePaint = [&](HWND w, UINT msg, WPARAM, LPARAM) {
            CHECK(w == &c.window && msg == WM_PAINT);
            CHECK(g_ansiTextRefs == 1 && PanelTest::attaches == 1);
            CHECK(c.module.hooks.empty() && c.module.refs.empty());
            CHECK(::DrawTextA(&c.dc, "P&anel", -1, &rect, 0x21) == 15);
        };
        CHECK(c.Message(WM_PAINT));
        CHECK(c.module.hooks.empty() && !PanelTest::textDetour && PanelTest::detaches == 1);
        CHECK(Test::alphaText == 1 && PanelTest::alphaText == L"P&anel");
        CHECK(PanelTest::alphaCount == 6 && PanelTest::alphaFlags == 0x21);
        CHECK(PanelTest::alphaDc == &c.dc && PanelTest::nativeAnsi == 0);
        Test::duringNativePaint = [&](HWND w, UINT msg, WPARAM wp, LPARAM lp) {
            CHECK(w == &c.window && msg == WM_PRINTCLIENT && reinterpret_cast<HDC>(wp) == &c.memory && lp == 4);
            const auto exported = reinterpret_cast<AnsiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "DrawTextA"));
            CHECK(exported(&c.memory,"Print",5,&rect,0) == 15);
        };
        CHECK(c.Message(WM_PRINTCLIENT,&c.memory,4));
        CHECK(PanelTest::alphaDc == &c.memory && PanelTest::alphaText == L"Print");
        CHECK(Test::alphaText == 2 && PanelTest::nativeAnsi == 0);
        CHECK(!c.Message(WM_ERASEBKGND)); c.Refs(0);
        // The independent export lease is not contingent on dispatcher.moduleAddress either.
        Shutdown(); GlassCore::ModuleHandler::g_entryPointModule = nullptr; Startup();
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        CHECK(::DrawTextA(&c.dc, "Panel", 5, &rect, 0) == 15); EndPaint();
        CHECK(!PanelTest::textDetour && c.module.hooks.empty());
        CHECK(Test::black == 0 && PanelTest::blends == 0 && Test::nativeFill == 0);
    }
    void AnsiExactClassesAndMessages()
    {
        Context c{true};
        for (LPCWSTR name : {L"TPanel", L"tPaNeL", L"TFrame", L"TSampleFrame", L"TEmbeddedForm",
            L"TFlowPanel", L"TGridPanel", L"TStackPanel", L"TRelativePanel", L"TCategoryPanel",
            L"TCategoryPanelGroup", L"TScrollBox", L"TPage", L"TPageControl", L"TPanel2", L"MyTPanel"})
        {
            c.window.cls = name;
            for (UINT msg : {WM_PAINT, WM_PRINTCLIENT, WM_ERASEBKGND})
            {
                // A panel, and a scroll box: the labels of the graphic controls it holds.
                const bool expected = (_wcsicmp(name, L"TPanel") == 0 || _wcsicmp(name, L"TScrollBox") == 0)
                    && msg != WM_ERASEBKGND;
                const bool began = BeginPaint(&c.window, msg, msg == WM_PRINTCLIENT ? &c.memory : nullptr);
                CHECK(bool(PanelTest::textDetour) == expected);
                CHECK(!c.module.hooks.count("DrawTextA"));
                if (began) EndPaint();
                c.Refs(0);
            }
        }
        CHECK(!GlassShared::IsDelphiPanelClassName(L"TPage"));
        for (LPCWSTR name : {L"TScrollBox2", L"MyTScrollBox", L"TScrollBo", L"TScrollingWinControl"})
        {
            c.window.cls = name;
            CHECK(!BeginPaint(&c.window, WM_PAINT, nullptr) && !PanelTest::textDetour);
        }
    }
    void ScrollBoxLabelsAreDrawnLikePanelCaptions()
    {
        // What a TScrollBox shows under its pictures are labels: graphic controls
        // painted inside its own paint with the application's DrawTextA.
        Context c{true}; c.window.cls = L"tScrollBOX";
        TestFont cyrillic{204}; c.dc.font = &cyrillic; PanelTest::fontCharsets[&cyrillic] = 204;
        RECT rect{8,4,180,17};
        const char label[]{char(0xc8),char(0xea),char(0xee),char(0xed),char(0xea),char(0xe0),' ','6','4','x','6','4'};
        Test::duringNativePaint = [&](HWND w, UINT msg, WPARAM, LPARAM) {
            CHECK(w == &c.window && msg == WM_PAINT && g_ansiTextRefs == 1 && PanelTest::attaches == 1);
            c.Refs(1); // the background imports of a panel class, as before
            CHECK(::DrawTextA(&c.dc, label, static_cast<int>(sizeof(label)), &rect, 0x40) == 15);
        };
        CHECK(c.Message(WM_PAINT));
        CHECK(Test::alphaText == 1 && PanelTest::nativeAnsi == 0 && PanelTest::alphaDc == &c.dc);
        CHECK(PanelTest::alphaText == L"\u0418\u043a\u043e\u043d\u043a\u0430 64x64" && PanelTest::alphaCount == 12);
        CHECK(PanelTest::alphaFlags == 0x40 && PanelTest::codepage == 1251);
        CHECK(rect.left == 8 && rect.top == 4 && rect.right == 180 && rect.bottom == 17);
        CHECK(!PanelTest::textDetour && PanelTest::detaches == 1); c.Refs(0);
        // A print of it is the same text; its erase is the background alone.
        Test::duringNativePaint = [&](HWND, UINT msg, WPARAM wp, LPARAM) {
            CHECK(msg == WM_PRINTCLIENT && reinterpret_cast<HDC>(wp) == &c.memory);
            CHECK(::DrawTextA(&c.memory, "Print", 5, &rect, 0) == 15);
        };
        CHECK(c.Message(WM_PRINTCLIENT, &c.memory, 4));
        CHECK(Test::alphaText == 2 && PanelTest::alphaDc == &c.memory && PanelTest::alphaText == L"Print");
        Test::duringNativePaint = [&](HWND, UINT msg, WPARAM, LPARAM) {
            CHECK(msg == WM_ERASEBKGND && !PanelTest::textDetour);
            CHECK(::DrawTextA(&c.dc, "Erase", 5, &rect, 0) == 43); // nobody's hook: the export itself
            c.Fill();
        };
        const int blends = PanelTest::blends;
        CHECK(c.Message()); CHECK(PanelTest::blends == blends + 1 && PanelTest::nativeAnsi == 1 && Test::alphaText == 2);
        // Another window's DC inside its paint is not its label.
        Test::duringNativePaint = [&](HWND, UINT, WPARAM, LPARAM) {
            CHECK(::DrawTextA(&c.foreign, "Other", 5, &rect, 0) == 43);
        };
        CHECK(c.Message(WM_PAINT)); CHECK(PanelTest::nativeAnsi == 2 && Test::alphaText == 2);
    }
    void AnsiCountsAndUtf16Length()
    {
        Context c{true};
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        const auto text = AnsiHook(c);
        const char exact[]{'A','&','B'}; // No trailing NUL: ASan catches strlen/count regressions.
        CHECK(text(&c.dc, exact, 3, &c.client, 0) == 15);
        CHECK(PanelTest::alphaText == L"A&B" && PanelTest::alphaCount == 3);
        const char embedded[]{'A',0,'B'};
        CHECK(text(&c.dc, embedded, 3, &c.client, 0) == 15);
        CHECK(PanelTest::alphaText == std::wstring(L"A\0B", 3));
        CHECK(text(&c.dc, "A\0ignored", -1, &c.client, 0) == 15);
        CHECK(PanelTest::alphaText == L"A" && PanelTest::alphaCount == 1);
        CHECK(text(&c.dc, exact, 1, &c.client, 0) == 15 && PanelTest::alphaText == L"A");
        TestFont japanese{128}; c.dc.font = &japanese; PanelTest::fontCharsets[&japanese] = 128;
        const char dbcs[]{char(0x82),char(0xa0),'X'};
        CHECK(text(&c.dc, dbcs, 3, &c.client, 0) == 15);
        CHECK(PanelTest::alphaText == L"\u3042X" && PanelTest::alphaCount == 2 && PanelTest::codepage == 932);
        c.dc.font = nullptr; PanelTest::systemCodepage = 65001;
        const char utf8[]{char(0xf0),char(0x9f),char(0x99),char(0x82),'X'};
        CHECK(text(&c.dc, utf8, 5, &c.client, 0) == 15);
        const std::wstring utf16{wchar_t(0xd83d), wchar_t(0xde42), L'X'};
        CHECK(PanelTest::alphaText == utf16 && PanelTest::alphaCount == 3);
        CHECK(PanelTest::codepage == CP_ACP && PanelTest::nativeAnsi == 0);
        const int before = Test::alphaText;
        CHECK(text(&c.dc, "", -1, &c.client, 0) == 43);
        CHECK(Test::alphaText == before && PanelTest::nativeAnsi == 1);
        EndPaint(); c.Refs(0);
    }
    void AnsiSelectedFontCodepage()
    {
        Context c{true}; TestFont font{204}; c.dc.font = &font;
        PanelTest::fontCharsets[&font] = 204;
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = AnsiHook(c);
        const char cyrillic[]{char(0xcf),char(0xf0)};
        CHECK(text(&c.dc, cyrillic, 2, &c.client, 0) == 15);
        CHECK(PanelTest::codepage == 1251 && PanelTest::alphaText == L"\u041f\u0440");
        CHECK(PanelTest::alphaState.font == &font && Test::alphaFont == &font);
        PanelTest::fontCharsets[&font] = 0;
        CHECK(text(&c.dc, "caf\xe9", 4, &c.client, 0) == 15);
        CHECK(PanelTest::codepage == 1252 && PanelTest::alphaText == L"caf\u00e9");
        PanelTest::fontCharsets[&font] = DEFAULT_CHARSET;
        const int translations = PanelTest::translations;
        CHECK(text(&c.dc, "ACP", -1, &c.client, 0) == 15 && PanelTest::codepage == CP_ACP);
        CHECK(PanelTest::translations == translations);
        for (int charset : {SYMBOL_CHARSET, -1, 254})
        {
            PanelTest::fontCharsets[&font] = charset;
            const int alpha = Test::alphaText, native = PanelTest::nativeAnsi;
            CHECK(text(&c.dc, cyrillic, 2, &c.client, 0) == 43);
            CHECK(Test::alphaText == alpha && PanelTest::nativeAnsi == native+1);
        }
        EndPaint(); c.Refs(0);
    }
    void AnsiDcOwnership()
    {
        Context c{true}; RECT rect{2,3,90,20};
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = AnsiHook(c);
        const auto expect = [&](HDC dc, bool alpha) {
            const int before = Test::alphaText, native = PanelTest::nativeAnsi;
            CHECK(text(dc, "Panel", 5, &rect, 0) == (alpha ? 15 : 43));
            CHECK(Test::alphaText == before+int(alpha) && PanelTest::nativeAnsi == native+int(!alpha));
        };
        Test::host = &c.other; expect(&c.dc, true); // Native message bracket, not an ambient host guess.
        expect(&c.foreign, false); expect(&c.memory, false); expect(nullptr, false);
        Test::stackOwners[&c.memory] = &c.window; expect(&c.memory, true);
        Test::topWindow = &c.other; expect(&c.memory, false);
        Test::topWindow = &c.window; Test::stackOwners[&c.memory] = &c.other; expect(&c.memory, false);
        Test::stackOwners[&c.memory] = &c.window;
        Test::stackOwners[&c.foreign] = &c.window; expect(&c.foreign, false);
        EndPaint();
        Test::stackOwners.clear();
        CHECK(BeginPaint(&c.window, WM_PRINTCLIENT, &c.memory));
        expect(&c.memory, true); expect(&c.foreign, false); EndPaint();
        CHECK(BeginPaint(&c.window, WM_PRINTCLIENT, &c.foreign));
        expect(&c.foreign, true); expect(&c.memory, false); EndPaint(); c.Refs(0);
    }
    void AnsiRuntimeGates()
    {
        Context c{true};
        Test::full = false; CHECK(!BeginPaint(&c.window, WM_PAINT, nullptr)); Test::full = true;
        c.window.backdrop = false; CHECK(!BeginPaint(&c.window, WM_PAINT, nullptr)); c.window.backdrop = true;
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = AnsiHook(c);
        Test::full = false; CHECK(text(&c.dc,"X",1,&c.client,0) == 43); Test::full = true;
        c.window.backdrop = false; CHECK(text(&c.dc,"X",1,&c.client,0) == 43); c.window.backdrop = true;
        c.window.valid = false; CHECK(text(&c.dc,"X",1,&c.client,0) == 43); c.window.valid = true;
        // A native HWND does not change its class in place. Exercise another
        // admitted paint context, not a fake mid-session class mutation.
        c.other.cls = L"TFrame"; CHECK(BeginPaint(&c.other, WM_PAINT, nullptr));
        CHECK(text(&c.foreign,"X",1,&c.client,0) == 43); EndPaint();
        CHECK(Test::alphaText == 0 && PanelTest::nativeAnsi == 4);
        CHECK(text(&c.dc,"X",1,&c.client,0) == 15 && Test::alphaText == 1);
        Shutdown();
        CHECK(text(&c.dc,"X",1,&c.client,0) == 43 && Test::alphaText == 1);
        EndPaint(); c.Refs(0); CHECK(!BeginPaint(&c.window, WM_PAINT, nullptr));
    }
    void AnsiNativeArgumentSemantics()
    {
        Context c{true}; CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = AnsiHook(c);
        char value[16] = "Panel"; RECT rect{2,3,90,20};
        const auto native = [&](LPCSTR input, int count, LPRECT area, UINT flags) {
            const int before = PanelTest::nativeAnsi;
            CHECK(text(&c.dc, input, count, area, flags) == 43);
            CHECK(PanelTest::nativeAnsi == before+1 && PanelTest::chainedAnsi == PanelTest::nativeAnsi);
            CHECK(PanelTest::ansiDc == &c.dc && PanelTest::ansiPointer == input);
            CHECK(PanelTest::ansiCount == count && PanelTest::ansiRect == area && PanelTest::ansiFlags == flags);
        };
        native(value, -1, &rect, DT_CALCRECT); CHECK(rect.right == 97);
        native(value, 5, &rect, DT_MODIFYSTRING); CHECK(value[0] == 'X');
        native(value, 5, &rect, DT_NOCLIP);
        native(nullptr, 5, &rect, 0); native(value, 0, &rect, 0); native(value, -2, &rect, 0);
        native(value, 5, nullptr, 0);
        for (RECT area : {RECT{}, RECT{10,0,5,8}, RECT{0,8,10,1},
            RECT{(std::numeric_limits<LONG>::min)(),0,(std::numeric_limits<LONG>::max)(),8},
            RECT{0,(std::numeric_limits<LONG>::min)(),8,(std::numeric_limits<LONG>::max)()}})
            native(value, 5, &area, 0);
        PanelTest::maps[&c.dc] = 8; native(value, 5, &rect, 0);
        CHECK(Test::alphaText == 0 && PanelTest::conversions == 0);
        EndPaint(); c.Refs(0);
    }
    void AnsiPreservesDcClippingAndBackground()
    {
        Context c{true}; TestFont font{204}; c.dc.font = &font;
        c.dc.text = RGB(10,20,30); c.dc.background = RGB(80,90,100); c.dc.bkMode = OPAQUE;
        const DcState state = c.dc; const auto details = PanelTest::dcDetails[&c.dc];
        PanelTest::mutateRenderer = true;
        RECT rect{3,4,80,20}; const RECT beforeRect = rect;
        Test::duringNativePaint = [&](HWND, UINT, WPARAM, LPARAM) {
            c.Fill(); // Accepted panel tint/black base must survive ANSI captions.
            const int black = Test::black, blends = PanelTest::blends, fill = Test::nativeFill;
            const int ext = Test::nativeExt, wide = Test::nativeText;
            CHECK(AnsiHook(c)(&c.dc,"Text",4,&rect,0x21) == 15);
            CHECK(Test::black == black && PanelTest::blends == blends && Test::nativeFill == fill);
            CHECK(Test::nativeExt == ext && Test::nativeText == wide);
            CHECK(PanelTest::alphaState.bkMode == TRANSPARENT);
            CHECK(PanelTest::alphaState.font == state.font && PanelTest::alphaState.text == state.text);
            CHECK(PanelTest::alphaState.background == state.background);
            CHECK(SameDetails(PanelTest::alphaDetails, details));
            CHECK(SameState(c.dc, state) && SameDetails(PanelTest::dcDetails[&c.dc], details));
            CHECK(GetMapMode(&c.dc) == MM_TEXT && c.dc.saved.empty());
            CHECK(PanelTest::savedDetails[&c.dc].empty());
            CHECK(std::memcmp(&rect, &beforeRect, sizeof(rect)) == 0);
            CHECK(PanelTest::alphaRectPointer != &rect);
        };
        CHECK(c.Message(WM_PAINT)); CHECK(PanelTest::blendPixel[3] == 100);
        c.custom.lbColor = GetSysColor(COLOR_WINDOW); CHECK(c.Message(WM_PAINT));
        c.dc.bkMode = TRANSPARENT; const int saved = SaveDC(&c.dc);
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        CHECK(AnsiHook(c)(&c.dc,"Text",4,&rect,0) == 15);
        CHECK(c.dc.bkMode == TRANSPARENT && c.dc.saved.size() == 1);
        EndPaint(); CHECK(RestoreDC(&c.dc, saved)); c.Refs(0);
    }
    void AnsiConversionFailures()
    {
        Context c{true}; TestFont font{204}; c.dc.font = &font;
        PanelTest::fontCharsets[&font] = 204;
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = AnsiHook(c);
        const char bytes[]{char(0xcf),char(0xf0)};
        for (bool* failure : {&PanelTest::failTranslate, &PanelTest::failConvert,
            &PanelTest::partialConvert, &PanelTest::throwConvert})
        {
            *failure = true; SetLastError(0x1234);
            const int native = PanelTest::nativeAnsi;
            CHECK(text(&c.dc, bytes, 2, &c.client, 0x20) == 43);
            CHECK(GetLastError() == PanelTest::nativeError && PanelTest::nativeEntryError == 0x1234);
            CHECK(PanelTest::nativeAnsi == native+1 && Test::alphaText == 0);
            CHECK(PanelTest::ansiPointer == bytes && PanelTest::ansiCount == 2 && PanelTest::ansiFlags == 0x20);
            CHECK(c.dc.saved.empty()); *failure = false;
        }
        CHECK(text(&c.dc, bytes, 2, &c.client, 0) == 15 && Test::alphaText == 1);
        EndPaint(); c.Refs(0);
    }
    void AnsiRendererFailures()
    {
        Context c{true}; RECT rect{3,4,90,20}; const RECT original = rect;
        c.dc.text = RGB(10,20,30); const DcState state = c.dc;
        const auto details = PanelTest::dcDetails[&c.dc];
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = AnsiHook(c);
        const auto fallback = [&] {
            const int native = PanelTest::nativeAnsi;
            CHECK(text(&c.dc,"Panel",5,&rect,0x20) == 43);
            CHECK(PanelTest::nativeAnsi == native+1 && PanelTest::ansiRect == &rect);
            CHECK(SameState(PanelTest::nativeState, state));
            CHECK(SameState(c.dc, state) && SameDetails(PanelTest::dcDetails[&c.dc], details));
            CHECK(SameDetails(PanelTest::nativeDetails, details) && c.dc.saved.empty());
            CHECK(std::memcmp(&rect,&original,sizeof(rect)) == 0);
        };
        PanelTest::failSave = true; fallback(); PanelTest::failSave = false;
        PanelTest::failMode = true; fallback(); PanelTest::failMode = false;
        CHECK(Test::alphaText == 0);
        PanelTest::mutateRenderer = true;
        Test::alphaResult = E_FAIL; fallback(); Test::alphaResult = S_OK;
        PanelTest::throwRenderer = true; fallback(); PanelTest::throwRenderer = false;
        CHECK(Test::alphaText == 2 && Test::black == 0 && PanelTest::blends == 0);
        PanelTest::renderedResult = 0;
        CHECK(text(&c.dc,"Panel",5,&rect,0) == 0 && PanelTest::nativeAnsi == 4);
        PanelTest::renderedResult = -7;
        CHECK(text(&c.dc,"Panel",5,&rect,0) == -7 && PanelTest::nativeAnsi == 4);
        EndPaint(); c.Refs(0);
    }
    void AnsiOriginalAndStaleFallback()
    {
        Context c{true}; CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = AnsiHook(c);
        CHECK(g_actualDrawTextA == &PanelAnsiTrampoline);
        CHECK(text(&c.dc,"Panel",-2,&c.client,0) == 43);
        CHECK(PanelTest::chainedAnsi == 1 && PanelTest::nativeAnsi == 1 && PanelTest::trampolines == 1);
        // Missing IAT original cannot bypass the export trampoline or suppress this renderer.
        PanelTest::noOriginal[&c.module] = {"DrawTextA"};
        CHECK(text(&c.dc,"Panel",5,&c.client,0) == 15);
        CHECK(Test::alphaText == 1 && PanelTest::nativeAnsi == 1);
        PanelTest::noOriginal.clear(); EndPaint();
        CHECK(g_actualDrawTextA == PanelTest::textExport && !PanelTest::textDetour);
        SetLastError(0x1234);
        CHECK(text(&c.dc,"Panel",5,&c.client,0) == 43);
        CHECK(PanelTest::nativeEntryError == 0x1234 && GetLastError() == PanelTest::nativeError);
        CHECK(PanelTest::chainedAnsi == 2 && PanelTest::nativeAnsi == 2 && PanelTest::trampolines == 1); c.Refs(0);
    }
    void AnsiLastError()
    {
        Context c{true}; CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = AnsiHook(c);
        PanelTest::logFormats.clear();
        SetLastError(0x1234);
        CHECK(text(&c.dc,"Do not log this text",-1,&c.client,0) == 15);
        CHECK(GetLastError() == 0x1234 && !PanelTest::logFormats.empty());
        for (const auto& format : PanelTest::logFormats) CHECK(format.find(L"Do not log this text") == format.npos);
        for (bool leavesError : {false,true})
        {
            PanelTest::nativeLeavesError = leavesError; SetLastError(0x1234);
            CHECK(text(&c.dc,"Panel",-2,&c.client,0) == 43);
            CHECK(PanelTest::nativeEntryError == 0x1234);
            CHECK(GetLastError() == (leavesError ? 0x1234u : PanelTest::nativeError));
        }
        PanelTest::nativeLeavesError = false; PanelTest::mutateRenderer = true; Test::alphaResult = E_FAIL;
        SetLastError(0x2468); CHECK(text(&c.dc,"Panel",5,&c.client,0) == 43);
        CHECK(PanelTest::nativeEntryError == 0x2468 && GetLastError() == PanelTest::nativeError);
        PanelTest::nativeResult = 0; SetLastError(0x9876);
        CHECK(text(&c.dc,"Panel",-2,&c.client,0) == 0);
        CHECK(PanelTest::nativeEntryError == 0x9876 && GetLastError() == PanelTest::nativeError);
        EndPaint(); c.Refs(0);
    }
    void AnsiNestedRefDeltas()
    {
        Context c{true}; CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = AnsiHook(c);
        CHECK(g_ansiTextRefs == 1 && PanelTest::attaches == 1);
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); CHECK(g_ansiTextRefs == 2);
        CHECK(text(&c.dc,"Panel",5,&c.client,0) == 15);
        EndPaint(); CHECK(g_ansiTextRefs == 1 && PanelTest::detaches == 0);
        CHECK(BeginPaint(&c.window, WM_ERASEBKGND, &c.dc)); CHECK(g_ansiTextRefs == 1);
        CHECK(text(&c.dc,"Panel",5,&c.client,0) == 43); EndPaint();
        c.other.cls = L"TFrame";
        CHECK(BeginPaint(&c.other, WM_PAINT, nullptr));
        CHECK(text(&c.foreign,"Frame",5,&c.client,0) == 43);
        CHECK(g_ansiTextRefs == 1); EndPaint();
        c.other.cls = L"TPanel";
        // Nested owners reuse the detour: even transaction failure flags must
        // not cause another Attach, a false acquisition, or an early Detach.
        PanelTest::failDetourBegin = PanelTest::failAttach = PanelTest::failCommit = true;
        CHECK(BeginPaint(&c.other, WM_PAINT, nullptr)); CHECK(g_ansiTextRefs == 2);
        CHECK(text(&c.dc,"Other",5,&c.client,0) == 43);
        CHECK(text(&c.foreign,"Inner",5,&c.client,0) == 15); EndPaint();
        CHECK(g_ansiTextRefs == 1 && PanelTest::attaches == 1 && PanelTest::attachAttempts == 1);
        PanelTest::failDetourBegin = PanelTest::failAttach = PanelTest::failCommit = false;
        CHECK(text(&c.dc,"Outer",5,&c.client,0) == 15); EndPaint(); c.Refs(0);
        CHECK(!g_ansiTextRefs && !PanelTest::textDetour && PanelTest::detaches == 1);
        CHECK(c.module.hooks.empty()); EndPaint(); c.Refs(0); CHECK(PanelTest::detaches == 1);
    }
    int WINAPI OtherAnsiOccupant(HDC, LPCSTR, int, LPRECT, UINT) { return 97; }
    void AnsiExportAndIndependentIatOccupant()
    {
        Context c{true};
        c.module.originals["DrawTextA"] = reinterpret_cast<void*>(&::DrawTextA);
        const GlassCore::Hooks::HookDispatcherDependency<1,1> dep{{std::tuple{
            std::array{std::string_view{"user32.dll"}}, "DrawTextA", reinterpret_cast<void*>(&OtherAnsiOccupant)}}};
        GlassCore::Hooks::HookDispatcher<1,1> peer{dep, &c.module};
        CHECK(peer.EnableHook(0,true));
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        CHECK(c.module.refs.at("DrawTextA") == 1 && peer.GetRef(0) == 1);
        CHECK(c.module.hooks.at("DrawTextA") == reinterpret_cast<void*>(&OtherAnsiOccupant));
        CHECK(reinterpret_cast<AnsiFn>(c.module.hooks.at("DrawTextA"))(&c.dc,"Panel",5,&c.client,0) == 97);
        CHECK(::DrawTextA(&c.dc,"Panel",5,&c.client,0) == 15 && Test::alphaText == 1);
        EndPaint(); CHECK(peer.GetRef(0) == 1 && !PanelTest::textDetour);
        CHECK(::DrawTextA(&c.dc,"Panel",5,&c.client,0) == 43 && PanelTest::chainedAnsi == 1);
        CHECK(peer.EnableHook(0,false)); c.Refs(0);
        // An import-only registration failure cannot disable the export route.
        PanelTest::registrationFailures[&c.module] = {"DrawTextA"};
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        CHECK(::DrawTextA(&c.dc,"Panel",5,&c.client,0) == 15); EndPaint();
        CHECK(c.module.refs.at("DrawTextA") == 0 && c.module.hooks.empty()); c.Refs(0);
    }
    void AnsiDetourAcquisitionFailures()
    {
        Context c{true};
        for (bool* fail : {&PanelTest::failDetourBegin, &PanelTest::failAttach, &PanelTest::failCommit})
        {
            *fail = true;
            CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); // Background refs still acquired.
            CHECK(!g_stack.back().ansiTextAcquired && !g_ansiTextRefs && !g_ansiTextAttached);
            CHECK(g_actualDrawTextA == PanelTest::textExport && !PanelTest::textDetour);
            CHECK(::DrawTextA(&c.dc,"Panel",5,&c.client,0) == 43); EndPaint(); c.Refs(0);
            Shutdown(); c.module.originals.clear(); Startup();
            CHECK(!BeginPaint(&c.window, WM_PAINT, nullptr)); CHECK(g_stack.empty());
            CHECK(!g_ansiTextRefs && !PanelTest::textDetour); *fail = false;
            CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
            CHECK(::DrawTextA(&c.dc,"Panel",5,&c.client,0) == 15); EndPaint();
            c.module.originals["FillRect"] = reinterpret_cast<void*>(&::FillRect);
        }
        CHECK(PanelTest::attaches == 3 && PanelTest::detaches == 3 && Test::alphaText == 3);
        CHECK(PanelTest::nativeAnsi == 3 && PanelTest::chainedAnsi == 3);
    }
    void AnsiMissingExportAndIdempotentStartup()
    {
        Context c{true}; Shutdown(); c.module.originals.clear();
        for (bool* available : {&PanelTest::user32Available, &PanelTest::textExportAvailable})
        {
            *available = false; Startup();
            CHECK(!g_actualDrawTextA && !BeginPaint(&c.window, WM_PAINT, nullptr));
            CHECK(!PanelTest::textDetour && PanelTest::attaches == 0 && g_stack.empty());
            Shutdown(); *available = true;
        }
        Startup(); const int resolutions = PanelTest::textResolutions; Startup();
        CHECK(PanelTest::textResolutions == resolutions && !PanelTest::textDetour);
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto trampoline = g_actualDrawTextA;
        Startup(); CHECK(g_actualDrawTextA == trampoline && PanelTest::textResolutions == resolutions);
        CHECK(::DrawTextA(&c.dc,"Panel",5,&c.client,0) == 15); EndPaint();
        CHECK(PanelTest::attaches == 1 && PanelTest::detaches == 1);
    }
    void AnsiDetachFailureAndShutdownLifetime()
    {
        Context c{true}; CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        const auto hook = AnsiHook(c); const auto trampoline = g_actualDrawTextA;
        PanelTest::failDetach = true; EndPaint(); c.Refs(0);
        CHECK(!g_ansiTextRefs && g_ansiTextAttached && g_actualDrawTextA == trampoline);
        CHECK(::DrawTextA(&c.dc,"No lease",8,&c.client,0) == 43 && Test::alphaText == 0);
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); CHECK(g_ansiTextRefs == 1);
        CHECK(PanelTest::attaches == 1 && PanelTest::attachAttempts == 1);
        CHECK(::DrawTextA(&c.dc,"Panel",5,&c.client,0) == 15);
        PanelTest::failDetach = false; EndPaint(); CHECK(!PanelTest::textDetour && PanelTest::detaches == 1);
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        PanelTest::failCommit = true; Shutdown();
        CHECK(!g_ansiTextRefs && g_ansiTextAttached && g_stack.empty()); c.Refs(0);
        CHECK(hook(&c.dc,"Shutdown",8,&c.client,0) == 43);
        const int resolutions = PanelTest::textResolutions;
        PanelTest::failCommit = false; Startup();
        CHECK(g_actualDrawTextA == trampoline && PanelTest::textResolutions == resolutions);
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); CHECK(PanelTest::attaches == 2);
        CHECK(::DrawTextA(&c.dc,"Panel",5,&c.client,0) == 15); Shutdown();
        CHECK(!PanelTest::textDetour && !g_ansiTextAttached && !g_ansiTextRefs);
        CHECK(PanelTest::attaches == 2 && PanelTest::detaches == 2);
        EndPaint(); Shutdown(); CHECK(PanelTest::detaches == 2); c.Refs(0);
        CHECK(hook(&c.dc,"Detached",8,&c.client,0) == 43);
    }
    void AnsiReentryAndLogBudget()
    {
        Context c{true}; CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = AnsiHook(c);
        bool insideCallback = false;
        Test::duringAlpha = [&] {
            CHECK(!insideCallback); insideCallback = true;
            CHECK(text(&c.dc,"Nested",6,&c.client,0) == 43); insideCallback = false;
        };
        CHECK(text(&c.dc,"Outer",5,&c.client,0) == 15);
        CHECK(Test::alphaText == 1 && PanelTest::nativeAnsi == 1); Test::duringAlpha = {};
        PanelTest::clock += 1001; PanelTest::logFormats.clear();
        for (int i = 0; i < 40; ++i) CHECK(text(&c.dc,"Burst",5,&c.client,0) == 15);
        CHECK(Test::alphaText == 41 && PanelTest::logFormats.size() == 16);
        CHECK(text(&c.dc,"Native",-2,&c.client,0) == 43 && PanelTest::nativeAnsi == 2);
        CHECK(PanelTest::logFormats.size() == 16);
        PanelTest::clock += 1001; CHECK(text(&c.dc,"Next",4,&c.client,0) == 15);
        CHECK(PanelTest::logFormats.size() == 17); EndPaint(); c.Refs(0);
    }
    void AnsiThreadLocalContext()
    {
        Context c{true}; CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = AnsiHook(c);
        bool passed = false;
        std::thread outsider([&] {
            RECT rect{2,3,90,20}; SetLastError(0x5678);
            passed = text(&c.dc,"Other thread",-1,&rect,0) == 43
                && PanelTest::nativeEntryError == 0x5678 && GetLastError() == PanelTest::nativeError;
        });
        outsider.join(); CHECK(passed && Test::alphaText == 0 && PanelTest::nativeAnsi == 1);
        CHECK(text(&c.dc,"Owner",5,&c.client,0) == 15); EndPaint(); c.Refs(0);
    }

    // TPage deliberately owns only the EXE ANSI import. Background imports
    // remain present to catch accidental enrolment in the wider panel policy.
    struct PageContext : Context
    {
        PageContext() : Context(true)
        {
            Shutdown();
            window.cls = L"TPage";
            module.originals["DrawTextA"] = reinterpret_cast<void*>(&::DrawTextA);
            Startup();
        }
    };
    int ImportRef(Context& c, const char* name)
    {
        const auto it = c.module.refs.find(name);
        return it == c.module.refs.end() ? 0 : it->second;
    }
    AnsiFn PageHook(Context& c)
    {
        CHECK(c.module.hooks.count("DrawTextA"));
        const auto fn = reinterpret_cast<AnsiFn>(c.module.hooks.at("DrawTextA"));
        CHECK(fn != &PanelDrawTextA);
        return fn;
    }
    int ImportedAnsi(Context& c, HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
    {
        const auto it = c.module.hooks.find("DrawTextA");
        const auto fn = reinterpret_cast<AnsiFn>(it == c.module.hooks.end()
            ? c.module.originals.at("DrawTextA") : it->second);
        return fn(dc, text, count, rect, flags);
    }
    void PageOnlyRefs(Context& c, int refs = 1)
    {
        CHECK(ImportRef(c, "DrawTextA") == refs);
        for (const char* name : {"FillRect", "PatBlt", "ExtTextOutW", "StretchBlt"})
            CHECK(ImportRef(c, name) == 0 && !c.module.hooks.count(name));
        CHECK(!PanelTest::textDetour && !g_ansiTextRefs && !g_ansiTextAttached);
    }
    void PageIatMessageRoute()
    {
        PageContext c;
        CHECK(c.window.kind == GlassShared::GlassControlKind::None);
        CHECK(!GlassShared::IsDelphiPanelWindow(&c.window));
        TestFont font{204}; c.dc.font = &font; PanelTest::fontCharsets[&font] = 204;
        RECT first{0,0,361,62}, second{0,70,361,124};
        const std::string a(219, char(0xcf)), b(176, char(0xf0));
        Test::duringNativePaint = [&](HWND w, UINT msg, WPARAM, LPARAM) {
            CHECK(w == &c.window && msg == WM_PAINT);
            PageOnlyRefs(c);
            CHECK(ImportedAnsi(c, &c.dc, a.data(), 219, &first, 0x50) == 15);
            CHECK(PanelTest::alphaCount == 219 && PanelTest::alphaText == std::wstring(219, L'\u041f'));
            CHECK(PanelTest::alphaFlags == 0x50 && PanelTest::codepage == 1251);
            CHECK(ImportedAnsi(c, &c.dc, b.data(), 176, &second, 0x50) == 15);
            CHECK(PanelTest::alphaCount == 176 && PanelTest::alphaText == std::wstring(176, L'\u0440'));
            CHECK(PanelTest::alphaState.bkMode == TRANSPARENT && PanelTest::alphaState.font == &font);
        };
        CHECK(c.Message(WM_PAINT)); PageOnlyRefs(c, 0);
        CHECK(Test::alphaText == 2 && PanelTest::nativeAnsi == 0 && c.module.hooks.empty());
        CHECK(Test::nativeFill == 0 && Test::nativeExt == 0 && Test::black == 0 && PanelTest::blends == 0);
        CHECK(PanelTest::attachAttempts == 0 && PanelTest::detourWrites == 0);
    }
    void PageIatPrintAndBridgeGates()
    {
        PageContext c; RECT rect{2,3,90,20};
        Test::duringNativePaint = [&](HWND w, UINT msg, WPARAM wp, LPARAM lp) {
            CHECK(w == &c.window && msg == WM_PRINTCLIENT && reinterpret_cast<HDC>(wp) == &c.memory && lp == 4);
            PageOnlyRefs(c);
            PanelTest::endAvailable = false; // End must use the snapshot, not a second lookup.
            CHECK(ImportedAnsi(c, &c.memory, "Print", 5, &rect, 0) == 15);
        };
        CHECK(c.Message(WM_PRINTCLIENT, &c.memory, 4)); PageOnlyRefs(c, 0);
        PanelTest::endAvailable = true; Test::duringNativePaint = {};
        CHECK(PanelTest::alphaDc == &c.memory && Test::nativePaint == 1);
        CHECK(!c.Message(WM_ERASEBKGND) && !c.Message(WM_NCDESTROY));
        Test::full = false; CHECK(!c.Message(WM_PAINT)); Test::full = true;
        c.window.backdrop = false; CHECK(!c.Message(WM_PAINT)); c.window.backdrop = true;
        Test::shuttingDown = true; CHECK(!c.Message(WM_PAINT)); Test::shuttingDown = false;
        PanelTest::coreAvailable = false; CHECK(!c.Message(WM_PAINT)); PanelTest::coreAvailable = true;
        PanelTest::endAvailable = false; CHECK(!c.Message(WM_PAINT)); PanelTest::endAvailable = true;
        c.window.valid = false; CHECK(!c.Message(WM_PAINT)); c.window.valid = true;
        CHECK(Test::nativePaint == 1 && Test::alphaText == 1); PageOnlyRefs(c, 0);
    }
    void PageIatExactClassesAndMessages()
    {
        PageContext c;
        for (LPCWSTR name : {L"TPage", L"tPaGe", L"TPageControl", L"TPage2", L"MyTPage", L"TTabSheet",
            L"TPanel", L"TFrame", L"TFlowPanel", L"TScrollBox", L"TForm", L"TButton", L""})
        {
            c.window.cls = name;
            const bool page = _wcsicmp(name, L"TPage") == 0;
            CHECK(!page || !GlassShared::IsDelphiPanelClassName(name));
            const auto themed = GlassCore::Root::PanelTextSpecsForTest(&c.module, name);
            if (page) CHECK(themed.paintHooks.empty() && themed.dcHooks.empty());
            for (UINT msg : {WM_PAINT, WM_PRINTCLIENT, WM_ERASEBKGND, WM_NCDESTROY})
            {
                const bool expected = page && (msg == WM_PAINT || msg == WM_PRINTCLIENT);
                const bool began = BeginPaint(&c.window, msg, msg == WM_PRINTCLIENT ? &c.memory : nullptr);
                CHECK(ImportRef(c, "DrawTextA") == int(expected));
                CHECK(bool(c.module.hooks.count("DrawTextA")) == expected);
                if (page) CHECK(began == expected && PanelTest::textDetour == nullptr);
                if (began) EndPaint();
                CHECK(!ImportRef(c, "DrawTextA") && c.module.hooks.empty()); c.Refs(0);
            }
        }
    }
    void PageIatMissingImportAndRegistration()
    {
        PageContext c;
        c.module.originals.erase("DrawTextA");
        CHECK(!BeginPaint(&c.window, WM_PAINT, nullptr) && !c.Message(WM_PAINT));
        CHECK(g_stack.empty()); PageOnlyRefs(c, 0);
        c.module.originals["DrawTextA"] = reinterpret_cast<void*>(&::DrawTextA);
        PanelTest::registrationFailures[&c.module] = {"DrawTextA"};
        CHECK(!BeginPaint(&c.window, WM_PAINT, nullptr) && !c.Message(WM_PRINTCLIENT, &c.memory));
        CHECK(g_stack.empty()); PageOnlyRefs(c, 0);
        PanelTest::registrationFailures.clear();
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        PageOnlyRefs(c); EndPaint(); PageOnlyRefs(c, 0); EndPaint();
        Shutdown(); GlassCore::ModuleHandler::g_entryPointModule = nullptr; Startup();
        CHECK(!BeginPaint(&c.window, WM_PAINT, nullptr) && g_stack.empty());
        CHECK(!PanelTest::attachAttempts && c.module.hooks.empty());
    }
    void PageIatIndependentOfResolvedExport()
    {
        PageContext c; RECT rect{2,3,90,20};
        for (bool* available : {&PanelTest::user32Available, &PanelTest::textExportAvailable})
        {
            Shutdown(); *available = false; Startup(); CHECK(!g_actualDrawTextA);
            CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); PageOnlyRefs(c);
            CHECK(ImportedAnsi(c, &c.dc, "IAT", 3, &rect, 0) == 15);
            CHECK(ImportedAnsi(c, &c.dc, "Native", -2, &rect, 0) == 43);
            EndPaint(); PageOnlyRefs(c, 0); *available = true;
        }
        CHECK(Test::alphaText == 2 && PanelTest::nativeAnsi == 2);
        CHECK(PanelTest::attachAttempts == 0 && PanelTest::detourWrites == 0);
    }
    void PageIatDcAndRuntimeOwnership()
    {
        PageContext c; RECT rect{2,3,90,20};
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = PageHook(c);
        const auto expect = [&](HDC dc, bool render) {
            const int alpha = Test::alphaText, native = PanelTest::nativeAnsi;
            CHECK(text(dc,"Page",4,&rect,0) == (render ? 15 : 43));
            CHECK(Test::alphaText == alpha+int(render) && PanelTest::nativeAnsi == native+int(!render));
        };
        Test::host = &c.other; expect(&c.dc, true);
        expect(&c.foreign, false); expect(&c.memory, false); expect(nullptr, false);
        Test::stackOwners[&c.memory] = &c.window; expect(&c.memory, true);
        Test::topWindow = &c.other; expect(&c.memory, false); Test::topWindow = &c.window;
        Test::stackOwners[&c.memory] = &c.other; expect(&c.memory, false);
        Test::stackOwners[&c.foreign] = &c.window; expect(&c.foreign, false);
        Test::full = false; expect(&c.dc, false); Test::full = true;
        c.window.backdrop = false; expect(&c.dc, false); c.window.backdrop = true;
        c.window.valid = false; expect(&c.dc, false); c.window.valid = true;
        c.other.cls = L"TFrame"; CHECK(BeginPaint(&c.other, WM_PAINT, nullptr));
        expect(&c.foreign, false); EndPaint(); // Another real window context is not an ANSI panel.
        expect(&c.dc, true); EndPaint();
        Test::stackOwners.clear(); CHECK(BeginPaint(&c.window, WM_PRINTCLIENT, &c.memory));
        expect(&c.memory, true); expect(&c.foreign, false); EndPaint();
        CHECK(BeginPaint(&c.window, WM_PRINTCLIENT, &c.foreign));
        expect(&c.foreign, true); expect(&c.memory, false); EndPaint(); PageOnlyRefs(c, 0);
    }
    void PageIatAnsiCountsAndFont()
    {
        PageContext c; TestFont font{204}; c.dc.font = &font; PanelTest::fontCharsets[&font] = 204;
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = PageHook(c);
        const char cyrillic[]{char(0xcf),char(0xf0)};
        CHECK(text(&c.dc,cyrillic,2,&c.client,0) == 15);
        CHECK(PanelTest::alphaText == L"\u041f\u0440" && PanelTest::codepage == 1251);
        const char bounded[]{'A','&','B'};
        CHECK(text(&c.dc,bounded,3,&c.client,0) == 15 && PanelTest::alphaText == L"A&B");
        const char embedded[]{'A',0,'B'};
        CHECK(text(&c.dc,embedded,3,&c.client,0) == 15 && PanelTest::alphaText == std::wstring(L"A\0B",3));
        CHECK(text(&c.dc,"A\0ignored",-1,&c.client,0) == 15 && PanelTest::alphaText == L"A");
        PanelTest::fontCharsets[&font] = 0;
        CHECK(text(&c.dc,"caf\xe9",4,&c.client,0) == 15 && PanelTest::alphaText == L"caf\u00e9");
        PanelTest::fontCharsets[&font] = 128;
        const char dbcs[]{char(0x82),char(0xa0),'X'};
        CHECK(text(&c.dc,dbcs,3,&c.client,0) == 15);
        CHECK(PanelTest::alphaCount == 2 && PanelTest::alphaText == L"\u3042X" && PanelTest::codepage == 932);
        PanelTest::fontCharsets[&font] = DEFAULT_CHARSET; PanelTest::systemCodepage = 65001;
        const char utf8[]{char(0xf0),char(0x9f),char(0x99),char(0x82),'X'};
        CHECK(text(&c.dc,utf8,5,&c.client,0) == 15);
        CHECK(PanelTest::alphaCount == 3 && PanelTest::alphaText == std::wstring({wchar_t(0xd83d),wchar_t(0xde42),L'X'}));
        for (int charset : {SYMBOL_CHARSET,-1,254})
        {
            PanelTest::fontCharsets[&font] = charset;
            CHECK(text(&c.dc,cyrillic,2,&c.client,0) == 43);
        }
        CHECK(Test::alphaText == 7 && PanelTest::nativeAnsi == 3); EndPaint(); PageOnlyRefs(c, 0);
    }
    void PageIatNativeArgumentSemantics()
    {
        PageContext c; CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = PageHook(c);
        char value[16] = "Page"; RECT rect{2,3,90,20};
        const auto native = [&](LPCSTR input, int count, LPRECT area, UINT flags) {
            const int before = PanelTest::nativeAnsi; SetLastError(0x1234);
            CHECK(text(&c.dc,input,count,area,flags) == 43);
            CHECK(PanelTest::nativeAnsi == before+1 && PanelTest::chainedAnsi == PanelTest::nativeAnsi);
            CHECK(PanelTest::ansiDc == &c.dc && PanelTest::ansiPointer == input);
            CHECK(PanelTest::ansiCount == count && PanelTest::ansiRect == area && PanelTest::ansiFlags == flags);
            CHECK(PanelTest::nativeEntryError == 0x1234 && GetLastError() == PanelTest::nativeError);
        };
        native(value,-1,&rect,DT_CALCRECT); CHECK(rect.right == 97);
        native(value,4,&rect,DT_MODIFYSTRING); CHECK(value[0] == 'X');
        native(value,4,&rect,DT_NOCLIP); native(nullptr,4,&rect,0);
        native(value,0,&rect,0); native(value,-2,&rect,0); native(value,4,nullptr,0);
        for (RECT area : {RECT{}, RECT{10,0,5,8}, RECT{0,8,10,1},
            RECT{(std::numeric_limits<LONG>::min)(),0,(std::numeric_limits<LONG>::max)(),8},
            RECT{0,(std::numeric_limits<LONG>::min)(),8,(std::numeric_limits<LONG>::max)()}})
            native(value,4,&area,0);
        PanelTest::maps[&c.dc] = 8; native(value,4,&rect,0);
        CHECK(Test::alphaText == 0 && PanelTest::conversions == 0); EndPaint(); PageOnlyRefs(c, 0);
    }
    void PageIatStateAndLastError()
    {
        PageContext c; TestFont font{204}; c.dc.font = &font; PanelTest::fontCharsets[&font] = 204;
        c.dc.text = RGB(10,20,30); c.dc.background = RGB(80,90,100);
        const DcState state = c.dc; const auto details = PanelTest::dcDetails[&c.dc];
        RECT rect{3,4,80,20}; const RECT original = rect;
        PanelTest::mutateRenderer = true;
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = PageHook(c);
        for (int mode : {OPAQUE,TRANSPARENT})
        {
            c.dc.bkMode = mode; const DcState expected = c.dc;
            SetLastError(0x2468); CHECK(text(&c.dc,"Sensitive page caption",-1,&rect,0x50) == 15);
            CHECK(GetLastError() == 0x2468 && SameState(c.dc, expected));
            CHECK(PanelTest::alphaState.bkMode == TRANSPARENT && PanelTest::alphaState.font == state.font);
            CHECK(PanelTest::alphaState.text == state.text && PanelTest::alphaState.background == state.background);
            CHECK(SameDetails(PanelTest::alphaDetails, details) && SameDetails(PanelTest::dcDetails[&c.dc], details));
            CHECK(std::memcmp(&rect,&original,sizeof(rect)) == 0 && PanelTest::alphaRectPointer != &rect);
            CHECK(c.dc.saved.empty() && PanelTest::savedDetails[&c.dc].empty() && GetMapMode(&c.dc) == MM_TEXT);
        }
        CHECK(Test::black == 0 && Test::nativeFill == 0 && Test::nativeExt == 0 && PanelTest::blends == 0);
        PanelTest::mutateRenderer = false;
        for (bool leavesError : {false,true})
        {
            PanelTest::nativeLeavesError = leavesError; SetLastError(0x1234);
            CHECK(text(&c.dc,"Native",-2,&rect,0) == 43 && PanelTest::nativeEntryError == 0x1234);
            CHECK(GetLastError() == (leavesError ? 0x1234u : PanelTest::nativeError));
        }
        EndPaint(); PageOnlyRefs(c, 0);
    }
    void PageIatFailureFallbacks()
    {
        PageContext c; TestFont font{204}; c.dc.font = &font; PanelTest::fontCharsets[&font] = 204;
        RECT rect{3,4,90,20}; const RECT original = rect;
        const DcState state = c.dc; const auto details = PanelTest::dcDetails[&c.dc];
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = PageHook(c);
        const auto fallback = [&] {
            const int before = PanelTest::nativeAnsi; SetLastError(0x1234);
            CHECK(text(&c.dc,"Page",4,&rect,0x50) == 43);
            CHECK(PanelTest::nativeAnsi == before+1 && PanelTest::chainedAnsi == PanelTest::nativeAnsi);
            CHECK(PanelTest::nativeEntryError == 0x1234 && GetLastError() == PanelTest::nativeError);
            CHECK(SameState(c.dc,state) && SameState(PanelTest::nativeState,state));
            CHECK(SameDetails(PanelTest::dcDetails[&c.dc],details) && SameDetails(PanelTest::nativeDetails,details));
            CHECK(std::memcmp(&rect,&original,sizeof(rect)) == 0 && PanelTest::ansiRect == &rect && c.dc.saved.empty());
        };
        for (bool* failure : {&PanelTest::failTranslate,&PanelTest::failConvert,&PanelTest::partialConvert,
            &PanelTest::throwConvert,&PanelTest::failSave,&PanelTest::failMode})
        { *failure = true; fallback(); *failure = false; }
        PanelTest::mutateRenderer = true; Test::alphaResult = E_FAIL; fallback(); Test::alphaResult = S_OK;
        PanelTest::throwRenderer = true; fallback(); PanelTest::throwRenderer = false;
        PanelTest::renderedResult = 0; CHECK(text(&c.dc,"Page",4,&rect,0) == 0);
        PanelTest::renderedResult = -7; CHECK(text(&c.dc,"Page",4,&rect,0) == -7);
        CHECK(PanelTest::nativeAnsi == 8 && Test::black == 0 && PanelTest::blends == 0);
        EndPaint(); PageOnlyRefs(c, 0);
    }
    void PageIatNestedRefsAndBarriers()
    {
        PageContext c; c.other.cls = L"TPage";
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = PageHook(c);
        CHECK(BeginPaint(&c.other, WM_PAINT, nullptr)); PageOnlyRefs(c, 2);
        CHECK(text(&c.dc,"Outer",5,&c.client,0) == 43);
        CHECK(text(&c.foreign,"Inner",5,&c.client,0) == 15);
        EndPaint(); PageOnlyRefs(c);
        CHECK(text(&c.dc,"Outer",5,&c.client,0) == 15);
        PanelTest::registrationFailures[&c.module] = {"DrawTextA"};
        CHECK(!BeginPaint(&c.other, WM_PAINT, nullptr));
        CHECK(g_stack.size() == 1 && ImportRef(c,"DrawTextA") == 1);
        PanelTest::registrationFailures.clear();
        c.other.cls = L"TFrame"; CHECK(BeginPaint(&c.other, WM_PAINT, nullptr));
        CHECK(text(&c.foreign,"Frame",5,&c.client,0) == 43); EndPaint();
        CHECK(text(&c.dc,"Outer",5,&c.client,0) == 15); EndPaint(); PageOnlyRefs(c, 0);
        CHECK(c.module.hooks.empty() && g_stack.empty() && Test::alphaText == 3 && PanelTest::nativeAnsi == 2);
        EndPaint(); PageOnlyRefs(c, 0);
    }
    void PageIatBackgroundIsolationUnderPanel()
    {
        PageContext c; c.other.cls = L"TPanel";
        CHECK(BeginPaint(&c.other, WM_PAINT, nullptr));
        CHECK(PanelTest::textDetour && g_ansiTextRefs == 1);
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        CHECK(ImportRef(c,"DrawTextA") == 1 && ImportRef(c,"FillRect") == 1);
        const int black = Test::black, blends = PanelTest::blends;
        CHECK(c.Call("FillRect",&::FillRect,&c.dc,&c.client,&c.custom));
        CHECK(c.Call("PatBlt",&::PatBlt,&c.dc,0,0,198,58,PATCOPY));
        CHECK(c.Call("ExtTextOutW",&::ExtTextOutW,&c.dc,0,0,ETO_OPAQUE,&c.client,nullptr,0u,nullptr));
        CHECK(Test::nativeFill == 1 && Test::nativeExt == 1);
        CHECK(Test::black == black && PanelTest::blends == blends);
        TestTheme theme; DTTOPTS options{}; options.dwSize = sizeof(options); options.dwFlags = 0x801;
        CHECK(DrawContainerThemeTextEx(&::DrawThemeTextEx,&theme,&c.dc,1,1,L"Page",4,0,&c.client,&options) == S_OK);
        CHECK(PanelTest::themeBuffers == 0 && PanelTest::nativeThemeCalls == 1);
        CHECK(ImportedAnsi(c,&c.dc,"Page",4,&c.client,0) == 15);
        CHECK(Test::alphaText == 1 && PanelTest::nativeAnsi == 0);
        CHECK(::DrawTextA(&c.dc,"Page through either hook",-1,&c.client,0) == 15);
        CHECK(Test::alphaText == 2 && PanelTest::nativeAnsi == 0);
        EndPaint(); CHECK(!ImportRef(c,"DrawTextA") && g_ansiTextRefs == 1);
        CHECK(::DrawTextA(&c.foreign,"Panel",5,&c.client,0) == 15);
        EndPaint(); c.Refs(0); CHECK(!PanelTest::textDetour && PanelTest::attaches == 1 && PanelTest::detaches == 1);
    }
    int iatForwardCalls = 0;
    int WINAPI CountedIatOriginal(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
    {
        ++iatForwardCalls;
        return ::DrawTextA(dc,text,count,rect,flags);
    }
    void PageIatDrawsNestedPanelDirectly()
    {
        PageContext c; c.other.cls = L"TPanel";
        c.module.originals["DrawTextA"] = reinterpret_cast<void*>(&CountedIatOriginal);
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        CHECK(BeginPaint(&c.other, WM_PAINT, nullptr));
        CHECK(ImportRef(c,"DrawTextA") == 1 && g_ansiTextRefs == 1);
        const int conversions = PanelTest::conversions;
        CHECK(ImportedAnsi(c,&c.foreign,"Panel",5,&c.client,0) == 15);
        CHECK(Test::alphaText == 1 && PanelTest::nativeAnsi == 0 && PanelTest::conversions == conversions+2);
        CHECK(iatForwardCalls == 0); // No pass through a second text hook just because the owner is TPanel.
        CHECK(PanelTest::attaches == 1 && PanelTest::detaches == 0);
        EndPaint(); PageOnlyRefs(c);
        CHECK(ImportedAnsi(c,&c.dc,"Page",4,&c.client,0) == 15);
        CHECK(Test::alphaText == 2 && PanelTest::nativeAnsi == 0 && PanelTest::detaches == 1);
        EndPaint(); PageOnlyRefs(c, 0);
    }
    void SharedAnsiContextDoesNotDependOnTheMatchingHookLease()
    {
        PageContext c; c.other.cls = L"TPanel";
        c.module.originals["DrawTextA"] = reinterpret_cast<void*>(&CountedIatOriginal);
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        // The already active IAT hook can draw the nested Glass panel even if
        // that panel's own export acquisition failed. Refs control lifetime,
        // not whether the current ANSI paint context is allowed to draw Glass.
        PanelTest::failAttach = true;
        CHECK(BeginPaint(&c.other, WM_PAINT, nullptr));
        CHECK(!g_stack.back().ansiTextAcquired && !g_stack.back().pageAnsiTextAcquired);
        CHECK(ImportRef(c,"DrawTextA") == 1 && g_ansiTextRefs == 0 && !PanelTest::textDetour);
        CHECK(ImportedAnsi(c,&c.foreign,"Panel",5,&c.client,0) == 15);
        CHECK(Test::alphaText == 1 && PanelTest::nativeAnsi == 0 && iatForwardCalls == 0);
        PanelTest::failAttach = false;
        EndPaint(); PageOnlyRefs(c);
        CHECK(ImportedAnsi(c,&c.dc,"Page",4,&c.client,0) == 15 && iatForwardCalls == 0);
        EndPaint(); PageOnlyRefs(c, 0);
    }
    void SharedAnsiFallbackIsNativeOnceForBothOwnersAndEntries()
    {
        PageContext c; c.other.cls = L"TPanel";
        c.module.originals["DrawTextA"] = reinterpret_cast<void*>(&CountedIatOriginal);
        RECT rect{3,4,90,20}; const RECT original = rect;
        Test::alphaResult = E_FAIL; PanelTest::mutateRenderer = true;
        for (bool pageOwner : {true,false})
        {
            if (pageOwner)
            { CHECK(BeginPaint(&c.other, WM_PAINT, nullptr)); CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); }
            else
            { CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); CHECK(BeginPaint(&c.other, WM_PAINT, nullptr)); }
            HDC dc = pageOwner ? &c.dc : &c.foreign;
            const DcState state = *dc;
            const auto details = PanelTest::dcDetails[dc];
            const int classQueries = PanelTest::classQueries;
            for (bool iatEntry : {true,false})
            {
                const int alpha = Test::alphaText, native = PanelTest::nativeAnsi;
                const int trampoline = PanelTest::trampolines, forwarded = iatForwardCalls;
                SetLastError(0x1234);
                CHECK((iatEntry ? ImportedAnsi(c,dc,"Text",4,&rect,0) : ::DrawTextA(dc,"Text",4,&rect,0)) == 43);
                CHECK(Test::alphaText == alpha+1 && PanelTest::nativeAnsi == native+1);
                CHECK(iatForwardCalls == forwarded+int(iatEntry) && PanelTest::trampolines == trampoline+1);
                CHECK(PanelTest::nativeEntryError == 0x1234 && GetLastError() == PanelTest::nativeError);
                CHECK(SameState(*dc,state) && SameState(PanelTest::nativeState,state));
                CHECK(SameDetails(PanelTest::dcDetails[dc],details) && dc->saved.empty());
                CHECK(std::memcmp(&rect,&original,sizeof(rect)) == 0 && !g_inHook);
                CHECK(PanelTest::classQueries == classQueries); // Admission is cached, including the fallback chain.
            }
            EndPaint(); EndPaint(); c.Refs(0);
        }
        Test::alphaResult = S_OK; PanelTest::mutateRenderer = false;
        CHECK(Test::alphaText == 4 && PanelTest::nativeAnsi == 4 && iatForwardCalls == 2);
    }
    void SharedAnsiMissingOriginalAndStaleIatStayNativeUnderExport()
    {
        PageContext c; c.other.cls = L"TPanel";
        CHECK(BeginPaint(&c.other, WM_PAINT, nullptr));
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = PageHook(c);
        PanelTest::noOriginal[&c.module] = {"DrawTextA"};
        SetLastError(0x1234);
        CHECK(text(&c.dc,"Missing original",-1,&c.client,0) == 43);
        CHECK(Test::alphaText == 0 && PanelTest::nativeAnsi == 1 && PanelTest::trampolines == 1);
        CHECK(PanelTest::nativeEntryError == 0x1234 && GetLastError() == PanelTest::nativeError && !g_inHook);
        PanelTest::noOriginal.clear(); EndPaint();
        CHECK(!ImportRef(c,"DrawTextA") && g_ansiTextRefs == 1);
        SetLastError(0x5678);
        CHECK(text(&c.foreign,"Stale IAT callback",-1,&c.client,0) == 43);
        CHECK(Test::alphaText == 0 && PanelTest::nativeAnsi == 2 && PanelTest::trampolines == 2);
        CHECK(PanelTest::nativeEntryError == 0x5678 && GetLastError() == PanelTest::nativeError && !g_inHook);
        CHECK(::DrawTextA(&c.foreign,"Panel",5,&c.client,0) == 15 && Test::alphaText == 1);
        EndPaint(); c.Refs(0); CHECK(!PanelTest::textDetour);
    }
    void PageIatHybridFallbackNativeOnce()
    {
        PageContext c; c.other.cls = L"TPanel";
        CHECK(BeginPaint(&c.other, WM_PAINT, nullptr)); CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        const auto text = PageHook(c); RECT rect{3,4,90,20};
        for (bool* failure : {&PanelTest::failConvert,&PanelTest::failSave,&PanelTest::failMode,&PanelTest::throwRenderer})
        {
            *failure = true; const int native = PanelTest::nativeAnsi, tramp = PanelTest::trampolines;
            SetLastError(0x1234); CHECK(text(&c.dc,"Page",4,&rect,0) == 43);
            CHECK(PanelTest::nativeAnsi == native+1 && PanelTest::trampolines == tramp+1);
            CHECK(PanelTest::nativeEntryError == 0x1234 && GetLastError() == PanelTest::nativeError);
            CHECK(!g_inHook); *failure = false;
        }
        const int native = PanelTest::nativeAnsi, alpha = Test::alphaText;
        Test::duringAlpha = [&] { CHECK(ImportedAnsi(c,&c.dc,"Reentry",7,&rect,0) == 43); };
        CHECK(text(&c.dc,"Page",4,&rect,0) == 15); Test::duringAlpha = {};
        CHECK(Test::alphaText == alpha+1 && PanelTest::nativeAnsi == native+1 && !g_inHook);
        EndPaint(); EndPaint(); c.Refs(0); CHECK(!PanelTest::textDetour);
    }
    void PageIatFirstOccupantAndPeerLifetime()
    {
        PageContext c;
        const GlassCore::Hooks::HookDispatcherDependency<1,1> dep{{std::tuple{
            std::array{std::string_view{"user32.dll"}},"DrawTextA",reinterpret_cast<void*>(&OtherAnsiOccupant)}}};
        GlassCore::Hooks::HookDispatcher<1,1> peer{dep,&c.module};
        CHECK(peer.EnableHook(0,true)); CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        CHECK(ImportRef(c,"DrawTextA") == 2 && peer.GetRef(0) == 1);
        CHECK(c.module.hooks.at("DrawTextA") == reinterpret_cast<void*>(&OtherAnsiOccupant));
        CHECK(ImportedAnsi(c,&c.dc,"Peer",4,&c.client,0) == 97);
        CHECK(Test::alphaText == 0 && PanelTest::nativeAnsi == 0);
        EndPaint(); CHECK(peer.GetRef(0) == 1 && ImportRef(c,"DrawTextA") == 1);
        CHECK(peer.EnableHook(0,false)); PageOnlyRefs(c, 0);
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto page = PageHook(c);
        CHECK(peer.EnableHook(0,true));
        CHECK(c.module.hooks.at("DrawTextA") == reinterpret_cast<void*>(page));
        CHECK(ImportedAnsi(c,&c.dc,"Page",4,&c.client,0) == 15);
        EndPaint(); CHECK(peer.GetRef(0) == 1 && ImportedAnsi(c,&c.dc,"Peer",4,&c.client,0) == 97);
        CHECK(peer.EnableHook(0,false)); PageOnlyRefs(c, 0);
    }
    void PageIatOriginalAndStaleHandler()
    {
        PageContext c; RECT rect{2,3,90,20};
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = PageHook(c);
        c.module.originals["DrawTextA"] = reinterpret_cast<void*>(&OtherAnsiOccupant);
        CHECK(text(&c.dc,"Original",-2,&rect,0) == 97 && PanelTest::nativeAnsi == 0);
        c.module.originals["DrawTextA"] = reinterpret_cast<void*>(&::DrawTextA);
        PanelTest::noOriginal[&c.module] = {"DrawTextA"};
        SetLastError(0x1234); CHECK(text(&c.dc,"Missing original",-1,&rect,0) == 43);
        CHECK(Test::alphaText == 0 && PanelTest::nativeAnsi == 1 && PanelTest::nativeEntryError == 0x1234);
        CHECK(GetLastError() == PanelTest::nativeError && !g_inHook);
        PanelTest::noOriginal.clear(); EndPaint();
        SetLastError(0x5678); CHECK(text(&c.dc,"Stale",5,&rect,0) == 43);
        CHECK(Test::alphaText == 0 && PanelTest::nativeAnsi == 2 && PanelTest::nativeEntryError == 0x5678);
        CHECK(GetLastError() == PanelTest::nativeError); PageOnlyRefs(c, 0);
    }
    Context* pageOriginalContext{};
    bool inPageOriginal = false;
    int WINAPI ReentrantPageOriginal(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
    {
        if (!inPageOriginal)
        {
            inPageOriginal = true;
            // Model a registry-supplied original which itself makes another
            // imported ANSI call. It must stay native during the owned fallback.
            CHECK(ImportedAnsi(*pageOriginalContext,dc,"Original reentry",-1,rect,0) == 43);
            inPageOriginal = false;
        }
        return ::DrawTextA(dc,text,count,rect,flags);
    }
    bool inExportOriginal = false;
    DWORD exportOriginalEntryError{};
    int WINAPI ReentrantExportOriginal(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
    {
        if (!inExportOriginal)
        {
            exportOriginalEntryError = GetLastError();
            inExportOriginal = true;
            CHECK(ImportedAnsi(*pageOriginalContext,dc,"Export original reentry",-1,rect,0) == 43);
            inExportOriginal = false;
        }
        return ChainedPanelAnsi(dc,text,count,rect,flags);
    }
    void SharedAnsiNativeFallbackOriginalReentry()
    {
        PageContext c; pageOriginalContext = &c;
        c.module.originals["DrawTextA"] = reinterpret_cast<void*>(&ReentrantPageOriginal);
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        SetLastError(0x1234);
        CHECK(ImportedAnsi(c,&c.dc,"Native",-2,&c.client,0) == 43);
        CHECK(Test::alphaText == 0 && PanelTest::nativeAnsi == 2);
        CHECK(PanelTest::conversions == 0 && !g_inHook && !inPageOriginal);
        CHECK(GetLastError() == PanelTest::nativeError);
        EndPaint(); PageOnlyRefs(c, 0);
        // The export trampoline's original can also call back through the IAT.
        // Both originals see native reentry, not a fresh compositor attempt.
        Shutdown();
        c.module.originals["DrawTextA"] = reinterpret_cast<void*>(&::DrawTextA);
        PanelTest::textExport = &ReentrantExportOriginal; Startup();
        c.other.cls = L"TPanel";
        CHECK(BeginPaint(&c.other, WM_PAINT, nullptr));
        CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        SetLastError(0x1234);
        CHECK(::DrawTextA(&c.dc,"Native",-2,&c.client,0) == 43);
        CHECK(Test::alphaText == 0 && PanelTest::nativeAnsi == 4 && PanelTest::trampolines == 2);
        CHECK(PanelTest::conversions == 0 && !g_inHook && !inExportOriginal);
        CHECK(exportOriginalEntryError == 0x1234 && GetLastError() == PanelTest::nativeError);
        EndPaint(); EndPaint(); c.Refs(0); pageOriginalContext = nullptr;
        Shutdown(); PanelTest::textExport = &ChainedPanelAnsi; Startup();
    }
    void PageIatShutdownAndRestart()
    {
        PageContext c; CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = PageHook(c);
        CHECK(BeginPaint(&c.window, WM_PRINTCLIENT, &c.memory)); PageOnlyRefs(c, 2);
        Startup(); PageOnlyRefs(c, 2); CHECK(g_stack.size() == 2);
        Shutdown(); CHECK(c.module.hooks.empty() && g_stack.empty()); PageOnlyRefs(c, 0);
        CHECK(text(&c.dc,"Shutdown",8,&c.client,0) == 43 && Test::alphaText == 0);
        EndPaint(); EndPaint(); CHECK(!BeginPaint(&c.window, WM_PAINT, nullptr));
        Startup(); CHECK(BeginPaint(&c.window, WM_PAINT, nullptr));
        CHECK(ImportedAnsi(c,&c.dc,"Restart",7,&c.client,0) == 15); EndPaint(); PageOnlyRefs(c, 0);
        CHECK(!PanelTest::attachAttempts && !PanelTest::detourWrites);
    }
    void PageIatReentryAndDiagnosticBudget()
    {
        PageContext c; CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = PageHook(c);
        Test::duringAlpha = [&] { CHECK(ImportedAnsi(c,&c.dc,"Reentry",7,&c.client,0) == 43); };
        CHECK(text(&c.dc,"Outer",5,&c.client,0) == 15); Test::duringAlpha = {};
        CHECK(Test::alphaText == 1 && PanelTest::nativeAnsi == 1 && !g_inHook);
        PanelTest::clock += 1001; PanelTest::logFormats.clear(); PanelTest::logStrings.clear();
        for (int i = 0; i < 40; ++i) CHECK(text(&c.dc,"Sensitive page caption",-1,&c.client,0) == 15);
        CHECK(Test::alphaText == 41 && PanelTest::logFormats.size() == 16);
        CHECK(std::find(PanelTest::logStrings.begin(),PanelTest::logStrings.end(),L"page-iat") != PanelTest::logStrings.end());
        for (const auto& value : PanelTest::logFormats) CHECK(value.find(L"Sensitive page caption") == value.npos);
        for (const auto& value : PanelTest::logStrings) CHECK(value.find(L"Sensitive page caption") == value.npos);
        CHECK(text(&c.dc,"Native",-2,&c.client,0) == 43 && PanelTest::nativeAnsi == 2);
        CHECK(PanelTest::logFormats.size() == 16); EndPaint(); PageOnlyRefs(c, 0);
    }
    void PageIatThreadLocalContext()
    {
        PageContext c; CHECK(BeginPaint(&c.window, WM_PAINT, nullptr)); const auto text = PageHook(c);
        bool passed = false;
        std::thread outsider([&] {
            RECT rect{2,3,90,20}; SetLastError(0x5678);
            passed = text(&c.dc,"Other thread",-1,&rect,0) == 43
                && PanelTest::nativeEntryError == 0x5678 && GetLastError() == PanelTest::nativeError;
        });
        outsider.join(); CHECK(passed && Test::alphaText == 0 && PanelTest::nativeAnsi == 1);
        CHECK(ImportedAnsi(c,&c.dc,"Owner",5,&c.client,0) == 15); EndPaint(); PageOnlyRefs(c, 0);
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
        {"ansi-route", AnsiExportWithoutImportRoute}, {"ansi-classes", AnsiExactClassesAndMessages},
        {"scroll-labels", ScrollBoxLabelsAreDrawnLikePanelCaptions},
        {"ansi-lengths", AnsiCountsAndUtf16Length}, {"ansi-codepages", AnsiSelectedFontCodepage},
        {"ansi-dcs", AnsiDcOwnership}, {"ansi-gates", AnsiRuntimeGates},
        {"ansi-native-args", AnsiNativeArgumentSemantics}, {"ansi-state", AnsiPreservesDcClippingAndBackground},
        {"ansi-conversion-failures", AnsiConversionFailures}, {"ansi-render-failures", AnsiRendererFailures},
        {"ansi-original", AnsiOriginalAndStaleFallback}, {"ansi-last-error", AnsiLastError},
        {"ansi-nested", AnsiNestedRefDeltas}, {"ansi-imports", AnsiExportAndIndependentIatOccupant},
        {"ansi-detour-failures", AnsiDetourAcquisitionFailures},
        {"ansi-detour-resolution", AnsiMissingExportAndIdempotentStartup},
        {"ansi-detour-lifetime", AnsiDetachFailureAndShutdownLifetime},
        {"ansi-reentry-budget", AnsiReentryAndLogBudget}, {"ansi-tls", AnsiThreadLocalContext},
        {"page-route", PageIatMessageRoute}, {"page-print-gates", PageIatPrintAndBridgeGates},
        {"page-classes", PageIatExactClassesAndMessages}, {"page-import-failures", PageIatMissingImportAndRegistration},
        {"page-export-independent", PageIatIndependentOfResolvedExport},
        {"page-dcs", PageIatDcAndRuntimeOwnership}, {"page-ansi", PageIatAnsiCountsAndFont},
        {"page-native-args", PageIatNativeArgumentSemantics}, {"page-state-error", PageIatStateAndLastError},
        {"page-fallbacks", PageIatFailureFallbacks}, {"page-nested", PageIatNestedRefsAndBarriers},
        {"page-no-background", PageIatBackgroundIsolationUnderPanel},
        {"page-nested-panel", PageIatDrawsNestedPanelDirectly}, {"page-hybrid-fallback", PageIatHybridFallbackNativeOnce},
        {"page-occupants", PageIatFirstOccupantAndPeerLifetime}, {"page-original", PageIatOriginalAndStaleHandler},
        {"page-original-reentry", SharedAnsiNativeFallbackOriginalReentry},
        {"page-shutdown", PageIatShutdownAndRestart}, {"page-reentry", PageIatReentryAndDiagnosticBudget},
        {"page-tls", PageIatThreadLocalContext},
        {"ansi-shared-context", SharedAnsiContextDoesNotDependOnTheMatchingHookLease},
        {"ansi-shared-fallback", SharedAnsiFallbackIsNativeOnceForBothOwnersAndEntries},
        {"ansi-shared-missing-original", SharedAnsiMissingOriginalAndStaleIatStayNativeUnderExport},
    };
    try { CHECK(argc == 2); cases.at(argv[1])(); std::cout << argv[1] << ": OK\n"; return 0; }
    catch (const std::exception& e) { std::cerr << argv[1] << ": " << e.what() << '\n'; return 1; }
}
