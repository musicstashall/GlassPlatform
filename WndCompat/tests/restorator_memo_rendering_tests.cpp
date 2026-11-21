#include "restorator_memo_test_shim.h"
#include <GlassDelphiPanel.h>
#include <GlassDelphiButtonGroups.h>
#include "../../GlassCore/src/Hooks/delphi/RestoratorMemoDraw.cpp"
#include <root_restorator_memo_test.inc>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
namespace
{
    using namespace GlassCore;
    constexpr UINT kFlags = DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER;
    bool Same(const RECT& a, const RECT& b) { return std::memcmp(&a,&b,sizeof(a)) == 0; }
    struct Context
    {
        TestModule module, otherModule;
        TestWindow window, gutter, other;
        TestDC dc, memory, gutterDc, foreign, unknown;
        RECT rect{0,0,640,16};
        LOGBRUSH brush{BS_SOLID, 0xffffff};
        Context()
        {
            ModuleHandler::g_entryPointModule = &module;
            MemoTest::exePath = LR"(C:\Tools\Restorator.exe)";
            Test::full = true; Root::g_shutdown = false;
            window.cls = L"TBomeMemo"; gutter.cls = L"TBMGutter"; other.cls = L"TPanel";
            dc.owner = &window; gutterDc.owner = &gutter; foreign.owner = &other;
            module.originals = {{"DrawTextA", reinterpret_cast<void*>(ChainAnsi)},
                {"CreateBrushIndirect", reinterpret_cast<void*>(ChainBrush)},
                {"GetSysColor", reinterpret_cast<void*>(ChainSysColor)},
                {"FillRect", reinterpret_cast<void*>(ChainFill)}};
            Root::s_rootCache[&window] = Root::MemoSpecsForTest(&window,&module);
            Root::s_rootCache[&gutter] = Root::MemoSpecsForTest(&gutter,&module);
            CHECK(Root::s_rootCache[&window].restoratorMemo && Root::s_rootCache[&gutter].restoratorMemo);
            Test::host = Test::topWindow = nullptr; Test::stackOwners.clear();
        }
        ~Context()
        {
            while (!Root::g_rootPaintStack.empty()) Root::Root_OnEndPaint(Root::g_rootPaintStack.back().hWnd,&module);
            while (!Root::g_rootDcStack.empty()) Root::Root_OnReleaseDC(Root::g_rootDcStack.back().hWnd,&module);
            RestoratorMemo::ClearSessions(); Root::DisableAllRootHooks(); Root::s_rootCache.clear();
            for (const auto& [name, ref] : module.refs) { (void)name; assert(!ref); }
            assert(module.hooks.empty() && !MemoTest::formAttachCalls);
            Test::stackOwners.clear(); Test::host = Test::topWindow = nullptr;
        }
        void Activate(HWND host, HDC target)
        { Test::host = Test::topWindow = host; Test::stackOwners[target] = host; }
        void Begin(bool paint = true)
        {
            if (paint) Root::Root_OnBeginPaint(&window,&module); else Root::Root_OnGetDC(&window,&module);
            Activate(&window,&dc); Test::stackOwners[&memory] = &window;
        }
        template <typename Fn, typename... Args> auto Call(const char* name, Fn, Args... args)
        {
            const auto it = module.hooks.find(name);
            return reinterpret_cast<Fn>(it == module.hooks.end() ? module.originals.at(name) : it->second)(args...);
        }
        int Text(HDC target = nullptr, LPCSTR text = "row", int count = -1, UINT flags = kFlags, LPRECT r = nullptr)
        { return Call("DrawTextA", &DrawTextA, target ? target : &dc, text, count, r ? r : &rect, flags); }
        int Fill(HDC target = nullptr, HBRUSH b = nullptr)
        { return Call("FillRect", &::FillRect, target ? target : &dc, &rect, b ? b : &brush); }
        DWORD Color(int index) { return Call("GetSysColor", &GetSysColor, index); }
        HBRUSH Brush(const LOGBRUSH* b) { return Call("CreateBrushIndirect", &CreateBrushIndirect, b); }
        int Ref(size_t offset = 0)
        { return Root::g_rootMap.at(&module).GetRef(Root::OHK_RESTORATOR_MEMO_TEXT + offset); }
    };
    void Classes()
    {
        Context c;
        CHECK(!RestoratorMemo::IsClassName(nullptr));
        for (LPCWSTR cls : {L"TBomeMemo",L"TBMGutter",L"tbomememo"})
        {
            c.window.cls = cls;
            const auto cache = Root::MemoSpecsForTest(&c.window,&c.module);
            CHECK(cache.restoratorMemo && cache.paintHooks.empty() && cache.dcHooks.empty());
            CHECK(!Root::MemoSpecsForTest(&c.window,&c.otherModule).restoratorMemo);
        }
        for (LPCWSTR cls : {L"TMemo",L"Edit",L"TRichEdit",L"MyTBomeMemo",L"TBomeMemo2",L"TBMGutterChild",L""})
        { c.window.cls = cls; CHECK(!RestoratorMemo::IsWindow(&c.window)); CHECK(!Root::MemoSpecsForTest(&c.window,&c.module).restoratorMemo); }
        c.window.cls = L"TBomeMemo"; MemoTest::exePath = LR"(C:\Tools\OtherRestorator.exe)";
        CHECK(!RestoratorMemo::IsWindow(&c.window));
        CHECK(!Root::MemoSpecsForTest(&c.window,&c.module).restoratorMemo);
        MemoTest::exePath.assign(MAX_PATH+1,L'x'); CHECK(!RestoratorMemo::IsWindow(&c.window));
        MemoTest::exePath = L"RESTORATOR.EXE"; CHECK(RestoratorMemo::IsWindow(&c.window));
        c.window.valid = false; CHECK(!RestoratorMemo::IsWindow(&c.window));
    }
    void Transaction()
    {
        Context c;
        const char* names[]{"DrawTextA","CreateBrushIndirect","GetSysColor","FillRect"};
        for (size_t missing = 0; missing < 4; ++missing)
        {
            void* original = c.module.originals.at(names[missing]);
            c.module.originals.erase(names[missing]);
            const unsigned mask = Root::AcquireRestoratorMemoHooks();
            CHECK(mask == (15u & ~(1u << missing)) && c.module.hooks.empty());
            for (size_t i=0;i<4;++i) CHECK(!c.Ref(i));
            c.module.originals[names[missing]] = original;
            c.module.hooks[names[missing]] = original; // foreign IAT owner
            CHECK(Root::AcquireRestoratorMemoHooks() == mask);
            CHECK(c.module.hooks.size() == 1 && c.module.hooks.at(names[missing]) == original);
            for (size_t i=0;i<4;++i) CHECK(!c.Ref(i));
            c.module.hooks.clear();
        }
        // A prior independently owned ref must survive a failed transaction.
        CHECK(Root::EnableRootHook(&c.module,Root::OHK_RESTORATOR_MEMO_TEXT,true));
        c.module.originals.erase("FillRect");
        CHECK(Root::AcquireRestoratorMemoHooks() == 7 && c.Ref() == 1 && c.module.hooks.size() == 1);
        CHECK(Root::EnableRootHook(&c.module,Root::OHK_RESTORATOR_MEMO_TEXT,false));
        c.module.originals["FillRect"] = reinterpret_cast<void*>(ChainFill);
        CHECK(Root::AcquireRestoratorMemoHooks() == 15);
        CHECK(Root::AcquireRestoratorMemoHooks() == 15); // nested EnableHook returns false, still acquired
        for (size_t i=0;i<4;++i) CHECK(c.Ref(i) == 2);
        Root::ReleaseRestoratorMemoHooks(); Root::ReleaseRestoratorMemoHooks();
        CHECK(c.module.hooks.empty());
    }
    void Paint()
    {
        Context c;
        Root::Root_OnBeginPaint(&c.window,&c.module);
        CHECK(c.module.hooks.size() == 4 && c.Ref() == 1);
        CHECK(c.Text() == 43); // returned BeginPaint DC is not on the dispatcher stack yet
        PAINTSTRUCT ps{&c.dc,TRUE,{8,16,420,80}};
        const int black = Test::black;
        CHECK(Root::Root_OnRestoratorMemoAfterBeginPaint(&c.window,&c.dc,&ps,&c.module));
        CHECK(Test::black == black+1 && Same(MemoTest::blackRect,ps.rcPaint));
        CHECK(ps.fErase == TRUE); // only PaintDispatcher owns resetting fErase on success
        c.Activate(&c.window,&c.dc); Test::stackOwners[&c.memory] = &c.window;
        CHECK(c.Text(&c.memory) == 15);
        Root::Root_OnEndPaint(&c.window,&c.module);
        CHECK(c.module.hooks.empty() && Root::g_rootPaintStack.empty());
        CHECK(!Root::Root_OnRestoratorMemoAfterBeginPaint(&c.window,&c.dc,&ps,&c.module));
        // Incomplete acquisition cannot erase and cannot keep a partial import.
        c.module.originals.erase("CreateBrushIndirect");
        Root::Root_OnBeginPaint(&c.window,&c.module);
        CHECK(c.module.hooks.empty() && c.Text() == 43);
        CHECK(!Root::Root_OnRestoratorMemoAfterBeginPaint(&c.window,&c.dc,&ps,&c.module));
        Root::Root_OnEndPaint(&c.window,&c.module);
    }
    void GetDc()
    {
        Context c; c.Begin(false);
        CHECK(c.module.hooks.size() == 4 && c.Text() == 15 && c.Text(&c.memory) == 15);
        CHECK(c.Color(COLOR_WINDOW) == 0 && c.Color(COLOR_BTNFACE) == 0);
        Root::Root_OnGetDC(&c.window,&c.otherModule); // native barrier over an armed same-HWND session
        CHECK(c.Ref() == 1 && c.Text() == 43 && c.Color(COLOR_WINDOW) == MemoTest::windowColor);
        Root::Root_OnReleaseDC(&c.window,&c.otherModule);
        CHECK(c.Ref() == 1 && c.Text() == 15);
        Root::Root_OnReleaseDC(&c.window,&c.module);
        CHECK(c.module.hooks.empty());
    }
    void TextLines()
    {
        Context c; c.Begin(); TestFont font{41}; c.memory.font = &font; c.memory.text = RGB(12,34,56);
        // Captured shape: ten source lines then ten line numbers, not recovered
        // RECT/string memory from the deleted capture. All twenty use 0x824.
        for (int row=0;row<20;++row)
        {
            const std::string text = row < 10 ? "<node value=\"sample\"/>" : std::to_string(row-9);
            RECT r{0,(row%10)*16,640,(row%10+1)*16}, before = r;
            const int black = Test::black;
            CHECK(c.Text(&c.memory,text.data(),int(text.size()),kFlags,&r) == 15);
            CHECK(MemoTest::alphaText == std::wstring(text.begin(),text.end()));
            CHECK(MemoTest::alphaFlags == kFlags && MemoTest::alphaCount == int(text.size()));
            CHECK(Same(r,before) && Same(MemoTest::alphaRect,before));
            CHECK(Test::black == black+1 && Test::alphaBkMode == TRANSPARENT);
            CHECK(Test::alphaTextColor == c.memory.text && Test::alphaFont == &font);
            CHECK(c.memory.bkMode == OPAQUE && c.memory.saved.empty());
        }
        for (std::uint16_t bpp : {1,24,32}) { MemoTest::bitmap.bmBitsPixel = bpp; CHECK(c.Text(&c.memory) == 15); }
        MemoTest::haveBitmap = false; CHECK(c.Text(&c.memory) == 15);
    }
    void Conversion()
    {
        Context c; c.Begin();
        MemoTest::charsets[&c.dc] = 128;
        CHECK(c.Text(nullptr,"\x82\xa0!",3) == 15 && MemoTest::alphaText == L"\u3042!" && MemoTest::alphaCount == 2);
        CHECK(MemoTest::codepage == 932);
        CHECK(c.Text(nullptr,"\x82\xa0",-1) == 15 && MemoTest::alphaText == L"\u3042" && MemoTest::alphaCount == 1);
        MemoTest::charsets[&c.dc] = 204;
        CHECK(c.Text(nullptr,"\xc0\xe1",2) == 15 && MemoTest::alphaText == L"\u0410\u0431" && MemoTest::codepage == 1251);
        MemoTest::charsets[&c.dc] = DEFAULT_CHARSET;
        const char explicitBytes[]{'A',0,'B','Z'};
        CHECK(c.Text(nullptr,explicitBytes,3) == 15 && MemoTest::alphaText == std::wstring(L"A\0B",3));
        CHECK(MemoTest::codepage == CP_ACP);
        CHECK(c.Text(nullptr,explicitBytes,-1) == 15 && MemoTest::alphaText == L"A");
    }
    void Native()
    {
        Context c; c.Begin();
        const int alpha = Test::alphaText, black = Test::black;
        const char* text = "measure"; RECT r{1,2,3,4};
        CHECK(c.Text(nullptr,text,-1,DT_CALCRECT,&r) == 43 && r.right == 10);
        CHECK(MemoTest::ansiPointer == text && MemoTest::ansiRect == &r && MemoTest::ansiCount == -1);
        char mutableText[] = "change";
        CHECK(c.Text(nullptr,mutableText,-1,DT_MODIFYSTRING) == 43 && mutableText[0] == 'X');
        CHECK(c.Text(nullptr,text,0) == 43 && c.Text(nullptr,nullptr,3) == 43);
        CHECK(c.Text(nullptr,text,-2) == 43 && c.Text(nullptr,"",-1) == 43);
        CHECK(c.Text(nullptr,text,-1,DT_NOCLIP) == 43);
        RECT empty{}; CHECK(c.Text(nullptr,text,-1,kFlags,&empty) == 43);
        MemoTest::charsets[&c.dc] = SYMBOL_CHARSET; CHECK(c.Text() == 43);
        MemoTest::charsets[&c.dc] = 128; MemoTest::failTranslate = true; CHECK(c.Text() == 43);
        MemoTest::failTranslate = false; MemoTest::failConvert = true; CHECK(c.Text() == 43);
        CHECK(Test::alphaText == alpha && Test::black == black);
        CHECK(MemoTest::nativeMode == OPAQUE);
    }
    void Backgrounds()
    {
        Context c; c.Begin();
        for (COLORREF rgb : {MemoTest::windowColor,MemoTest::faceColor})
        {
            LOGBRUSH input{BS_SOLID,rgb};
            std::unique_ptr<LOGBRUSH> created(c.Brush(&input));
            CHECK(created && created.get() != &input && created->lbColor == 0 && created->lbStyle == BS_SOLID);
            CHECK(input.lbColor == rgb && MemoTest::brushPointer != &input);
            c.brush.lbColor = rgb; CHECK(c.Fill() == 1);
        }
        for (LOGBRUSH input : {LOGBRUSH{BS_SOLID,RGB(45,67,89)},LOGBRUSH{BS_SOLID,0},LOGBRUSH{3,0xffffff}})
        {
            std::unique_ptr<LOGBRUSH> created(c.Brush(&input));
            CHECK(created && created->lbStyle == input.lbStyle && created->lbColor == input.lbColor);
            CHECK(MemoTest::brushPointer == &input && c.Fill(nullptr,&input) == 37);
        }
        CHECK(!c.Brush(nullptr));
        CHECK(c.Fill(nullptr,reinterpret_cast<HBRUSH>(ULONG_PTR(COLOR_WINDOW+1))) == 1);
        CHECK(c.Fill(nullptr,reinterpret_cast<HBRUSH>(ULONG_PTR(9+1))) == 37);
        CHECK(c.Color(9) == MemoSysColor(9));
        MemoTest::windowColor = RGB(20,30,40); MemoTest::faceColor = RGB(60,70,80);
        c.brush.lbColor = 0xffffff; CHECK(c.Fill() == 37); // white is now custom, not a hardcoded key
        c.brush.lbColor = MemoTest::windowColor; CHECK(c.Fill() == 1);
        Root::Root_OnEndPaint(&c.window,&c.module);
        CHECK(c.Color(COLOR_WINDOW) == MemoTest::windowColor && c.Fill() == 37);
    }
    void Opaque()
    {
        Context c; c.Begin(); c.dc.background = RGB(35,65,95);
        const int commits = MemoTest::commits, black = Test::black;
        CHECK(c.Text() == 15 && MemoTest::commits == commits+1 && MemoTest::normalized == 1);
        const auto p = MemoTest::committed;
        CHECK(p.rgbRed == 35 && p.rgbGreen == 65 && p.rgbBlue == 95 && p.rgbReserved == 255);
        CHECK(Test::black == black && c.dc.background == RGB(35,65,95) && c.dc.bkMode == OPAQUE);
        c.dc.bkMode = TRANSPARENT;
        CHECK(c.Text() == 15 && MemoTest::commits == commits+1 && Test::black == black);
        c.dc.bkMode = OPAQUE; c.dc.background = 0;
        CHECK(c.Text() == 15 && Test::black == black+1 && MemoTest::commits == commits+1);
    }
    void Isolation()
    {
        Context c; c.Begin();
        const int alpha = Test::alphaText;
        CHECK(c.Text(&c.unknown) == 43 && c.Text(&c.foreign) == 43);
        Test::stackOwners[&c.unknown] = &c.other; CHECK(c.Text(&c.unknown) == 43);
        Test::stackOwners.erase(&c.memory); CHECK(c.Text(&c.memory) == 43);
        Test::stackOwners[&c.memory] = &c.window;
        MemoTest::maps[&c.dc] = 2; CHECK(c.Text() == 43); MemoTest::maps.clear();
        Test::topWindow = &c.other; CHECK(c.Text() == 43); Test::topWindow = &c.window;
        Test::host = &c.other; CHECK(c.Text() == 43); Test::host = &c.window;
        Test::full = false; CHECK(c.Text() == 43 && c.Color(COLOR_WINDOW) == MemoTest::windowColor); Test::full = true;
        c.window.backdrop = false; CHECK(c.Text() == 43 && c.Fill() == 37); c.window.backdrop = true;
        CHECK(Test::alphaText == alpha && c.Text(&c.memory) == 15);
    }
    void Failures()
    {
        Context c; c.Begin(); TestFont font{57}; c.dc.font = &font;
        const auto state = static_cast<DcState&>(c.dc);
        MemoTest::failSave = true; CHECK(c.Text() == 43); MemoTest::failSave = false;
        MemoTest::failBlack = true; CHECK(c.Text() == 43 && c.Fill() == 37); MemoTest::failBlack = false;
        PAINTSTRUCT ps{&c.dc,TRUE,c.rect};
        MemoTest::failBlack = true; CHECK(!Root::Root_OnRestoratorMemoAfterBeginPaint(&c.window,&c.dc,&ps,&c.module)); MemoTest::failBlack = false;
        for (bool* fail : {&MemoTest::failBrush,&MemoTest::failBuffer,&MemoTest::failFlush,&MemoTest::failAlpha})
        {
            c.dc.background = RGB(20,40,60); *fail = true;
            CHECK(c.Text() == 43 && MemoTest::nativeMode == OPAQUE && MemoTest::nativeBackground == c.dc.background);
            CHECK(c.dc.saved.empty()); *fail = false;
        }
        c.dc.background = state.background;
        Test::alphaResult = E_FAIL;
        CHECK(c.Text() == 43 && MemoTest::nativeMode == OPAQUE && MemoTest::nativeBackground == state.background);
        Test::alphaResult = S_OK;
        Test::duringAlpha = [&] { c.dc.text = 0x123456; c.dc.font = nullptr; throw std::runtime_error("renderer"); };
        CHECK(c.Text() == 43 && MemoTest::nativeMode == OPAQUE);
        Test::duringAlpha = {};
        CHECK(c.dc.text == state.text && c.dc.font == &font && c.dc.saved.empty());
        int reentry = 0;
        Test::duringAlpha = [&] { CHECK(c.Text() == 43); ++reentry; };
        CHECK(c.Text() == 15 && reentry == 1); Test::duringAlpha = {};
        CHECK(c.Text() == 15 && c.dc.bkMode == OPAQUE);
    }
    void Nesting()
    {
        Context c; c.Begin();
        Root::Root_OnGetDC(&c.gutter,&c.module); c.Activate(&c.gutter,&c.gutterDc);
        for (size_t i=0;i<4;++i) CHECK(c.Ref(i) == 2);
        CHECK(c.Text(&c.gutterDc) == 15 && c.Text() == 43);
        Root::Root_OnEndPaint(&c.window,&c.module); // remove lower Paint, keep upper GetDC
        CHECK(c.Ref() == 1 && c.Text(&c.gutterDc) == 15);
        Root::Root_OnEndPaint(&c.window,&c.module); CHECK(c.Ref() == 1); // duplicate/no-open close
        Root::Root_OnReleaseDC(&c.gutter,&c.module); CHECK(c.module.hooks.empty());
        // The actual PaintDispatcher failure branch compensates onBegin with onEnd.
        Root::Root_OnBeginPaint(&c.window,&c.module); CHECK(c.Ref() == 1);
        Root::Root_OnEndPaint(&c.window,&c.module);
        CHECK(c.module.hooks.empty() && Root::g_rootPaintStack.empty() && Root::g_rootDcStack.empty());
        CHECK(c.Text() == 43);
    }
    void Lifecycle()
    {
        Context c; c.Begin();
        const int native = MemoTest::chainAnsi;
        std::thread otherThread([&] { CHECK(c.Text() == 43); }); otherThread.join();
        CHECK(MemoTest::chainAnsi == native+1 && c.Text() == 15);
        Root::g_shutdown = true;
        CHECK(c.Text() == 43 && c.Color(COLOR_WINDOW) == MemoTest::windowColor && c.Fill() == 37);
        LOGBRUSH input{BS_SOLID,0xffffff};
        { std::unique_ptr<LOGBRUSH> result(c.Brush(&input)); CHECK(result && result->lbColor == 0xffffff); }
        Root::g_shutdown = false;
        RestoratorMemo::ClearSessions(); Root::DisableAllRootHooks();
        CHECK(c.module.hooks.empty() && c.Text() == 43);
        // No original in a slot: thunk's explicit Win32 fallback, no guessed caller.
        CHECK(Root::Root_MyRestoratorMemoText(&c.dc,"native",6,&c.rect,kFlags) == 43);
        CHECK(Root::Root_MyRestoratorMemoColor(COLOR_WINDOW) == MemoTest::windowColor);
        CHECK(Root::Root_MyRestoratorMemoFill(&c.dc,&c.rect,&c.brush) == 37);
        { std::unique_ptr<LOGBRUSH> result(Root::Root_MyRestoratorMemoBrush(&input)); CHECK(result && result->lbColor == 0xffffff); }
    }
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::pair<const char*,void(*)()> cases[]{
        {"classes",Classes},{"transaction",Transaction},{"paint",Paint},{"getdc",GetDc},
        {"text",TextLines},{"conversion",Conversion},{"native",Native},{"backgrounds",Backgrounds},
        {"opaque",Opaque},{"isolation",Isolation},{"failures",Failures},{"nesting",Nesting},{"lifecycle",Lifecycle}};
    try { for (const auto& [name,run] : cases) if (std::strcmp(argv[1],name) == 0) { run(); return 0; } }
    catch (const std::exception& error) { std::cerr << argv[1] << ": " << error.what() << '\n'; return 1; }
    return 2;
}
