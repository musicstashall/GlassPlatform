// Compile the actual panel hook and the actual WndCompat message bridge. The
// entry window starts unmarked; native erase supplies its real solid brush.
#include "panel_test_shim.h"
#include "../../GlassCore/src/Hooks/delphi/TPanelDraw.cpp"
#include "../Root/DelphiPanelPaint.h"
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
            Test::duringNativePaint = {}; PanelTest::duringBlend = {};
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
        {"apis-and-chrome", NativeBrushApisAndChrome}, {"dc-and-print", DcOwnershipAndPrintClient},
        {"gates-and-sessions", GatesAndBalancedSessions}, {"failures-and-reentry", FailureAndReentry},
    };
    try { CHECK(argc == 2); cases.at(argv[1])(); std::cout << argv[1] << ": OK\n"; return 0; }
    catch (const std::exception& e) { std::cerr << argv[1] << ": " << e.what() << '\n'; return 1; }
}
