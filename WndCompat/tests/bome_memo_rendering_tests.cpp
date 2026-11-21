#include "bome_memo_test_shim.h"
#include "../../GlassCore/src/Hooks/delphi/TBomeMemoDraw.cpp"
#include <root_bome_memo_test.inc>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
namespace
{
    constexpr UINT kRowFlags = DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER;
    int WINAPI ChainText(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
    { ++BomeTest::chainTexts; return DrawTextA(dc, text, count, rect, flags); }
    int WINAPI ChainFill(HDC dc, const RECT* rect, HBRUSH brush)
    { ++BomeTest::chainFills; return BomeNativeFill(dc, rect, brush); }
    struct Context
    {
        TestModule module;
        TestWindow window, other;
        TestDC dc, memory, foreign, nc;
        TestFont font{45};
        LOGBRUSH brush{BS_SOLID,0xffffff};
        GlassCore::Hooks::HookDispatcher<2,3> dispatcher;
        Context() : dispatcher(GlassCore::Root::BomeDependenciesForTest, &module)
        {
            window.cls = L"TBomeMemo"; other.cls = L"TPanel";
            dc.owner = nc.owner = &window; foreign.owner = &other; dc.font = memory.font = &font;
            Test::host = Test::topWindow = &window;
            Test::stackOwners.clear(); Test::stackOwners[&memory] = &window;
            BomeTest::dcs.clear(); BomeTest::paints.clear(); BomeTest::paints[&window] = &dc;
            GlassCore::ModuleHandler::g_entryPointModule = &module;
            module.originals = {{"DrawTextA", reinterpret_cast<void*>(ChainText)},
                {"FillRect", reinterpret_cast<void*>(ChainFill)}};
            dispatcher.EnableHook(0, true); dispatcher.EnableHook(1, true);
        }
        ~Context()
        {
            dispatcher.DisableAllHooks(); assert(module.hooks.empty());
            for (const auto& [api,ref] : module.refs) { (void)api; assert(!ref); }
        }
        int Text(HDC target = nullptr, LPCSTR text = "row", int count = 3, UINT flags = kRowFlags,
            LPRECT rect = nullptr)
        {
            RECT row{0,0,981,16};
            return reinterpret_cast<decltype(&DrawTextA)>(module.hooks.at("DrawTextA"))(
                target ? target : &dc, text, count, rect ? rect : &row, flags);
        }
        int Fill(HDC target = nullptr, const RECT* rect = nullptr, HBRUSH value = nullptr)
        {
            RECT row{440,0,981,16};
            return reinterpret_cast<decltype(&FillRect)>(module.hooks.at("FillRect"))(
                target ? target : &dc, rect ? rect : &row, value ? value : &brush);
        }
    };
    void ExactClassRoute()
    {
        Context c; TestModule otherModule;
        for (const auto* name : {L"TBomeMemo", L"TBMGutter", L"tbomememo", L"tbmgutter"})
        {
            c.window.cls = name;
            CHECK(GlassShared::IsBomeMemoClassName(name));
            const auto specs = GlassCore::Root::BomeSpecsForTest(&c.module,name);
            CHECK(specs.paintHooks.size() == 2 && specs.dcHooks.empty());
            CHECK(specs.paintHooks[0] == std::make_pair(&c.module,size_t(GlassCore::Root::OHK_BOME_MEMO_TEXT)));
            CHECK(specs.paintHooks[1] == std::make_pair(&c.module,size_t(GlassCore::Root::OHK_BOME_MEMO_FILL)));
            CHECK(GlassCore::Root::BomeSpecsForTest(&otherModule,name).paintHooks.empty());
            CHECK(BomeClassifier::KindForTest(name) == BomeClassifier::ControlKind::DelphiContainer);
            CHECK(c.Text() == 15);
        }
        CHECK(!GlassShared::IsBomeMemoClassName(nullptr));
        const int alpha = Test::alphaText;
        for (const auto* name : {L"TBomeMemo2", L"TBMGutterChild", L"BomeMemo", L"Memo", L"Edit", L"TMemo", L"TPanel"})
        {
            c.window.cls = name;
            CHECK(!GlassShared::IsBomeMemoClassName(name));
            CHECK(GlassCore::Root::BomeSpecsForTest(&c.module,name).paintHooks.empty());
            CHECK(c.Text() == 41);
        }
        CHECK(Test::alphaText == alpha);
    }
    void CapturedMemoAndGutter()
    {
        Context c;
        const char* rows[]{"<?xml version='1.0' encoding='UTF-8' standalone='yes'?>",
            "<assembly xmlns='urn:schemas-microsoft-com:asm.v1' manifestVersion='1.0'>",
            "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">", "    <security>",
            "      <requestedPrivileges>", "        <requestedExecutionLevel level='asInvoker' uiAccess='false' />",
            "      </requestedPrivileges>", "    </security>", "  </trustInfo>", "</assembly>"};
        for (int i = 0; i < 10; ++i)
        {
            RECT rect{0,i*16,981,(i+1)*16}; const RECT original = rect;
            const int length = std::strlen(rows[i]);
            CHECK(c.Text(&c.memory,rows[i],length,kRowFlags,&rect) == 15);
            CHECK(BomeTest::textDc == &c.memory && BomeTest::textFlags == kRowFlags);
            CHECK(BomeTest::wideText == std::wstring(rows[i],rows[i]+length));
            CHECK(BomeTest::wideCount == length && std::memcmp(&rect,&original,sizeof(rect)) == 0);
            CHECK(Test::alphaFont == &c.font && Test::alphaTextColor == c.memory.text);
            CHECK(Test::alphaBkMode == TRANSPARENT && c.memory.saved.empty());
            RECT remainder{600,i*16,981,(i+1)*16}; CHECK(c.Fill(&c.memory,&remainder) == 1);
        }
        CHECK(Test::alphaText == 10 && Test::black == 20 && BomeTest::rawTexts == 0 && BomeTest::rawFills == 0);
        CHECK(c.memory.background == 0xffffff && c.memory.bkMode == OPAQUE && c.memory.font == &c.font);
        // Capture: the gutter is a separate BeginPaint, not the memo's memory DC.
        c.window.cls = L"TBMGutter"; c.dc.background = c.brush.lbColor = 0xf0f0f0;
        for (int i = 0; i < 10; ++i)
        {
            const auto number = std::to_string(i+1); RECT rect{2,i*16,32,(i+1)*16};
            CHECK(c.Fill(&c.dc,&rect) == 1);
            CHECK(c.Text(&c.dc,number.c_str(),number.size(),kRowFlags,&rect) == 15);
            CHECK(BomeTest::textDc == &c.dc && BomeTest::textRect.left == 2);
        }
        CHECK(Test::alphaText == 20 && Test::black == 40);
        CHECK(c.dc.background == 0xf0f0f0 && c.dc.font == &c.font && c.dc.saved.empty());
    }
    void NativeSemantics()
    {
        Context c; RECT rect{}; char mutableText[] = "row";
        CHECK(c.Text(&c.dc,mutableText,-1,DT_CALCRECT,&rect) == 41 && rect.right == 13);
        rect = RECT{0,0,981,16};
        CHECK(c.Text(&c.dc,mutableText,-1,kRowFlags | DT_CALCRECT,&rect) == 41 && rect.right == 994);
        CHECK(c.Text(&c.dc,mutableText,-1,kRowFlags | DT_MODIFYSTRING,&rect) == 41 && mutableText[0] == '#');
        CHECK(BomeTest::nativeString == mutableText && BomeTest::nativeRect == &rect && BomeTest::nativeCount == -1);
        CHECK(c.Text(&c.dc,mutableText,3,kRowFlags | DT_NOCLIP,&rect) == 41);
        CHECK(c.Text(&c.dc,mutableText,3,DT_NOPREFIX,&rect) == 41);
        CHECK(c.Text(&c.dc,nullptr,1) == 41 && c.Text(&c.dc,"row",0) == 41 && c.Text(&c.dc,"row",-2) == 41);
        RECT invalid{7,0,6,16}; CHECK(c.Text(&c.dc,"row",3,kRowFlags,&invalid) == 41);
        RECT huge{(std::numeric_limits<LONG>::min)(),0,(std::numeric_limits<LONG>::max)(),1};
        CHECK(c.Text(&c.dc,"row",3,kRowFlags,&huge) == 41 && c.Fill(&c.dc,&huge) == 37);
        CHECK(Test::alphaText == 0 && Test::black == 0 && BomeTest::conversions == 0);
        CHECK(c.Text(&c.dc,"",-1) == 41 && Test::alphaText == 0);
    }
    void AnsiCodePages()
    {
        Context c;
        BomeTest::dcs[&c.dc].charset = 204;
        CHECK(c.Text(&c.dc,"A\xc0",-1) == 15);
        CHECK(BomeTest::conversionPage == 1251 && BomeTest::wideText == L"A\u0410" && BomeTest::wideCount == 2);
        BomeTest::dcs[&c.dc].charset = 128;
        const char dbcs[]{char(0x82),char(0xa0),'X',0};
        CHECK(c.Text(&c.dc,dbcs,2) == 15);
        CHECK(BomeTest::conversionPage == 932 && BomeTest::conversionCount == 2 && BomeTest::conversionFlags == 0);
        CHECK(BomeTest::wideText == L"\u3042" && BomeTest::wideCount == 1);
        BomeTest::dcs[&c.dc].charset = DEFAULT_CHARSET;
        CHECK(c.Text() == 15 && BomeTest::conversionPage == CP_ACP);
        for (int charset : {SYMBOL_CHARSET,-1})
        { BomeTest::dcs[&c.dc].charset = charset; CHECK(c.Text() == 41); }
        BomeTest::dcs[&c.dc].charset = 0; BomeTest::failCharset = true;
        CHECK(c.Text() == 41); BomeTest::failCharset = false;
        for (int failing : {1,2})
        {
            BomeTest::conversions = 0; BomeTest::failConversionCall = failing;
            CHECK(c.Text() == 41);
        }
        BomeTest::failConversionCall = 0; CHECK(c.Text() == 15);
    }
    void BackgroundScope()
    {
        Context c;
        CHECK(c.Fill() == 1);
        for (COLORREF color : {0u,0xf0f0f0u,RGB(30,60,90)})
        {
            c.brush.lbColor = c.dc.background = color;
            CHECK(c.Fill() == 37 && c.Text() == 41);
        }
        c.window.cls = L"TBMGutter"; c.brush.lbColor = c.dc.background = 0xf0f0f0;
        CHECK(c.Fill() == 1 && c.Text() == 15);
        CHECK(c.Fill(&c.dc,nullptr,reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_WINDOW+1))) == 1);
        CHECK(c.Fill(&c.dc,nullptr,reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_BTNFACE+1))) == 1);
        c.brush.lbStyle = BS_HATCHED; CHECK(c.Fill() == 37 && BomeTest::nativeBrush == &c.brush);
        c.brush.lbStyle = BS_SOLID; c.dc.background = RGB(20,30,40); c.dc.bkMode = TRANSPARENT;
        const int black = Test::black; CHECK(c.Text() == 15 && Test::black == black);
        BomeTest::windowColor = RGB(10,20,30); BomeTest::faceColor = RGB(40,50,60);
        c.brush.lbColor = 0xffffff; CHECK(c.Fill() == 37);
        c.brush.lbColor = BomeTest::windowColor; CHECK(c.Fill() == 1);
        c.brush.lbColor = BomeTest::faceColor; CHECK(c.Fill() == 1);
        c.window.cls = L"TBomeMemo"; CHECK(c.Fill() == 37);
        CHECK(c.Fill(&c.dc,nullptr,reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_WINDOW+1))) == 1);
    }
    void DcAndModeGates()
    {
        Context c; TestDC unowned;
        auto blocked = [&](HDC dc) { CHECK(c.Text(dc) == 41 && c.Fill(dc) == 37); };
        blocked(&unowned); blocked(&c.foreign); blocked(&c.nc);
        Test::stackOwners[&unowned] = &c.other; blocked(&unowned);
        Test::full = false; blocked(&c.dc); Test::full = true;
        c.window.backdrop = false; blocked(&c.dc); c.window.backdrop = true;
        c.window.valid = false; blocked(&c.dc); c.window.valid = true;
        BomeTest::paints.clear(); blocked(&c.memory); BomeTest::paints[&c.window] = &c.dc;
        Test::topWindow = &c.other; blocked(&c.dc); Test::topWindow = &c.window;
        auto& info = BomeTest::dcs[&c.dc];
        info.mapMode = 2; blocked(&c.dc); info.mapMode = MM_TEXT;
        info.graphics = 2; blocked(&c.dc); info.graphics = GM_COMPATIBLE;
        info.layout = 1; blocked(&c.dc); info.layout = 0;
        info.viewport.x = 1; blocked(&c.dc); info.viewport.x = 0;
        info.window.y = -1; blocked(&c.dc); info.window.y = 0;
        info.originOk = false; blocked(&c.dc); info.originOk = true;
        auto& mem = BomeTest::dcs[&c.memory];
        mem.bitmap.bmBitsPixel = 24; blocked(&c.memory); mem.bitmap.bmBitsPixel = 32;
        mem.bitmap.bmPlanes = 2; blocked(&c.memory); mem.bitmap.bmPlanes = 1;
        mem.bitmap.bmWidth = 0; blocked(&c.memory); mem.bitmap.bmWidth = 981;
        mem.bitmapPresent = false; blocked(&c.memory); mem.bitmapPresent = true;
        mem.bitmapReadable = false; blocked(&c.memory); mem.bitmapReadable = true;
        CHECK(Test::alphaText == 0 && Test::black == 0 && BomeTest::conversions == 0);
        CHECK(c.Text(&c.dc) == 15 && c.Text(&c.memory) == 15);
    }
    void FailuresAndReentry()
    {
        Context c;
        BomeTest::failSave = true;
        CHECK(c.Text() == 41 && Test::black == 0 && BomeTest::nativeBkMode == OPAQUE);
        BomeTest::failSave = false; BomeTest::failMode = true;
        CHECK(c.Text() == 41 && Test::black == 0 && BomeTest::nativeBkMode == OPAQUE);
        BomeTest::failMode = false; BomeTest::failBlack = true;
        CHECK(c.Text() == 41 && c.Fill() == 37 && Test::alphaText == 0);
        CHECK(BomeTest::nativeBkMode == OPAQUE && c.dc.saved.empty());
        BomeTest::failBlack = false; Test::alphaResult = E_FAIL;
        CHECK(c.Text() == 41 && BomeTest::nativeBkMode == TRANSPARENT);
        CHECK(c.dc.bkMode == OPAQUE && c.dc.saved.empty()); Test::alphaResult = S_OK;
        Test::duringAlpha = [&] { CHECK(c.Text() == 41 && c.Fill() == 37); };
        const int alpha = Test::alphaText; CHECK(c.Text() == 15 && Test::alphaText == alpha+1);
        Test::duringAlpha = [] { throw std::runtime_error("render failure"); };
        CHECK(c.Text() == 41); Test::duringAlpha = {};
        CHECK(c.Text() == 15); // guard unwound on the throwing path, too.
        BomeTest::duringBlack = [&] { CHECK(c.Fill() == 37); };
        CHECK(c.Fill() == 1); BomeTest::duringBlack = {};
        CHECK(c.dc.background == 0xffffff && c.dc.bkMode == OPAQUE && c.dc.font == &c.font);
    }
    void SlotOriginalAndShutdown()
    {
        Context c; c.dc.background = 0x123456; c.brush.lbColor = 0x123456;
        CHECK(c.Text() == 41 && c.Fill() == 37);
        CHECK(BomeTest::chainTexts == 1 && BomeTest::chainFills == 1 && BomeTest::originalModule == &c.module);
        c.module.originals.erase("DrawTextA"); CHECK(c.Text() == 41 && BomeTest::chainTexts == 1);
        c.module.originals["DrawTextA"] = reinterpret_cast<void*>(ChainText);
        GlassCore::Root::g_shutdown.store(true);
        c.dc.background = c.brush.lbColor = 0xffffff;
        CHECK(c.Text() == 41 && c.Fill() == 37 && Test::alphaText == 0 && Test::black == 0);
        GlassCore::Root::g_shutdown.store(false);
        CHECK(c.Text() == 15 && c.Fill() == 1);
        CHECK(GlassCore::DelphiBomeMemo::DrawAnsiText(nullptr,&c.dc,"row",3,nullptr,kRowFlags) == 41);
    }
}
int main(int argc, char** argv)
{
    try
    {
        CHECK(argc == 2); const std::string test = argv[1];
        if (test == "classes") ExactClassRoute();
        else if (test == "captured-rows") CapturedMemoAndGutter();
        else if (test == "native-semantics") NativeSemantics();
        else if (test == "ansi") AnsiCodePages();
        else if (test == "backgrounds") BackgroundScope();
        else if (test == "dc-gates") DcAndModeGates();
        else if (test == "failures") FailuresAndReentry();
        else if (test == "originals") SlotOriginalAndShutdown();
        else throw std::runtime_error("Unknown case: " + test);
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
