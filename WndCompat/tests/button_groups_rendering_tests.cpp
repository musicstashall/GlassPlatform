#include "button_groups_test_shim.h"
#include "../../GlassCore/src/Hooks/delphi/TButtonGroupsDraw.cpp"
#include <root_button_groups_test.inc>
#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)
namespace
{
    int WINAPI ChainText(HDC dc, LPWSTR text, int count, LPRECT rect, UINT flags, LPDRAWTEXTPARAMS params)
    { ++GroupTest::chainText; return GroupNativeText(dc, text, count, rect, flags, params); }
    int WINAPI ChainFill(HDC dc, const RECT* rect, HBRUSH brush)
    { ++GroupTest::chainFill; return GroupNativeFill(dc, rect, brush); }
    BOOL WINAPI ChainGradient(HDC dc, PTRIVERTEX v, ULONG vc, PVOID mesh, ULONG mc, ULONG mode)
    { ++GroupTest::chainGradient; return GdiGradientFill(dc, v, vc, mesh, mc, mode); }
    BOOL WINAPI ChainGdiGradient(HDC dc, PTRIVERTEX v, ULONG vc, PVOID mesh, ULONG mc, ULONG mode)
    { ++GroupTest::chainGdiGradient; return GdiGradientFill(dc, v, vc, mesh, mc, mode); }
    struct Context
    {
        TestModule module;
        TestWindow window, other;
        TestDC dc, foreign, memory;
        RECT client{0,0,236,96};
        LOGBRUSH brush{BS_SOLID, RGB(48,96,192)};
        GlassCore::Hooks::HookDispatcher<4,3> dispatcher;
        Context() : dispatcher(GlassCore::Root::GroupDependenciesForTest, &module)
        {
            window.cls = L"TButtonGroup"; other.cls = L"TPanel";
            dc.owner = &window; foreign.owner = &other;
            GroupTest::clients[&window] = client;
            GlassCore::ModuleHandler::g_entryPointModule = &module;
            Test::topWindow = Test::host = &window;
            module.originals = {{"DrawTextExW", reinterpret_cast<void*>(ChainText)},
                {"FillRect", reinterpret_cast<void*>(ChainFill)},
                {"GradientFill", reinterpret_cast<void*>(ChainGradient)},
                {"GdiGradientFill", reinterpret_cast<void*>(ChainGdiGradient)}};
            for (size_t i = 0; i < 4; ++i) dispatcher.EnableHook(i, true);
        }
        ~Context()
        {
            dispatcher.DisableAllHooks();
            assert(module.hooks.empty());
            for (const auto& [name, ref] : module.refs) { (void)name; assert(!ref); }
        }
        template <typename Fn, typename... Args> auto Call(const char* name, Fn, Args... args)
        { return reinterpret_cast<Fn>(module.hooks.at(name))(args...); }
        int Fill(const RECT* rect = nullptr, HDC target = nullptr, HBRUSH b = nullptr)
        { return Call("FillRect", &::FillRect, target ? target : &dc, rect ? rect : &client, b ? b : &brush); }
        int Text(HDC target = nullptr, UINT flags = 0x8000, LPDRAWTEXTPARAMS params = nullptr)
        {
            static wchar_t text[] = L"One";
            RECT rect{6,6,21,26};
            return Call("DrawTextExW", &::DrawTextExW, target ? target : &dc, text, 3, &rect, flags, params);
        }
        BOOL Gradient(bool gdi = false, HDC target = nullptr, ULONG mode = GRADIENT_FILL_RECT_H)
        {
            TRIVERTEX vertices[]{{1,1,0,0x8000,0xff00,0}, {235,95,0xff00,0x4000,0,0}};
            GRADIENT_RECT pair{0,1};
            return Call(gdi ? "GdiGradientFill" : "GradientFill", &::GdiGradientFill,
                target ? target : &dc, vertices, 2u, &pair, 1u, mode);
        }
    };
    void ClassGateAndSlots()
    {
        using namespace GlassCore::Root;
        Context c; TestModule notExe;
        CHECK(!GlassShared::IsDelphiButtonGroupsClassName(nullptr));
        for (LPCWSTR name : {L"TButtonGroup", L"TCategoryButtons", L"tcategorybuttons"})
        {
            c.window.cls = name;
            CHECK(GlassShared::IsDelphiButtonGroupsWindow(&c.window));
            const auto specs = GroupSpecsForTest(&c.module, name);
            CHECK(specs.paintHooks.size() == 4 && specs.paintHooks == specs.dcHooks);
            for (size_t i = 0; i < 4; ++i)
            { CHECK(specs.paintHooks[i].first == &c.module); CHECK(specs.paintHooks[i].second == OHK_DELPHI_GROUP_TEXT + i); }
            CHECK(GroupSpecsForTest(&notExe, name).paintHooks.empty());
            const int alpha = Test::alphaText;
            CHECK(c.Text() == 15 && Test::alphaText == alpha+1);
            CHECK(GroupTest::originalModule == &c.module);
        }
        for (LPCWSTR name : {L"ButtonGroup", L"MyTCategoryButtons", L"TButtonGroup2", L"TButton",
            L"TPanel", L"TFrame", L"TCoolBar", L"TRadioGroup", L"TGroupButton", L""})
        {
            c.window.cls = name;
            CHECK(!GlassShared::IsDelphiButtonGroupsWindow(&c.window));
            const auto specs = GroupSpecsForTest(&c.module, name);
            CHECK(specs.paintHooks.empty() && specs.dcHooks.empty());
            const int alpha = Test::alphaText, buffers = GroupTest::bufferCalls;
            CHECK(c.Text() == 43 && c.Fill() == 37 && c.Gradient());
            CHECK(Test::alphaText == alpha && GroupTest::bufferCalls == buffers);
        }
    }
    void ProbeTextAndNativeParameters()
    {
        Context c;
        struct Probe { LPCWSTR name, text; int count; UINT flags; RECT rect; };
        const Probe probes[]{
            {L"TButtonGroup", L"One", 3, 0x8000, {6,6,21,26}},
            {L"TButtonGroup", L"Two", 3, 0x8000, {30,6,45,26}},
            {L"TButtonGroup", L"Three", 5, 0x8000, {54,6,69,26}},
            {L"TCategoryButtons", L"TCategoryButtons", 16, 0x8100, {19,2,236,17}},
            {L"TCategoryButtons", L"Alpha", 5, 0x8004, {9,25,25,42}},
            {L"TCategoryButtons", L"Beta", 4, 0x8004, {33,25,49,42}},
        };
        TestFont font{93}; c.dc.font = &font; c.dc.text = RGB(22,44,66);
        for (const auto& p : probes)
        {
            c.window.cls = p.name; RECT rect = p.rect;
            std::wstring mutableText = p.text;
            const int black = Test::black;
            CHECK(c.Call("DrawTextExW", &::DrawTextExW, &c.dc, mutableText.data(), p.count, &rect, p.flags, nullptr) == 15);
            CHECK(GroupTest::alphaString == mutableText.data() && GroupTest::alphaCount == p.count);
            CHECK(GroupTest::alphaFlags == p.flags && Test::alphaTextColor == c.dc.text && Test::alphaFont == &font);
            CHECK(std::memcmp(&GroupTest::alphaRect, &p.rect, sizeof(rect)) == 0);
            CHECK(std::memcmp(&rect, &p.rect, sizeof(rect)) == 0 && mutableText == p.text);
            CHECK(black == Test::black); // Caption must not clear the gradient it overlays.
        }
        CHECK(GroupTest::chainText == 0);
        DRAWTEXTPARAMS params{sizeof(params)};
        CHECK(c.Text(nullptr, 0x8000, &params) == 43);
        CHECK(c.Text(nullptr, DT_MODIFYSTRING) == 43);
        RECT rect{0,0,20,10}; wchar_t text[] = L"measure";
        CHECK(c.Call("DrawTextExW", &::DrawTextExW, &c.dc, text, -1, &rect, DT_CALCRECT, nullptr) == 43);
        CHECK(rect.right == 27 && GroupTest::chainText == 3);
        const int before = Test::alphaText;
        RECT empty{};
        CHECK(c.Call("DrawTextExW", &::DrawTextExW, &c.dc, text, 0, &rect, 0u, nullptr) == 43);
        CHECK(c.Call("DrawTextExW", &::DrawTextExW, &c.dc, text, -1, &empty, 0u, nullptr) == 43);
        CHECK(c.Call("DrawTextExW", &::DrawTextExW, &c.dc, static_cast<LPWSTR>(nullptr), 0, &rect, 0u, nullptr) == 43);
        CHECK(Test::alphaText == before);
    }
    void FillPoliciesAndRepaints()
    {
        Context c;
        for (LPCWSTR cls : {L"TButtonGroup", L"TCategoryButtons"})
        {
            c.window.cls = cls;
            for (COLORREF rgb : {GetSysColor(COLOR_BTNFACE), GetSysColor(COLOR_WINDOW)})
            {
                c.brush.lbColor = rgb; const int buffers = GroupTest::bufferCalls, black = Test::black;
                CHECK(c.Fill() == 1 && Test::black == black+1 && GroupTest::bufferCalls == buffers);
            }
            // Actual item rectangles from the probe must take the same policy.
            const RECT item = std::wstring_view(cls) == L"TButtonGroup" ? RECT{2,1,22,27} : RECT{5,22,27,44};
            CHECK(c.Fill(&item) == 1);
            for (COLORREF rgb : {RGB(48,96,192), RGB(0,0,0), RGB(255,0,0)})
            {
                c.brush.lbColor = rgb;
                const int black = Test::black, commits = GroupTest::commits;
                CHECK(c.Fill() == 1 && c.Fill() == 1);
                CHECK(Test::black == black+2 && GroupTest::commits == commits+2);
                CHECK(GroupTest::constantAlpha == 100 && !GroupTest::bufferErase);
                for (const auto& p : GroupTest::committed)
                    CHECK(p.rgbRed == (rgb & 255) && p.rgbGreen == ((rgb >> 8) & 255)
                        && p.rgbBlue == ((rgb >> 16) & 255) && p.rgbReserved == 255);
            }
        }
        // Dynamic system colours: white is custom when COLOR_WINDOW is not white.
        GroupTest::windowColor = RGB(20,30,40); GroupTest::faceColor = RGB(60,70,80);
        c.brush.lbColor = RGB(255,255,255);
        const int commits = GroupTest::commits;
        CHECK(c.Fill() == 1 && GroupTest::commits == commits+1);
        const auto encoded = reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_WINDOW+1));
        CHECK(c.Fill(nullptr, nullptr, encoded) == 1 && GroupTest::commits == commits+1);
        CHECK(GroupTest::lastBrush == &c.brush);
    }
    void NativeGradientAndBothImports()
    {
        Context c; c.window.cls = L"TCategoryButtons";
        for (bool gdi : {false, true})
            for (ULONG mode : {GRADIENT_FILL_RECT_H, GRADIENT_FILL_RECT_V})
            {
                TRIVERTEX vertices[]{{1,1,0,0,0,0}, {235,95,0x1200,0x3400,0x5600,0}};
                const auto before = std::array{vertices[0],vertices[1]};
                GRADIENT_RECT pair{0,1};
                const int black = Test::black, commits = GroupTest::commits;
                CHECK(c.Call(gdi ? "GdiGradientFill" : "GradientFill", &::GdiGradientFill,
                    &c.dc, vertices, 2u, &pair, 1u, mode));
                CHECK(GroupTest::commits == commits+1 && Test::black == black+1);
                CHECK(GroupTest::lastVertices == vertices && GroupTest::lastMesh == &pair);
                CHECK(GroupTest::lastVertexCount == 2 && GroupTest::lastMeshCount == 1 && GroupTest::lastMode == mode);
                CHECK(std::memcmp(vertices, before.data(), sizeof(vertices)) == 0);
                CHECK(pair.UpperLeft == 0 && pair.LowerRight == 1);
                CHECK(GroupTest::bufferRect.left == 1 && GroupTest::bufferRect.top == 1
                    && GroupTest::bufferRect.right == 235 && GroupTest::bufferRect.bottom == 95);
                CHECK(GroupTest::constantAlpha == 100 && !GroupTest::bufferErase);
                CHECK(GroupTest::committed.front().rgbRed == 0 && GroupTest::committed.front().rgbReserved == 255);
                CHECK(GroupTest::committed.back().rgbRed == 0x12 && GroupTest::committed.back().rgbGreen == 0x34
                    && GroupTest::committed.back().rgbBlue == 0x56 && GroupTest::committed.back().rgbReserved == 255);
                CHECK(GroupTest::originalModule == &c.module);
            }
        CHECK(GroupTest::chainGradient == 2 && GroupTest::chainGdiGradient == 2);
        CHECK(GroupTest::flushes == 4 && GroupTest::alphaCalls == 4);
    }
    void ObservedCategoryBackgroundStates()
    {
        // LogViewer-18: API inputs, not an emulation of VCL state transitions.
        // The later captured paint supplied a solid gray fill, not a pink gradient.
        Context c; c.window.cls = L"TCategoryButtons";
        TestDC reopened; reopened.owner = &c.window;
        const RECT category{2,1,234,44};
        TRIVERTEX vertices[]{{2,1,0xff00,0xea00,0xff00,0}, {234,44,0xf000,0xf000,0xf000,0}};
        const auto before = std::array{vertices[0],vertices[1]};
        GRADIENT_RECT pair{0,1};
        LOGBRUSH face{BS_SOLID, RGB(240,240,240)}, gray{BS_SOLID, RGB(230,230,230)},
            window{BS_SOLID, RGB(255,255,255)};
        for (bool solid : {false, true})
        {
            HDC dc = solid ? &reopened : &c.dc;
            const int buffers = GroupTest::bufferCalls, commits = GroupTest::commits,
                alpha = GroupTest::alphaCalls, black = Test::black,
                gradients = GroupTest::chainGradient;
            CHECK(c.Fill(nullptr, dc, &face) == 1);
            CHECK(GroupTest::bufferCalls == buffers && Test::black == black+1);
            if (solid)
                CHECK(c.Fill(&category, dc, &gray) == 1);
            else
                CHECK(c.Call("GradientFill", &::GdiGradientFill, dc,
                    vertices, 2u, &pair, 1u, GRADIENT_FILL_RECT_H));
            CHECK(GroupTest::bufferCalls == buffers+1 && GroupTest::commits == commits+1);
            CHECK(GroupTest::alphaCalls == alpha+1 && GroupTest::constantAlpha == 100);
            CHECK(Test::black == black+2 && !GroupTest::bufferErase);
            CHECK(GroupTest::chainGradient == gradients + (solid ? 0 : 1));
            CHECK(std::memcmp(&GroupTest::bufferRect, &category, sizeof(category)) == 0);
            if (solid)
            {
                for (const auto& p : GroupTest::committed)
                    CHECK(p.rgbRed == 230 && p.rgbGreen == 230 && p.rgbBlue == 230 && p.rgbReserved == 255);
            }
            else
            {
                const auto& first = GroupTest::committed.front();
                const auto& last = GroupTest::committed.back();
                CHECK(first.rgbRed == 255 && first.rgbGreen == 234 && first.rgbBlue == 255 && first.rgbReserved == 255);
                CHECK(last.rgbRed == 240 && last.rgbGreen == 240 && last.rgbBlue == 240 && last.rgbReserved == 255);
                CHECK(GroupTest::lastVertices == vertices && GroupTest::lastMesh == &pair);
                CHECK(GroupTest::lastMode == GRADIENT_FILL_RECT_H);
            }
            for (const RECT item : {RECT{5,22,27,44}, RECT{29,22,51,44}})
                CHECK(c.Fill(&item, dc, &window) == 1);
            CHECK(GroupTest::bufferCalls == buffers+1 && GroupTest::commits == commits+1);
            CHECK(Test::black == black+4);
        }
        CHECK(std::memcmp(vertices, before.data(), sizeof(vertices)) == 0);
        CHECK(GroupTest::chainGradient == 1 && GroupTest::chainFill == 1);
    }
    void DcModeAndBackdropIsolation()
    {
        Context c;
        const auto native = [&](HDC target) {
            const int alpha = Test::alphaText, buffers = GroupTest::bufferCalls;
            CHECK(c.Text(target) == 43 && c.Fill(nullptr,target) == 37 && c.Gradient(false,target));
            CHECK(Test::alphaText == alpha && GroupTest::bufferCalls == buffers);
        };
        native(&c.foreign); native(&c.memory);
        Test::stackOwners[&c.memory] = &c.window;
        CHECK(c.Text(&c.memory) == 15 && c.Fill(nullptr,&c.memory) == 1 && c.Gradient(false,&c.memory));
        Test::topWindow = &c.other; native(&c.dc); native(&c.memory); Test::topWindow = &c.window;
        c.window.backdrop = false; native(&c.dc); c.window.backdrop = true;
        Test::full = false; native(&c.dc); Test::full = true;
        GroupTest::mapModes[&c.dc] = 2; native(&c.dc); GroupTest::mapModes.clear();
        c.window.valid = false; native(&c.dc); c.window.valid = true;
        GlassCore::Root::g_shutdown = true; native(&c.dc); GlassCore::Root::g_shutdown = false;
        CHECK(c.Text() == 15 && c.Fill() == 1 && c.Gradient());
    }
    void UnsupportedGeometryAndBrushes()
    {
        Context c;
        const int buffers = GroupTest::bufferCalls;
        for (const RECT& r : {RECT{0,0,236,1}, RECT{2,2,3,20}, RECT{-1,0,100,30},
            RECT{1,1,237,90}, RECT{}, RECT{4,4,4,10}})
            CHECK(c.Fill(&r) == 37);
        LOGBRUSH hatch{2, RGB(25,50,75)};
        CHECK(c.Fill(nullptr,nullptr,&hatch) == 37);
        TRIVERTEX vertices[]{{1,1,0,0,0,0}, {235,95,0xff00,0xff00,0xff00,0}};
        GRADIENT_RECT pair{0,1};
        auto call = [&](PTRIVERTEX v, ULONG vc, PVOID mesh, ULONG mc, ULONG mode) {
            return c.Call("GradientFill", &::GdiGradientFill, &c.dc, v, vc, mesh, mc, mode);
        };
        CHECK(call(nullptr,2,&pair,1,0) && call(vertices,1,&pair,1,0));
        CHECK(call(vertices,2,nullptr,1,0) && call(vertices,2,&pair,2,0));
        CHECK(call(vertices,2,&pair,1,GRADIENT_FILL_TRIANGLE));
        pair.LowerRight = 2; CHECK(call(vertices,2,&pair,1,0));
        pair.LowerRight = 0; CHECK(call(vertices,2,&pair,1,0));
        pair.LowerRight = 1; vertices[0].x = -1; CHECK(call(vertices,2,&pair,1,0));
        CHECK(GroupTest::bufferCalls == buffers);
    }
    void FailureFallbackAndReentry()
    {
        Context c;
        for (bool* fail : {&GroupTest::failBuffer, &GroupTest::failAlpha, &GroupTest::failDraw,
            &GroupTest::failFlush, &GroupTest::failBlack})
        {
            const int commits = GroupTest::commits, black = Test::black;
            *fail = true;
            CHECK(c.Fill() == (fail == &GroupTest::failDraw ? 0 : 37));
            CHECK(c.Gradient() == (fail == &GroupTest::failDraw ? FALSE : TRUE));
            *fail = false;
            CHECK(GroupTest::commits == commits && Test::black == black);
            CHECK(GroupTest::lastNativeDc == &c.dc);
            CHECK(GroupTest::buffers.empty());
        }
        Test::alphaResult = E_FAIL; CHECK(c.Text() == 43); Test::alphaResult = S_OK;
        const int buffers = GroupTest::bufferCalls, alpha = Test::alphaText;
        GroupTest::duringBuffer = [&] { CHECK(c.Fill() == 37 && c.Gradient() && c.Text() == 43); };
        CHECK(c.Fill() == 1 && GroupTest::bufferCalls == buffers+1 && Test::alphaText == alpha);
        GroupTest::duringBuffer = {};
        Test::duringAlpha = [&] { CHECK(c.Text() == 43 && c.Fill() == 37); };
        CHECK(c.Text() == 15 && Test::alphaText == alpha+1);
        Test::duringAlpha = {};
        CHECK(c.Fill() == 1 && c.Text() == 15 && c.Gradient());
    }
    void SlotOriginalsAndMissingImports()
    {
        using namespace GlassCore::Root;
        Context c;
        CHECK(c.Text() == 15 && GroupTest::originalModule == &c.module);
        CHECK(c.Fill() == 1 && c.Gradient(true));
        // Missing GradientFill must not take GdiGradientFill's chain as its original.
        c.dispatcher.EnableHook(2,false);
        const int chain = GroupTest::chainGradient, gdi = GroupTest::chainGdiGradient;
        TRIVERTEX vertices[]{{1,1,0,0,0,0},{235,95,0xff00,0,0,0}};
        GRADIENT_RECT pair{0,1};
        const int buffers = GroupTest::bufferCalls;
        CHECK(Root_MyDelphiGroupGradient<OHK_DELPHI_GROUP_GRADIENT>(&c.dc,vertices,2,&pair,1,0));
        CHECK(GroupTest::chainGradient == chain && GroupTest::chainGdiGradient == gdi);
        CHECK(GroupTest::bufferCalls == buffers);
        CHECK(c.Gradient(true) && GroupTest::chainGdiGradient == gdi+1);
        for (size_t slot : {0u,1u,3u}) c.dispatcher.EnableHook(slot,false);
        const int text = Test::nativeText, fill = Test::nativeFill;
        RECT rect{0,0,20,15}; wchar_t label[] = L"One";
        CHECK(Root_MyDelphiGroupText(&c.dc,label,3,&rect,0,nullptr) == 18);
        CHECK(Root_MyDelphiGroupFill(&c.dc,&rect,&c.brush) > 0);
        CHECK(Test::nativeText == text+1 && Test::nativeFill == fill+1);
        CHECK(GroupTest::bufferCalls == buffers+1); // Only the last GdiGradient call buffered.
    }
}
int main(int argc, char** argv)
{
    const std::unordered_map<std::string, void(*)()> cases{
        {"class-gate",ClassGateAndSlots}, {"probe-text",ProbeTextAndNativeParameters},
        {"fills",FillPoliciesAndRepaints}, {"gradients",NativeGradientAndBothImports},
        {"category-states",ObservedCategoryBackgroundStates},
        {"dc-isolation",DcModeAndBackdropIsolation}, {"unsupported",UnsupportedGeometryAndBrushes},
        {"failures",FailureFallbackAndReentry}, {"slot-originals",SlotOriginalsAndMissingImports},
    };
    try { CHECK(argc == 2); cases.at(argv[1])(); std::cout << argv[1] << ": OK\n"; return 0; }
    catch (const std::exception& e) { std::cerr << argv[1] << ": " << e.what() << '\n'; return 1; }
}
